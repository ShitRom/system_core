/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "commands.h"

#include <sys/socket.h>
#include <sys/un.h>

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <cutils/android_reboot.h>

#include "constants.h"
#include "fastboot_device.h"
#include "utility.h"

using ::android::hardware::hidl_string;
using ::android::hardware::boot::V1_0::BoolResult;
using ::android::hardware::boot::V1_0::CommandResult;
using ::android::hardware::boot::V1_0::Slot;

bool GetVarHandler(FastbootDevice* device, const std::vector<std::string>& args) {
    using VariableHandler =
            std::function<std::string(FastbootDevice*, const std::vector<std::string>&)>;
    const std::unordered_map<std::string, VariableHandler> kVariableMap = {
            {FB_VAR_VERSION, GetVersion},
            {FB_VAR_VERSION_BOOTLOADER, GetBootloaderVersion},
            {FB_VAR_VERSION_BASEBAND, GetBasebandVersion},
            {FB_VAR_PRODUCT, GetProduct},
            {FB_VAR_SERIALNO, GetSerial},
            {FB_VAR_SECURE, GetSecure},
            {FB_VAR_UNLOCKED, GetUnlocked},
            {FB_VAR_MAX_DOWNLOAD_SIZE, GetMaxDownloadSize},
            {FB_VAR_CURRENT_SLOT, ::GetCurrentSlot},
            {FB_VAR_SLOT_COUNT, GetSlotCount},
            {FB_VAR_HAS_SLOT, GetHasSlot},
            {FB_VAR_SLOT_SUCCESSFUL, GetSlotSuccessful},
            {FB_VAR_SLOT_UNBOOTABLE, GetSlotUnbootable}};

    // args[0] is command name, args[1] is variable.
    auto found_variable = kVariableMap.find(args[1]);
    if (found_variable == kVariableMap.end()) {
        return device->WriteStatus(FastbootResult::FAIL, "Unknown variable");
    }

    std::vector<std::string> getvar_args(args.begin() + 2, args.end());
    auto result = found_variable->second(device, getvar_args);
    return device->WriteStatus(FastbootResult::OKAY, result);
}

bool DownloadHandler(FastbootDevice* device, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        return device->WriteStatus(FastbootResult::FAIL, "size argument unspecified");
    }
    // arg[0] is the command name, arg[1] contains size of data to be downloaded
    unsigned int size;
    if (!android::base::ParseUint("0x" + args[1], &size, UINT_MAX)) {
        return device->WriteStatus(FastbootResult::FAIL, "Invalid size");
    }
    device->get_download_data().resize(size);
    if (!device->WriteStatus(FastbootResult::DATA, android::base::StringPrintf("%08x", size))) {
        return false;
    }

    if (device->HandleData(true, &device->get_download_data())) {
        return device->WriteStatus(FastbootResult::OKAY, "");
    }

    PLOG(ERROR) << "Couldn't download data";
    return device->WriteStatus(FastbootResult::FAIL, "Couldn't download data");
}

bool SetActiveHandler(FastbootDevice* device, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        return device->WriteStatus(FastbootResult::FAIL, "Missing slot argument");
    }

    // Slot suffix needs to be between 'a' and 'z'.
    Slot slot;
    if (!GetSlotNumber(args[1], &slot)) {
        return device->WriteStatus(FastbootResult::FAIL, "Bad slot suffix");
    }

    // Non-A/B devices will not have a boot control HAL.
    auto boot_control_hal = device->boot_control_hal();
    if (!boot_control_hal) {
        return device->WriteStatus(FastbootResult::FAIL,
                                   "Cannot set slot: boot control HAL absent");
    }
    if (slot >= boot_control_hal->getNumberSlots()) {
        return device->WriteStatus(FastbootResult::FAIL, "Slot out of range");
    }
    CommandResult ret;
    auto cb = [&ret](CommandResult result) { ret = result; };
    auto result = boot_control_hal->setActiveBootSlot(slot, cb);
    if (result.isOk() && ret.success) return device->WriteStatus(FastbootResult::OKAY, "");
    return device->WriteStatus(FastbootResult::FAIL, "Unable to set slot");
}

bool ShutDownHandler(FastbootDevice* device, const std::vector<std::string>& /* args */) {
    auto result = device->WriteStatus(FastbootResult::OKAY, "Shutting down");
    android::base::SetProperty(ANDROID_RB_PROPERTY, "shutdown,fastboot");
    device->CloseDevice();
    TEMP_FAILURE_RETRY(pause());
    return result;
}

bool RebootHandler(FastbootDevice* device, const std::vector<std::string>& /* args */) {
    auto result = device->WriteStatus(FastbootResult::OKAY, "Rebooting");
    android::base::SetProperty(ANDROID_RB_PROPERTY, "reboot,from_fastboot");
    device->CloseDevice();
    TEMP_FAILURE_RETRY(pause());
    return result;
}

bool RebootBootloaderHandler(FastbootDevice* device, const std::vector<std::string>& /* args */) {
    auto result = device->WriteStatus(FastbootResult::OKAY, "Rebooting bootloader");
    android::base::SetProperty(ANDROID_RB_PROPERTY, "reboot,bootloader");
    device->CloseDevice();
    TEMP_FAILURE_RETRY(pause());
    return result;
}

bool RebootFastbootHandler(FastbootDevice* device, const std::vector<std::string>& /* args */) {
    auto result = device->WriteStatus(FastbootResult::OKAY, "Rebooting fastboot");
    android::base::SetProperty(ANDROID_RB_PROPERTY, "reboot,fastboot");
    device->CloseDevice();
    TEMP_FAILURE_RETRY(pause());
    return result;
}

static bool EnterRecovery() {
    const char msg_switch_to_recovery = 'r';

    android::base::unique_fd sock(socket(AF_UNIX, SOCK_STREAM, 0));
    if (sock < 0) {
        PLOG(ERROR) << "Couldn't create sock";
        return false;
    }

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, "/dev/socket/recovery", sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        PLOG(ERROR) << "Couldn't connect to recovery";
        return false;
    }
    // Switch to recovery will not update the boot reason since it does not
    // require a reboot.
    auto ret = write(sock, &msg_switch_to_recovery, sizeof(msg_switch_to_recovery));
    if (ret != sizeof(msg_switch_to_recovery)) {
        PLOG(ERROR) << "Couldn't write message to switch to recovery";
        return false;
    }

    return true;
}

bool RebootRecoveryHandler(FastbootDevice* device, const std::vector<std::string>& /* args */) {
    auto status = true;
    if (EnterRecovery()) {
        status = device->WriteStatus(FastbootResult::OKAY, "Rebooting to recovery");
    } else {
        status = device->WriteStatus(FastbootResult::FAIL, "Unable to reboot to recovery");
    }
    device->CloseDevice();
    TEMP_FAILURE_RETRY(pause());
    return status;
}
