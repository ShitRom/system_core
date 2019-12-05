/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "property_service.h"

#include <android/api-level.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <wchar.h>

#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <vector>

#include <android-base/chrono_utils.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <property_info_parser/property_info_parser.h>
#include <property_info_serializer/property_info_serializer.h>
#include <selinux/android.h>
#include <selinux/label.h>
#include <selinux/selinux.h>

#include "debug_ramdisk.h"
#include "epoll.h"
#include "init.h"
#include "persistent_properties.h"
#include "property_type.h"
#include "proto_utils.h"
#include "selinux.h"
#include "subcontext.h"
#include "system/core/init/property_service.pb.h"
#include "util.h"

using namespace std::literals;

using android::base::GetProperty;
using android::base::ReadFileToString;
using android::base::Split;
using android::base::StartsWith;
using android::base::StringPrintf;
using android::base::Timer;
using android::base::Trim;
using android::base::unique_fd;
using android::base::WriteStringToFile;
using android::properties::BuildTrie;
using android::properties::ParsePropertyInfoFile;
using android::properties::PropertyInfoAreaFile;
using android::properties::PropertyInfoEntry;

namespace android {
namespace init {

static bool persistent_properties_loaded = false;

static int property_set_fd = -1;
static int init_socket = -1;
static bool accept_messages = false;

static PropertyInfoAreaFile property_info_area;

struct PropertyAuditData {
    const ucred* cr;
    const char* name;
};

static int PropertyAuditCallback(void* data, security_class_t /*cls*/, char* buf, size_t len) {
    auto* d = reinterpret_cast<PropertyAuditData*>(data);

    if (!d || !d->name || !d->cr) {
        LOG(ERROR) << "AuditCallback invoked with null data arguments!";
        return 0;
    }

    snprintf(buf, len, "property=%s pid=%d uid=%d gid=%d", d->name, d->cr->pid, d->cr->uid,
             d->cr->gid);
    return 0;
}

bool CanReadProperty(const std::string& source_context, const std::string& name) {
    const char* target_context = nullptr;
    property_info_area->GetPropertyInfo(name.c_str(), &target_context, nullptr);

    PropertyAuditData audit_data;

    audit_data.name = name.c_str();

    ucred cr = {.pid = 0, .uid = 0, .gid = 0};
    audit_data.cr = &cr;

    return selinux_check_access(source_context.c_str(), target_context, "file", "read",
                                &audit_data) == 0;
}

static bool CheckMacPerms(const std::string& name, const char* target_context,
                          const char* source_context, const ucred& cr) {
    if (!target_context || !source_context) {
        return false;
    }

    PropertyAuditData audit_data;

    audit_data.name = name.c_str();
    audit_data.cr = &cr;

    bool has_access = (selinux_check_access(source_context, target_context, "property_service",
                                            "set", &audit_data) == 0);

    return has_access;
}

static void SendPropertyChanged(const std::string& name, const std::string& value) {
    auto property_msg = PropertyMessage{};
    auto* changed_message = property_msg.mutable_changed_message();
    changed_message->set_name(name);
    changed_message->set_value(value);

    if (auto result = SendMessage(init_socket, property_msg); !result) {
        LOG(ERROR) << "Failed to send property changed message: " << result.error();
    }
}

static uint32_t PropertySet(const std::string& name, const std::string& value, std::string* error) {
    size_t valuelen = value.size();

    if (!IsLegalPropertyName(name)) {
        *error = "Illegal property name";
        return PROP_ERROR_INVALID_NAME;
    }

    if (auto result = IsLegalPropertyValue(name, value); !result) {
        *error = result.error().message();
        return PROP_ERROR_INVALID_VALUE;
    }

    prop_info* pi = (prop_info*) __system_property_find(name.c_str());
    if (pi != nullptr) {
        // ro.* properties are actually "write-once".
        if (StartsWith(name, "ro.")) {
            *error = "Read-only property was already set";
            return PROP_ERROR_READ_ONLY_PROPERTY;
        }

        __system_property_update(pi, value.c_str(), valuelen);
    } else {
        int rc = __system_property_add(name.c_str(), name.size(), value.c_str(), valuelen);
        if (rc < 0) {
            *error = "__system_property_add failed";
            return PROP_ERROR_SET_FAILED;
        }
    }

    // Don't write properties to disk until after we have read all default
    // properties to prevent them from being overwritten by default values.
    if (persistent_properties_loaded && StartsWith(name, "persist.")) {
        WritePersistentProperty(name, value);
    }
    // If init hasn't started its main loop, then it won't be handling property changed messages
    // anyway, so there's no need to try to send them.
    if (accept_messages) {
        SendPropertyChanged(name, value);
    }
    return PROP_SUCCESS;
}

class AsyncRestorecon {
  public:
    void TriggerRestorecon(const std::string& path) {
        auto guard = std::lock_guard{mutex_};
        paths_.emplace(path);

        if (!thread_started_) {
            thread_started_ = true;
            std::thread{&AsyncRestorecon::ThreadFunction, this}.detach();
        }
    }

  private:
    void ThreadFunction() {
        auto lock = std::unique_lock{mutex_};

        while (!paths_.empty()) {
            auto path = paths_.front();
            paths_.pop();

            lock.unlock();
            if (selinux_android_restorecon(path.c_str(), SELINUX_ANDROID_RESTORECON_RECURSE) != 0) {
                LOG(ERROR) << "Asynchronous restorecon of '" << path << "' failed'";
            }
            android::base::SetProperty(kRestoreconProperty, path);
            lock.lock();
        }

        thread_started_ = false;
    }

    std::mutex mutex_;
    std::queue<std::string> paths_;
    bool thread_started_ = false;
};

class SocketConnection {
  public:
    SocketConnection(int socket, const ucred& cred) : socket_(socket), cred_(cred) {}

    bool RecvUint32(uint32_t* value, uint32_t* timeout_ms) {
        return RecvFully(value, sizeof(*value), timeout_ms);
    }

    bool RecvChars(char* chars, size_t size, uint32_t* timeout_ms) {
        return RecvFully(chars, size, timeout_ms);
    }

    bool RecvString(std::string* value, uint32_t* timeout_ms) {
        uint32_t len = 0;
        if (!RecvUint32(&len, timeout_ms)) {
            return false;
        }

        if (len == 0) {
            *value = "";
            return true;
        }

        // http://b/35166374: don't allow init to make arbitrarily large allocations.
        if (len > 0xffff) {
            LOG(ERROR) << "sys_prop: RecvString asked to read huge string: " << len;
            errno = ENOMEM;
            return false;
        }

        std::vector<char> chars(len);
        if (!RecvChars(&chars[0], len, timeout_ms)) {
            return false;
        }

        *value = std::string(&chars[0], len);
        return true;
    }

    bool SendUint32(uint32_t value) {
        if (!socket_.ok()) {
            return true;
        }
        int result = TEMP_FAILURE_RETRY(send(socket_, &value, sizeof(value), 0));
        return result == sizeof(value);
    }

    bool GetSourceContext(std::string* source_context) const {
        char* c_source_context = nullptr;
        if (getpeercon(socket_, &c_source_context) != 0) {
            return false;
        }
        *source_context = c_source_context;
        freecon(c_source_context);
        return true;
    }

    [[nodiscard]] int Release() { return socket_.release(); }

    const ucred& cred() { return cred_; }

  private:
    bool PollIn(uint32_t* timeout_ms) {
        struct pollfd ufds[1];
        ufds[0].fd = socket_;
        ufds[0].events = POLLIN;
        ufds[0].revents = 0;
        while (*timeout_ms > 0) {
            auto start_time = std::chrono::steady_clock::now();
            int nr = poll(ufds, 1, *timeout_ms);
            auto now = std::chrono::steady_clock::now();
            auto time_elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time);
            uint64_t millis = time_elapsed.count();
            *timeout_ms = (millis > *timeout_ms) ? 0 : *timeout_ms - millis;

            if (nr > 0) {
                return true;
            }

            if (nr == 0) {
                // Timeout
                break;
            }

            if (nr < 0 && errno != EINTR) {
                PLOG(ERROR) << "sys_prop: error waiting for uid " << cred_.uid
                            << " to send property message";
                return false;
            } else {  // errno == EINTR
                // Timer rounds milliseconds down in case of EINTR we want it to be rounded up
                // to avoid slowing init down by causing EINTR with under millisecond timeout.
                if (*timeout_ms > 0) {
                    --(*timeout_ms);
                }
            }
        }

        LOG(ERROR) << "sys_prop: timeout waiting for uid " << cred_.uid
                   << " to send property message.";
        return false;
    }

    bool RecvFully(void* data_ptr, size_t size, uint32_t* timeout_ms) {
        size_t bytes_left = size;
        char* data = static_cast<char*>(data_ptr);
        while (*timeout_ms > 0 && bytes_left > 0) {
            if (!PollIn(timeout_ms)) {
                return false;
            }

            int result = TEMP_FAILURE_RETRY(recv(socket_, data, bytes_left, MSG_DONTWAIT));
            if (result <= 0) {
                PLOG(ERROR) << "sys_prop: recv error";
                return false;
            }

            bytes_left -= result;
            data += result;
        }

        if (bytes_left != 0) {
            LOG(ERROR) << "sys_prop: recv data is not properly obtained.";
        }

        return bytes_left == 0;
    }

    unique_fd socket_;
    ucred cred_;

    DISALLOW_IMPLICIT_CONSTRUCTORS(SocketConnection);
};

static uint32_t SendControlMessage(const std::string& msg, const std::string& name, pid_t pid,
                                   SocketConnection* socket, std::string* error) {
    if (!accept_messages) {
        *error = "Received control message after shutdown, ignoring";
        return PROP_ERROR_HANDLE_CONTROL_MESSAGE;
    }

    auto property_msg = PropertyMessage{};
    auto* control_message = property_msg.mutable_control_message();
    control_message->set_msg(msg);
    control_message->set_name(name);
    control_message->set_pid(pid);

    // We must release the fd before sending it to init, otherwise there will be a race with init.
    // If init calls close() before Release(), then fdsan will see the wrong tag and abort().
    int fd = -1;
    if (socket != nullptr && SelinuxGetVendorAndroidVersion() > __ANDROID_API_Q__) {
        fd = socket->Release();
        control_message->set_fd(fd);
    }

    if (auto result = SendMessage(init_socket, property_msg); !result) {
        // We've already released the fd above, so if we fail to send the message to init, we need
        // to manually free it here.
        if (fd != -1) {
            close(fd);
        }
        *error = "Failed to send control message: " + result.error().message();
        return PROP_ERROR_HANDLE_CONTROL_MESSAGE;
    }

    return PROP_SUCCESS;
}

bool CheckControlPropertyPerms(const std::string& name, const std::string& value,
                               const std::string& source_context, const ucred& cr) {
    // We check the legacy method first but these properties are dontaudit, so we only log an audit
    // if the newer method fails as well.  We only do this with the legacy ctl. properties.
    if (name == "ctl.start" || name == "ctl.stop" || name == "ctl.restart") {
        // The legacy permissions model is that ctl. properties have their name ctl.<action> and
        // their value is the name of the service to apply that action to.  Permissions for these
        // actions are based on the service, so we must create a fake name of ctl.<service> to
        // check permissions.
        auto control_string_legacy = "ctl." + value;
        const char* target_context_legacy = nullptr;
        const char* type_legacy = nullptr;
        property_info_area->GetPropertyInfo(control_string_legacy.c_str(), &target_context_legacy,
                                            &type_legacy);

        if (CheckMacPerms(control_string_legacy, target_context_legacy, source_context.c_str(), cr)) {
            return true;
        }
    }

    auto control_string_full = name + "$" + value;
    const char* target_context_full = nullptr;
    const char* type_full = nullptr;
    property_info_area->GetPropertyInfo(control_string_full.c_str(), &target_context_full,
                                        &type_full);

    return CheckMacPerms(control_string_full, target_context_full, source_context.c_str(), cr);
}

// This returns one of the enum of PROP_SUCCESS or PROP_ERROR*.
uint32_t CheckPermissions(const std::string& name, const std::string& value,
                          const std::string& source_context, const ucred& cr, std::string* error) {
    if (!IsLegalPropertyName(name)) {
        *error = "Illegal property name";
        return PROP_ERROR_INVALID_NAME;
    }

    if (StartsWith(name, "ctl.")) {
        if (!CheckControlPropertyPerms(name, value, source_context, cr)) {
            *error = StringPrintf("Invalid permissions to perform '%s' on '%s'", name.c_str() + 4,
                                  value.c_str());
            return PROP_ERROR_HANDLE_CONTROL_MESSAGE;
        }

        return PROP_SUCCESS;
    }

    const char* target_context = nullptr;
    const char* type = nullptr;
    property_info_area->GetPropertyInfo(name.c_str(), &target_context, &type);

    if (!CheckMacPerms(name, target_context, source_context.c_str(), cr)) {
        *error = "SELinux permission check failed";
        return PROP_ERROR_PERMISSION_DENIED;
    }

    if (!CheckType(type, value)) {
        *error = StringPrintf("Property type check failed, value doesn't match expected type '%s'",
                              (type ?: "(null)"));
        return PROP_ERROR_INVALID_VALUE;
    }

    return PROP_SUCCESS;
}

// This returns one of the enum of PROP_SUCCESS or PROP_ERROR*.
uint32_t HandlePropertySet(const std::string& name, const std::string& value,
                           const std::string& source_context, const ucred& cr,
                           SocketConnection* socket, std::string* error) {
    if (auto ret = CheckPermissions(name, value, source_context, cr, error); ret != PROP_SUCCESS) {
        return ret;
    }

    if (StartsWith(name, "ctl.")) {
        return SendControlMessage(name.c_str() + 4, value, cr.pid, socket, error);
    }

    // sys.powerctl is a special property that is used to make the device reboot.  We want to log
    // any process that sets this property to be able to accurately blame the cause of a shutdown.
    if (name == "sys.powerctl") {
        std::string cmdline_path = StringPrintf("proc/%d/cmdline", cr.pid);
        std::string process_cmdline;
        std::string process_log_string;
        if (ReadFileToString(cmdline_path, &process_cmdline)) {
            // Since cmdline is null deliminated, .c_str() conveniently gives us just the process
            // path.
            process_log_string = StringPrintf(" (%s)", process_cmdline.c_str());
        }
        LOG(INFO) << "Received sys.powerctl='" << value << "' from pid: " << cr.pid
                  << process_log_string;
    }

    // If a process other than init is writing a non-empty value, it means that process is
    // requesting that init performs a restorecon operation on the path specified by 'value'.
    // We use a thread to do this restorecon operation to prevent holding up init, as it may take
    // a long time to complete.
    if (name == kRestoreconProperty && cr.pid != 1 && !value.empty()) {
        static AsyncRestorecon async_restorecon;
        async_restorecon.TriggerRestorecon(value);
        return PROP_SUCCESS;
    }

    return PropertySet(name, value, error);
}

static void handle_property_set_fd() {
    static constexpr uint32_t kDefaultSocketTimeout = 2000; /* ms */

    int s = accept4(property_set_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (s == -1) {
        return;
    }

    ucred cr;
    socklen_t cr_size = sizeof(cr);
    if (getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cr, &cr_size) < 0) {
        close(s);
        PLOG(ERROR) << "sys_prop: unable to get SO_PEERCRED";
        return;
    }

    SocketConnection socket(s, cr);
    uint32_t timeout_ms = kDefaultSocketTimeout;

    uint32_t cmd = 0;
    if (!socket.RecvUint32(&cmd, &timeout_ms)) {
        PLOG(ERROR) << "sys_prop: error while reading command from the socket";
        socket.SendUint32(PROP_ERROR_READ_CMD);
        return;
    }

    switch (cmd) {
    case PROP_MSG_SETPROP: {
        char prop_name[PROP_NAME_MAX];
        char prop_value[PROP_VALUE_MAX];

        if (!socket.RecvChars(prop_name, PROP_NAME_MAX, &timeout_ms) ||
            !socket.RecvChars(prop_value, PROP_VALUE_MAX, &timeout_ms)) {
          PLOG(ERROR) << "sys_prop(PROP_MSG_SETPROP): error while reading name/value from the socket";
          return;
        }

        prop_name[PROP_NAME_MAX-1] = 0;
        prop_value[PROP_VALUE_MAX-1] = 0;

        std::string source_context;
        if (!socket.GetSourceContext(&source_context)) {
            PLOG(ERROR) << "Unable to set property '" << prop_name << "': getpeercon() failed";
            return;
        }

        const auto& cr = socket.cred();
        std::string error;
        uint32_t result =
                HandlePropertySet(prop_name, prop_value, source_context, cr, nullptr, &error);
        if (result != PROP_SUCCESS) {
            LOG(ERROR) << "Unable to set property '" << prop_name << "' from uid:" << cr.uid
                       << " gid:" << cr.gid << " pid:" << cr.pid << ": " << error;
        }

        break;
      }

    case PROP_MSG_SETPROP2: {
        std::string name;
        std::string value;
        if (!socket.RecvString(&name, &timeout_ms) ||
            !socket.RecvString(&value, &timeout_ms)) {
          PLOG(ERROR) << "sys_prop(PROP_MSG_SETPROP2): error while reading name/value from the socket";
          socket.SendUint32(PROP_ERROR_READ_DATA);
          return;
        }

        std::string source_context;
        if (!socket.GetSourceContext(&source_context)) {
            PLOG(ERROR) << "Unable to set property '" << name << "': getpeercon() failed";
            socket.SendUint32(PROP_ERROR_PERMISSION_DENIED);
            return;
        }

        const auto& cr = socket.cred();
        std::string error;
        uint32_t result = HandlePropertySet(name, value, source_context, cr, &socket, &error);
        if (result != PROP_SUCCESS) {
            LOG(ERROR) << "Unable to set property '" << name << "' from uid:" << cr.uid
                       << " gid:" << cr.gid << " pid:" << cr.pid << ": " << error;
        }
        socket.SendUint32(result);
        break;
      }

    default:
        LOG(ERROR) << "sys_prop: invalid command " << cmd;
        socket.SendUint32(PROP_ERROR_INVALID_CMD);
        break;
    }
}

uint32_t InitPropertySet(const std::string& name, const std::string& value) {
    uint32_t result = 0;
    ucred cr = {.pid = 1, .uid = 0, .gid = 0};
    std::string error;
    result = HandlePropertySet(name, value, kInitContext, cr, nullptr, &error);
    if (result != PROP_SUCCESS) {
        LOG(ERROR) << "Init cannot set '" << name << "' to '" << value << "': " << error;
    }

    return result;
}

static bool load_properties_from_file(const char*, const char*,
                                      std::map<std::string, std::string>*);

/*
 * Filter is used to decide which properties to load: NULL loads all keys,
 * "ro.foo.*" is a prefix match, and "ro.foo.bar" is an exact match.
 */
static void LoadProperties(char* data, const char* filter, const char* filename,
                           std::map<std::string, std::string>* properties) {
    char *key, *value, *eol, *sol, *tmp, *fn;
    size_t flen = 0;

    static constexpr const char* const kVendorPathPrefixes[2] = {
            "/vendor",
            "/odm",
    };

    const char* context = kInitContext;
    if (SelinuxGetVendorAndroidVersion() >= __ANDROID_API_P__) {
        for (const auto& vendor_path_prefix : kVendorPathPrefixes) {
            if (StartsWith(filename, vendor_path_prefix)) {
                context = kVendorContext;
            }
        }
    }

    if (filter) {
        flen = strlen(filter);
    }

    sol = data;
    while ((eol = strchr(sol, '\n'))) {
        key = sol;
        *eol++ = 0;
        sol = eol;

        while (isspace(*key)) key++;
        if (*key == '#') continue;

        tmp = eol - 2;
        while ((tmp > key) && isspace(*tmp)) *tmp-- = 0;

        if (!strncmp(key, "import ", 7) && flen == 0) {
            fn = key + 7;
            while (isspace(*fn)) fn++;

            key = strchr(fn, ' ');
            if (key) {
                *key++ = 0;
                while (isspace(*key)) key++;
            }

            std::string raw_filename(fn);
            auto expanded_filename = ExpandProps(raw_filename);

            if (!expanded_filename) {
                LOG(ERROR) << "Could not expand filename ': " << expanded_filename.error();
                continue;
            }

            load_properties_from_file(expanded_filename->c_str(), key, properties);
        } else {
            value = strchr(key, '=');
            if (!value) continue;
            *value++ = 0;

            tmp = value - 2;
            while ((tmp > key) && isspace(*tmp)) *tmp-- = 0;

            while (isspace(*value)) value++;

            if (flen > 0) {
                if (filter[flen - 1] == '*') {
                    if (strncmp(key, filter, flen - 1) != 0) continue;
                } else {
                    if (strcmp(key, filter) != 0) continue;
                }
            }

            if (StartsWith(key, "ctl.") || key == "sys.powerctl"s ||
                std::string{key} == kRestoreconProperty) {
                LOG(ERROR) << "Ignoring disallowed property '" << key
                           << "' with special meaning in prop file '" << filename << "'";
                continue;
            }

            ucred cr = {.pid = 1, .uid = 0, .gid = 0};
            std::string error;
            if (CheckPermissions(key, value, context, cr, &error) == PROP_SUCCESS) {
                auto it = properties->find(key);
                if (it == properties->end()) {
                    (*properties)[key] = value;
                } else if (it->second != value) {
                    LOG(WARNING) << "Overriding previous 'ro.' property '" << key << "':'"
                                 << it->second << "' with new value '" << value << "'";
                    it->second = value;
                }
            } else {
                LOG(ERROR) << "Do not have permissions to set '" << key << "' to '" << value
                           << "' in property file '" << filename << "': " << error;
            }
        }
    }
}

// Filter is used to decide which properties to load: NULL loads all keys,
// "ro.foo.*" is a prefix match, and "ro.foo.bar" is an exact match.
static bool load_properties_from_file(const char* filename, const char* filter,
                                      std::map<std::string, std::string>* properties) {
    Timer t;
    auto file_contents = ReadFile(filename);
    if (!file_contents) {
        PLOG(WARNING) << "Couldn't load property file '" << filename
                      << "': " << file_contents.error();
        return false;
    }
    file_contents->push_back('\n');

    LoadProperties(file_contents->data(), filter, filename, properties);
    LOG(VERBOSE) << "(Loading properties from " << filename << " took " << t << ".)";
    return true;
}

// persist.sys.usb.config values can't be combined on build-time when property
// files are split into each partition.
// So we need to apply the same rule of build/make/tools/post_process_props.py
// on runtime.
static void update_sys_usb_config() {
    bool is_debuggable = android::base::GetBoolProperty("ro.debuggable", false);
    std::string config = android::base::GetProperty("persist.sys.usb.config", "");
    if (config.empty()) {
        InitPropertySet("persist.sys.usb.config", is_debuggable ? "adb" : "none");
    } else if (is_debuggable && config.find("adb") == std::string::npos &&
               config.length() + 4 < PROP_VALUE_MAX) {
        config.append(",adb");
        InitPropertySet("persist.sys.usb.config", config);
    }
}

static void load_override_properties() {
    if (ALLOW_LOCAL_PROP_OVERRIDE) {
        std::map<std::string, std::string> properties;
        load_properties_from_file("/data/local.prop", nullptr, &properties);
        for (const auto& [name, value] : properties) {
            std::string error;
            if (PropertySet(name, value, &error) != PROP_SUCCESS) {
                LOG(ERROR) << "Could not set '" << name << "' to '" << value
                           << "' in /data/local.prop: " << error;
            }
        }
    }
}

// If the ro.product.[brand|device|manufacturer|model|name] properties have not been explicitly
// set, derive them from ro.product.${partition}.* properties
static void property_initialize_ro_product_props() {
    const char* RO_PRODUCT_PROPS_PREFIX = "ro.product.";
    const char* RO_PRODUCT_PROPS[] = {
            "brand", "device", "manufacturer", "model", "name",
    };
    const char* RO_PRODUCT_PROPS_ALLOWED_SOURCES[] = {
            "odm", "product", "system_ext", "system", "vendor",
    };
    const char* RO_PRODUCT_PROPS_DEFAULT_SOURCE_ORDER = "product,odm,vendor,system_ext,system";
    const std::string EMPTY = "";

    std::string ro_product_props_source_order =
            GetProperty("ro.product.property_source_order", EMPTY);

    if (!ro_product_props_source_order.empty()) {
        // Verify that all specified sources are valid
        for (const auto& source : Split(ro_product_props_source_order, ",")) {
            // Verify that the specified source is valid
            bool is_allowed_source = false;
            for (const auto& allowed_source : RO_PRODUCT_PROPS_ALLOWED_SOURCES) {
                if (source == allowed_source) {
                    is_allowed_source = true;
                    break;
                }
            }
            if (!is_allowed_source) {
                LOG(ERROR) << "Found unexpected source in ro.product.property_source_order; "
                              "using the default property source order";
                ro_product_props_source_order = RO_PRODUCT_PROPS_DEFAULT_SOURCE_ORDER;
                break;
            }
        }
    } else {
        ro_product_props_source_order = RO_PRODUCT_PROPS_DEFAULT_SOURCE_ORDER;
    }

    for (const auto& ro_product_prop : RO_PRODUCT_PROPS) {
        std::string base_prop(RO_PRODUCT_PROPS_PREFIX);
        base_prop += ro_product_prop;

        std::string base_prop_val = GetProperty(base_prop, EMPTY);
        if (!base_prop_val.empty()) {
            continue;
        }

        for (const auto& source : Split(ro_product_props_source_order, ",")) {
            std::string target_prop(RO_PRODUCT_PROPS_PREFIX);
            target_prop += source;
            target_prop += '.';
            target_prop += ro_product_prop;

            std::string target_prop_val = GetProperty(target_prop, EMPTY);
            if (!target_prop_val.empty()) {
                LOG(INFO) << "Setting product property " << base_prop << " to '" << target_prop_val
                          << "' (from " << target_prop << ")";
                std::string error;
                uint32_t res = PropertySet(base_prop, target_prop_val, &error);
                if (res != PROP_SUCCESS) {
                    LOG(ERROR) << "Error setting product property " << base_prop << ": err=" << res
                               << " (" << error << ")";
                }
                break;
            }
        }
    }
}

// If the ro.build.fingerprint property has not been set, derive it from constituent pieces
static void property_derive_build_fingerprint() {
    std::string build_fingerprint = GetProperty("ro.build.fingerprint", "");
    if (!build_fingerprint.empty()) {
        return;
    }

    const std::string UNKNOWN = "unknown";
    build_fingerprint = GetProperty("ro.product.brand", UNKNOWN);
    build_fingerprint += '/';
    build_fingerprint += GetProperty("ro.product.name", UNKNOWN);
    build_fingerprint += '/';
    build_fingerprint += GetProperty("ro.product.device", UNKNOWN);
    build_fingerprint += ':';
    build_fingerprint += GetProperty("ro.build.version.release", UNKNOWN);
    build_fingerprint += '/';
    build_fingerprint += GetProperty("ro.build.id", UNKNOWN);
    build_fingerprint += '/';
    build_fingerprint += GetProperty("ro.build.version.incremental", UNKNOWN);
    build_fingerprint += ':';
    build_fingerprint += GetProperty("ro.build.type", UNKNOWN);
    build_fingerprint += '/';
    build_fingerprint += GetProperty("ro.build.tags", UNKNOWN);

    LOG(INFO) << "Setting property 'ro.build.fingerprint' to '" << build_fingerprint << "'";

    std::string error;
    uint32_t res = PropertySet("ro.build.fingerprint", build_fingerprint, &error);
    if (res != PROP_SUCCESS) {
        LOG(ERROR) << "Error setting property 'ro.build.fingerprint': err=" << res << " (" << error
                   << ")";
    }
}

void PropertyLoadBootDefaults() {
    // TODO(b/117892318): merge prop.default and build.prop files into one
    // We read the properties and their values into a map, in order to always allow properties
    // loaded in the later property files to override the properties in loaded in the earlier
    // property files, regardless of if they are "ro." properties or not.
    std::map<std::string, std::string> properties;
    if (!load_properties_from_file("/system/etc/prop.default", nullptr, &properties)) {
        // Try recovery path
        if (!load_properties_from_file("/prop.default", nullptr, &properties)) {
            // Try legacy path
            load_properties_from_file("/default.prop", nullptr, &properties);
        }
    }
    load_properties_from_file("/system/build.prop", nullptr, &properties);
    load_properties_from_file("/system_ext/build.prop", nullptr, &properties);
    load_properties_from_file("/vendor/default.prop", nullptr, &properties);
    load_properties_from_file("/vendor/build.prop", nullptr, &properties);
    if (SelinuxGetVendorAndroidVersion() >= __ANDROID_API_Q__) {
        load_properties_from_file("/odm/etc/build.prop", nullptr, &properties);
    } else {
        load_properties_from_file("/odm/default.prop", nullptr, &properties);
        load_properties_from_file("/odm/build.prop", nullptr, &properties);
    }
    load_properties_from_file("/product/build.prop", nullptr, &properties);
    load_properties_from_file("/factory/factory.prop", "ro.*", &properties);

    if (access(kDebugRamdiskProp, R_OK) == 0) {
        LOG(INFO) << "Loading " << kDebugRamdiskProp;
        load_properties_from_file(kDebugRamdiskProp, nullptr, &properties);
    }

    for (const auto& [name, value] : properties) {
        std::string error;
        if (PropertySet(name, value, &error) != PROP_SUCCESS) {
            LOG(ERROR) << "Could not set '" << name << "' to '" << value
                       << "' while loading .prop files" << error;
        }
    }

    property_initialize_ro_product_props();
    property_derive_build_fingerprint();

    update_sys_usb_config();
}

bool LoadPropertyInfoFromFile(const std::string& filename,
                              std::vector<PropertyInfoEntry>* property_infos) {
    auto file_contents = std::string();
    if (!ReadFileToString(filename, &file_contents)) {
        PLOG(ERROR) << "Could not read properties from '" << filename << "'";
        return false;
    }

    auto errors = std::vector<std::string>{};
    ParsePropertyInfoFile(file_contents, property_infos, &errors);
    // Individual parsing errors are reported but do not cause a failed boot, which is what
    // returning false would do here.
    for (const auto& error : errors) {
        LOG(ERROR) << "Could not read line from '" << filename << "': " << error;
    }

    return true;
}

void CreateSerializedPropertyInfo() {
    auto property_infos = std::vector<PropertyInfoEntry>();
    if (access("/system/etc/selinux/plat_property_contexts", R_OK) != -1) {
        if (!LoadPropertyInfoFromFile("/system/etc/selinux/plat_property_contexts",
                                      &property_infos)) {
            return;
        }
        // Don't check for failure here, so we always have a sane list of properties.
        // E.g. In case of recovery, the vendor partition will not have mounted and we
        // still need the system / platform properties to function.
        if (access("/system_ext/etc/selinux/system_ext_property_contexts", R_OK) != -1) {
            LoadPropertyInfoFromFile("/system_ext/etc/selinux/system_ext_property_contexts",
                                     &property_infos);
        }
        if (!LoadPropertyInfoFromFile("/vendor/etc/selinux/vendor_property_contexts",
                                      &property_infos)) {
            // Fallback to nonplat_* if vendor_* doesn't exist.
            LoadPropertyInfoFromFile("/vendor/etc/selinux/nonplat_property_contexts",
                                     &property_infos);
        }
        if (access("/product/etc/selinux/product_property_contexts", R_OK) != -1) {
            LoadPropertyInfoFromFile("/product/etc/selinux/product_property_contexts",
                                     &property_infos);
        }
        if (access("/odm/etc/selinux/odm_property_contexts", R_OK) != -1) {
            LoadPropertyInfoFromFile("/odm/etc/selinux/odm_property_contexts", &property_infos);
        }
    } else {
        if (!LoadPropertyInfoFromFile("/plat_property_contexts", &property_infos)) {
            return;
        }
        LoadPropertyInfoFromFile("/system_ext_property_contexts", &property_infos);
        if (!LoadPropertyInfoFromFile("/vendor_property_contexts", &property_infos)) {
            // Fallback to nonplat_* if vendor_* doesn't exist.
            LoadPropertyInfoFromFile("/nonplat_property_contexts", &property_infos);
        }
        LoadPropertyInfoFromFile("/product_property_contexts", &property_infos);
        LoadPropertyInfoFromFile("/odm_property_contexts", &property_infos);
    }

    auto serialized_contexts = std::string();
    auto error = std::string();
    if (!BuildTrie(property_infos, "u:object_r:default_prop:s0", "string", &serialized_contexts,
                   &error)) {
        LOG(ERROR) << "Unable to serialize property contexts: " << error;
        return;
    }

    constexpr static const char kPropertyInfosPath[] = "/dev/__properties__/property_info";
    if (!WriteStringToFile(serialized_contexts, kPropertyInfosPath, 0444, 0, 0, false)) {
        PLOG(ERROR) << "Unable to write serialized property infos to file";
    }
    selinux_android_restorecon(kPropertyInfosPath, 0);
}

static void ExportKernelBootProps() {
    constexpr const char* UNSET = "";
    struct {
        const char* src_prop;
        const char* dst_prop;
        const char* default_value;
    } prop_map[] = {
            // clang-format off
        { "ro.boot.serialno",   "ro.serialno",   UNSET, },
        { "ro.boot.mode",       "ro.bootmode",   "unknown", },
        { "ro.boot.baseband",   "ro.baseband",   "unknown", },
        { "ro.boot.bootloader", "ro.bootloader", "unknown", },
        { "ro.boot.hardware",   "ro.hardware",   "unknown", },
        { "ro.boot.revision",   "ro.revision",   "0", },
            // clang-format on
    };
    for (const auto& prop : prop_map) {
        std::string value = GetProperty(prop.src_prop, prop.default_value);
        if (value != UNSET) InitPropertySet(prop.dst_prop, value);
    }
}

static void ProcessKernelDt() {
    if (!is_android_dt_value_expected("compatible", "android,firmware")) {
        return;
    }

    std::unique_ptr<DIR, int (*)(DIR*)> dir(opendir(get_android_dt_dir().c_str()), closedir);
    if (!dir) return;

    std::string dt_file;
    struct dirent* dp;
    while ((dp = readdir(dir.get())) != NULL) {
        if (dp->d_type != DT_REG || !strcmp(dp->d_name, "compatible") ||
            !strcmp(dp->d_name, "name")) {
            continue;
        }

        std::string file_name = get_android_dt_dir() + dp->d_name;

        android::base::ReadFileToString(file_name, &dt_file);
        std::replace(dt_file.begin(), dt_file.end(), ',', '.');

        InitPropertySet("ro.boot."s + dp->d_name, dt_file);
    }
}

static void ProcessKernelCmdline() {
    bool for_emulator = false;
    ImportKernelCmdline([&](const std::string& key, const std::string& value) {
        if (key == "qemu") {
            for_emulator = true;
        } else if (StartsWith(key, "androidboot.")) {
            InitPropertySet("ro.boot." + key.substr(12), value);
        }
    });

    if (for_emulator) {
        ImportKernelCmdline([&](const std::string& key, const std::string& value) {
            // In the emulator, export any kernel option with the "ro.kernel." prefix.
            InitPropertySet("ro.kernel." + key, value);
        });
    }
}

void PropertyInit() {
    selinux_callback cb;
    cb.func_audit = PropertyAuditCallback;
    selinux_set_callback(SELINUX_CB_AUDIT, cb);

    mkdir("/dev/__properties__", S_IRWXU | S_IXGRP | S_IXOTH);
    CreateSerializedPropertyInfo();
    if (__system_property_area_init()) {
        LOG(FATAL) << "Failed to initialize property area";
    }
    if (!property_info_area.LoadDefaultPath()) {
        LOG(FATAL) << "Failed to load serialized property info file";
    }

    // If arguments are passed both on the command line and in DT,
    // properties set in DT always have priority over the command-line ones.
    ProcessKernelDt();
    ProcessKernelCmdline();

    // Propagate the kernel variables to internal variables
    // used by init as well as the current required properties.
    ExportKernelBootProps();

    PropertyLoadBootDefaults();
}

static void HandleInitSocket() {
    auto message = ReadMessage(init_socket);
    if (!message) {
        LOG(ERROR) << "Could not read message from init_dedicated_recv_socket: " << message.error();
        return;
    }

    auto init_message = InitMessage{};
    if (!init_message.ParseFromString(*message)) {
        LOG(ERROR) << "Could not parse message from init";
        return;
    }

    switch (init_message.msg_case()) {
        case InitMessage::kLoadPersistentProperties: {
            load_override_properties();
            // Read persistent properties after all default values have been loaded.
            auto persistent_properties = LoadPersistentProperties();
            for (const auto& persistent_property_record : persistent_properties.properties()) {
                InitPropertySet(persistent_property_record.name(),
                                persistent_property_record.value());
            }
            InitPropertySet("ro.persistent_properties.ready", "true");
            persistent_properties_loaded = true;
            break;
        }
        case InitMessage::kStopSendingMessages: {
            accept_messages = false;
            break;
        }
        case InitMessage::kStartSendingMessages: {
            accept_messages = true;
            break;
        }
        default:
            LOG(ERROR) << "Unknown message type from init: " << init_message.msg_case();
    }
}

static void PropertyServiceThread() {
    Epoll epoll;
    if (auto result = epoll.Open(); !result) {
        LOG(FATAL) << result.error();
    }

    if (auto result = epoll.RegisterHandler(property_set_fd, handle_property_set_fd); !result) {
        LOG(FATAL) << result.error();
    }

    if (auto result = epoll.RegisterHandler(init_socket, HandleInitSocket); !result) {
        LOG(FATAL) << result.error();
    }

    while (true) {
        auto pending_functions = epoll.Wait(std::nullopt);
        if (!pending_functions) {
            LOG(ERROR) << pending_functions.error();
        } else {
            for (const auto& function : *pending_functions) {
                (*function)();
            }
        }
    }
}

void StartPropertyService(int* epoll_socket) {
    InitPropertySet("ro.property_service.version", "2");

    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
        PLOG(FATAL) << "Failed to socketpair() between property_service and init";
    }
    *epoll_socket = sockets[0];
    init_socket = sockets[1];
    accept_messages = true;

    if (auto result = CreateSocket(PROP_SERVICE_NAME, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                                   false, 0666, 0, 0, {})) {
        property_set_fd = *result;
    } else {
        LOG(FATAL) << "start_property_service socket creation failed: " << result.error();
    }

    listen(property_set_fd, 8);

    std::thread{PropertyServiceThread}.detach();
}

}  // namespace init
}  // namespace android
