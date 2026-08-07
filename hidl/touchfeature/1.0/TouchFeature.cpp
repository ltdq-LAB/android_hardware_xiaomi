/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "TouchFeatureHal"

#include "TouchFeature.h"

#include <android-base/unique_fd.h>
#include <fcntl.h>
#include <log/log.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace vendor::xiaomi::hw::touchfeature::V1_0::implementation {
namespace {

constexpr char kTouchDevice[] = "/dev/xiaomi-touch";
constexpr char kTouchEventSocketName[] = "touchevent";

// Exact request/response record used between the stock HIDL implementation
// and liuqin's toucheventcheck daemon.  The first int is command 1, followed
// by the returned byte count and a NUL-terminated diagnostic event string.
struct TouchEventMessage {
    int32_t command;
    int32_t length;
    char event[1024];
};

static_assert(sizeof(TouchEventMessage) == 1032);

std::mutex gTouchEventLock;
android::base::unique_fd gTouchEventSocket;

// Runtime hashes extracted from the liuqin OS3.0.7.0 generated HIDL library.
// The source-level hash also covers formatting and comments, so override the
// generated implementation to preserve the stock service's observable chain.
constexpr std::array<uint8_t, 32> kStockInterfaceHash{
        0xd7, 0x0b, 0xb4, 0x0e, 0x51, 0xe8, 0x6f, 0x08, 0x53, 0x2a, 0x6a,
        0xdc, 0x5b, 0x21, 0x99, 0x60, 0x74, 0x5a, 0x7a, 0x2b, 0x37, 0x54,
        0xfa, 0xfe, 0x01, 0xe6, 0xe0, 0x53, 0x29, 0x69, 0x60, 0xef,
};
constexpr std::array<uint8_t, 32> kBaseInterfaceHash{
        0xec, 0x7f, 0xd7, 0x9e, 0xd0, 0x2d, 0xfa, 0x85, 0xbc, 0x49, 0x94,
        0x26, 0xad, 0xae, 0x3e, 0xbe, 0x23, 0xef, 0x05, 0x24, 0xf3, 0xcd,
        0x69, 0x57, 0x13, 0x93, 0x24, 0xb8, 0x3b, 0x18, 0xca, 0x4c,
};

// Exact DATA_MODE_* indices exported by the liuqin OS3 nt36532 module.
enum StockMode : int32_t {
    kGameMode = 0,
    kDoubleTap = 14,
    kStylusConnection = 20,
    kStylusQuickNote = 24,
};

// The Lineage kernel intentionally exposes a compact request ABI.  Do not use
// the stock service's 256-int ioctl payload with this device node.
enum KernelMode : int32_t {
    kKernelSingleTapGesture = 0,
    kKernelDoubleTapGesture = 1,
    kKernelReportRate = 5,
    kKernelStylusConnection = 7,
};

struct TouchModeRequest {
    int32_t mode;
    int32_t value;
};

static_assert(sizeof(TouchModeRequest) == 8);

constexpr unsigned long kSetCurrentValue = _IOW('T', 0, TouchModeRequest);
constexpr unsigned long kGetCurrentValue = _IOR('T', 1, TouchModeRequest);

struct ModeValues {
    int32_t initial;
    int32_t defaultValue;
    int32_t minValue;
    int32_t maxValue;
};

// Extracted from xiaomi_touch_interfaces in the liuqin OS3.0.7.0
// nt36532_touch.ko.  getModeValue() exposes these as
// [current, default, minimum, maximum].
constexpr std::array<ModeValues, 25> kModeValues{{
        {0, 0, 0, 1},    // 0  GAME
        {0, 0, 0, 1},    // 1  ACTIVE
        {2, 2, 0, 4},    // 2  UP_THRESHOLD
        {2, 2, 0, 4},    // 3  TOLERANCE
        {2, 2, 0, 4},    // 4  AIM_SENSITIVITY
        {2, 2, 0, 4},    // 5  TAP_STABILITY
        {1, 1, 1, 3},    // 6  EXPERT
        {0, 2, 0, 3},    // 7  EDGE_FILTER
        {0, 0, 0, 3},    // 8  ORIENTATION
        {0, 0, 0, 0},    // 9  REPORT_RATE (stock no-op)
        {0, 0, 0, 0},    // 10 FOD
        {0, 0, 0, 0},    // 11 AOD
        {0, 0, 0, 1},    // 12 RESIST_RF
        {0, 0, 0, 0},    // 13 IDLE_TIME
        {0, 0, 0, 1},    // 14 DOUBLETAP
        {0, 0, 0, 0},    // 15 GRIP
        {0, 0, 0, 0},    // 16 FOD_ICON
        {0, 0, 0, 0},    // 17 NONUI
        {0, 0, 0, 0},    // 18 DEBUG
        {0, 0, 0, 0},    // 19 POWER
        {0, 0, -1, 18},  // 20 STYLUS_MODE
        {0, 0, 0, 0},    // 21 PERFORMANCE
        {0, 0, 0, 0},    // 22 STYLUS_HOPPING
        {0, 0, 0, 0},    // 23 PASSIVE_PEN
        {0, 0, 0, 1},    // 24 STYLUS_QUICK_NOTE
}};

bool isValidMode(int32_t mode) {
    return mode >= 0 && static_cast<std::size_t>(mode) < kModeValues.size();
}

int32_t kernelIoctl(unsigned long request, int32_t mode, int32_t value, int32_t* result) {
    android::base::unique_fd fd(open(kTouchDevice, O_RDWR | O_CLOEXEC));
    if (fd.get() < 0) {
        const int error = errno;
        ALOGE("cannot open %s: %s", kTouchDevice, strerror(error));
        return -error;
    }

    TouchModeRequest modeRequest{mode, value};
    if (ioctl(fd.get(), request, &modeRequest) < 0) {
        const int error = errno;
        ALOGE("ioctl request=0x%lx mode=%d failed: %s", request, mode, strerror(error));
        return -error;
    }

    if (result != nullptr) {
        *result = modeRequest.value;
    }
    return 0;
}

int32_t setKernelMode(int32_t mode, int32_t value) {
    return kernelIoctl(kSetCurrentValue, mode, value, nullptr);
}

int32_t getKernelMode(int32_t mode) {
    int32_t value = 0;
    const int32_t status = kernelIoctl(kGetCurrentValue, mode, 0, &value);
    return status < 0 ? status : value;
}

bool connectTouchEventSocketLocked() {
    if (gTouchEventSocket.ok()) {
        pollfd descriptor{gTouchEventSocket.get(), 0, 0};
        if (poll(&descriptor, 1, 0) >= 0 && (descriptor.revents & POLLHUP) != 0) {
            ALOGI("touchevent peer closed");
            gTouchEventSocket.reset();
        }
    }

    if (gTouchEventSocket.ok()) {
        return true;
    }

    android::base::unique_fd socketFd(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
    if (!socketFd.ok()) {
        const int error = errno;
        ALOGE("cannot create touchevent socket: %s", strerror(error));
        return false;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    static_assert(sizeof(kTouchEventSocketName) <= sizeof(address.sun_path));
    memcpy(address.sun_path + 1, kTouchEventSocketName, sizeof(kTouchEventSocketName) - 1);
    constexpr socklen_t addressLength =
            offsetof(sockaddr_un, sun_path) + sizeof(kTouchEventSocketName);

    if (connect(socketFd.get(), reinterpret_cast<const sockaddr*>(&address), addressLength) < 0) {
        const int error = errno;
        ALOGE("cannot connect to abstract touchevent socket: %s", strerror(error));
        return false;
    }

    gTouchEventSocket = std::move(socketFd);
    return true;
}

std::string readTouchEvent() {
    std::lock_guard guard(gTouchEventLock);
    if (!connectTouchEventSocketLocked()) {
        return {};
    }

    TouchEventMessage message{};
    message.command = 1;

    if (send(gTouchEventSocket.get(), &message, sizeof(message), MSG_NOSIGNAL) <= 0) {
        const int error = errno;
        ALOGE("cannot request touchevent: %s", strerror(error));
        return {};
    }

    memset(&message, 0, sizeof(message));
    if (recv(gTouchEventSocket.get(), &message, sizeof(message), 0) <= 0) {
        const int error = errno;
        ALOGE("cannot read touchevent: %s", strerror(error));
        return {};
    }

    const std::size_t stringLength = strnlen(message.event, sizeof(message.event));
    const std::size_t eventLength =
            message.length < 0 ? stringLength
                               : std::min(static_cast<std::size_t>(message.length), stringLength);
    return std::string(message.event, eventLength);
}

}  // namespace

TouchFeature::TouchFeature() {
    for (std::size_t mode = 0; mode < kModeValues.size(); ++mode) {
        currentValues_[mode] = kModeValues[mode].initial;
    }
}

::android::hardware::Return<int32_t> TouchFeature::setModeValue(int32_t touchId,
                                                                int32_t ControlMode,
                                                                int32_t ModeValue) {
    if (touchId != 0 || !isValidMode(ControlMode)) {
        return -EINVAL;
    }

    std::lock_guard guard(lock_);
    return setModeValueLocked(ControlMode, ModeValue);
}

int32_t TouchFeature::setModeValueLocked(int32_t mode, int32_t value) {
    const ModeValues& values = kModeValues[mode];
    const int32_t clamped = std::clamp(value, values.minValue, values.maxValue);
    int32_t status;

    switch (mode) {
        case kGameMode:
            status = setKernelMode(kKernelReportRate, clamped);
            break;
        case kDoubleTap:
            status = setKernelMode(kKernelDoubleTapGesture, clamped);
            break;
        case kStylusConnection:
            // Stock first clamps to [-1, 18]. Connected types 3..7 therefore
            // become 0x12 and increment the type-2 count. Values 0 and 3..16
            // do not alter counters, but the KO still releases any active pen
            // event and recomputes its path state, so forward every value.
            status = setKernelMode(kKernelStylusConnection, clamped);
            break;
        case kStylusQuickNote:
            status = setKernelMode(kKernelSingleTapGesture, clamped);
            break;
        default:
            if (values.minValue == 0 && values.maxValue == 0) {
                // These are genuine zero-range no-op modes in the liuqin KO;
                // in particular mode 9 does not select a report rate.
                status = 0;
            } else {
                // The compact Lineage UAPI has no faithful translation for
                // the stock controller tuning command.
                status = -EINVAL;
            }
            break;
    }

    return status;
}

int32_t TouchFeature::getCurrentValueLocked(int32_t mode) {
    int32_t value;

    switch (mode) {
        case kGameMode:
            value = getKernelMode(kKernelReportRate);
            break;
        default:
            // The exact KO has separate SET_CUR and GET_CUR cells. Its
            // mode-14/20/24 special setters do not copy SET_CUR into GET_CUR,
            // so their observable current value remains the table value (0).
            return currentValues_[mode];
    }

    if (value >= 0) {
        currentValues_[mode] =
                std::clamp(value, kModeValues[mode].minValue, kModeValues[mode].maxValue);
        return currentValues_[mode];
    }
    return value;
}

::android::hardware::Return<int32_t> TouchFeature::getModeCurValue(int32_t touchId,
                                                                   int32_t ControlMode) {
    if (touchId != 0 || !isValidMode(ControlMode)) {
        return -EINVAL;
    }

    std::lock_guard guard(lock_);
    return getCurrentValueLocked(ControlMode);
}

::android::hardware::Return<int32_t> TouchFeature::getModeMaxValue(int32_t touchId,
                                                                   int32_t ControlMode) {
    if (touchId != 0 || !isValidMode(ControlMode)) {
        return -EINVAL;
    }
    return kModeValues[ControlMode].maxValue;
}

::android::hardware::Return<int32_t> TouchFeature::getModeMinValue(int32_t touchId,
                                                                   int32_t ControlMode) {
    if (touchId != 0 || !isValidMode(ControlMode)) {
        return -EINVAL;
    }
    return kModeValues[ControlMode].minValue;
}

::android::hardware::Return<int32_t> TouchFeature::getModeDefaultValue(int32_t touchId,
                                                                       int32_t ControlMode) {
    if (touchId != 0 || !isValidMode(ControlMode)) {
        return -EINVAL;
    }
    return kModeValues[ControlMode].defaultValue;
}

::android::hardware::Return<int32_t> TouchFeature::modeReset(int32_t touchId, int32_t ControlMode) {
    if (touchId != 0 || !isValidMode(ControlMode)) {
        return -EINVAL;
    }

    std::lock_guard guard(lock_);
    return resetModeLocked(ControlMode);
}

int32_t TouchFeature::resetModeLocked(int32_t mode) {
    // Stock mode 0 resets its game/tuning block, but the compact UAPI only has
    // a faithful backend for the game bit itself. Do not manufacture GET_CUR
    // changes for tuning rows which were never sent to the controller.
    if (mode == 0) {
        return setKernelMode(kKernelReportRate, kModeValues[0].defaultValue);
    } else if (mode <= 8) {
        return -EINVAL;
    }
    return 0;
}

::android::hardware::Return<void> TouchFeature::getModeValue(int32_t touchId, int32_t mode,
                                                             getModeValue_cb _hidl_cb) {
    ::android::hardware::hidl_vec<int32_t> result;
    result.resize(4);

    if (touchId != 0 || !isValidMode(mode)) {
        std::fill(result.begin(), result.end(), -EINVAL);
        _hidl_cb(result);
        return {};
    }

    std::lock_guard guard(lock_);
    result[0] = getCurrentValueLocked(mode);
    result[1] = kModeValues[mode].defaultValue;
    result[2] = kModeValues[mode].minValue;
    result[3] = kModeValues[mode].maxValue;
    _hidl_cb(result);
    return {};
}

::android::hardware::Return<int32_t> TouchFeature::setModeLongValue(
        int32_t touchId, int32_t ControlMode, int32_t ValueLen,
        const ::android::hardware::hidl_vec<int32_t>& ValueBuf) {
    // The stock HAL reserves three of its 256 ints for touch id, mode and
    // length.  Preserve its externally visible upper-bound failure exactly.
    if (ValueLen > 253) {
        return -1;
    }
    if (touchId != 0 || !isValidMode(ControlMode) || ValueLen < 0 ||
        static_cast<std::size_t>(ValueLen) > ValueBuf.size()) {
        return -EINVAL;
    }

    // The exact liuqin nt36532 module leaves the common driver's long-value
    // callback NULL.  Its stock 0x5407 ioctl consequently copies the record,
    // performs no controller operation and returns success.  Keep that
    // observable no-op rather than inventing an edge-data command for this IC.
    android::base::unique_fd fd(open(kTouchDevice, O_RDWR | O_CLOEXEC));
    if (!fd.ok()) {
        const int error = errno;
        ALOGE("cannot open %s for long-value no-op: %s", kTouchDevice, strerror(error));
        return -1;
    }
    return 0;
}

::android::hardware::Return<void> TouchFeature::getTouchEvent(getTouchEvent_cb _hidl_cb) {
    _hidl_cb(::android::hardware::hidl_string(readTouchEvent()));
    return {};
}

::android::hardware::Return<void> TouchFeature::getHashChain(getHashChain_cb _hidl_cb) {
    ::android::hardware::hidl_vec<::android::hardware::hidl_array<uint8_t, 32>> hashes;
    hashes.resize(2);
    std::copy(kStockInterfaceHash.begin(), kStockInterfaceHash.end(), hashes[0].data());
    std::copy(kBaseInterfaceHash.begin(), kBaseInterfaceHash.end(), hashes[1].data());
    _hidl_cb(hashes);
    return {};
}

}  // namespace vendor::xiaomi::hw::touchfeature::V1_0::implementation
