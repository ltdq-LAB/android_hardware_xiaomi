/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "LidStateTracker.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <log/log.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstring>
#include <mutex>
#include <string>

namespace android {
namespace hardware {
namespace sensors {
namespace V2_1 {
namespace subhal {
namespace implementation {

namespace {

constexpr const char* kInputDeviceDir = "/dev/input";
constexpr int kBitsPerLong = sizeof(unsigned long) * 8;

bool isBitSet(const unsigned long* bits, int bit) {
    return (bits[bit / kBitsPerLong] & (1UL << (bit % kBitsPerLong))) != 0;
}

bool supportsLidSwitch(int fd) {
    unsigned long evbits[(EV_MAX + kBitsPerLong - 1) / kBitsPerLong] = {};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) {
        return false;
    }
    if (!isBitSet(evbits, EV_SW)) {
        return false;
    }
    unsigned long swbits[(SW_MAX + kBitsPerLong - 1) / kBitsPerLong] = {};
    if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof(swbits)), swbits) < 0) {
        return false;
    }
    return isBitSet(swbits, SW_LID);
}

}  // namespace

LidStateTracker& LidStateTracker::get() {
    static LidStateTracker instance;
    return instance;
}

int LidStateTracker::findLidFd() {
    DIR* dir = opendir(kInputDeviceDir);
    if (dir == nullptr) {
        ALOGE("Cannot open %s: %s", kInputDeviceDir, strerror(errno));
        return -1;
    }

    int lidFd = -1;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        std::string path = std::string(kInputDeviceDir) + "/" + entry->d_name;
        int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        if (supportsLidSwitch(fd)) {
            lidFd = fd;
            break;
        }
        close(fd);
    }
    closedir(dir);
    return lidFd;
}

bool LidStateTracker::isLidOpen() {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);

    if (mLidFd < 0) {
        mLidFd = findLidFd();
        if (mLidFd < 0) {
            ALOGE("No lid switch input device found; LCD wake gestures are not lid-gated");
            return true;
        }
    }

    unsigned long sw[(SW_MAX + kBitsPerLong - 1) / kBitsPerLong] = {};
    if (ioctl(mLidFd, EVIOCGSW(sizeof(sw)), sw) < 0) {
        ALOGE("Cannot read lid switch state: %s", strerror(errno));
        close(mLidFd);
        mLidFd = -1;
        return true;
    }

    /* SW_LID is set while the lid is shut. */
    return !isBitSet(sw, SW_LID);
}

}  // namespace implementation
}  // namespace subhal
}  // namespace V2_1
}  // namespace sensors
}  // namespace hardware
}  // namespace android
