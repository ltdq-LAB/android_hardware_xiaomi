/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vendor.lineage.keyboard-service.xiaomi"

#include "NanoappDevice.h"

#include <android-base/logging.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <utility>

#include "Protocol.h"

namespace xiaomi::keyboard {
namespace {

constexpr char kNanoappDevice[] = "/dev/nanodev0";
constexpr int kOpenRetryDelayMs = 1000;

}  // namespace

NanoappDevice::NanoappDevice(DataCallback dataCallback, ErrorCallback errorCallback)
    : mDataCallback(std::move(dataCallback)), mErrorCallback(std::move(errorCallback)) {
    int pipeFds[2];
    if (pipe2(pipeFds, O_CLOEXEC | O_NONBLOCK) == 0) {
        mStopReadFd.reset(pipeFds[0]);
        mStopWriteFd.reset(pipeFds[1]);
    } else {
        PLOG(ERROR) << "Failed to create nanoapp stop pipe";
    }
}

NanoappDevice::~NanoappDevice() {
    stop();
}

void NanoappDevice::start() {
    bool expected = false;
    if (!mStarted.compare_exchange_strong(expected, true)) {
        return;
    }
    drainStopSignal();
    mStopping = false;
    mThread = std::thread(&NanoappDevice::run, this);
}

void NanoappDevice::stop() {
    if (!mStarted.exchange(false)) {
        return;
    }

    mStopping = true;
    if (mStopWriteFd.ok()) {
        const uint8_t value = 1;
        if (write(mStopWriteFd.get(), &value, sizeof(value)) < 0 && errno != EAGAIN) {
            PLOG(WARNING) << "Failed to interrupt nanoapp reader";
        }
    }
    if (mThread.joinable()) {
        mThread.join();
    }

    std::lock_guard<std::mutex> lock(mDeviceMutex);
    mDeviceFd.reset();
}

bool NanoappDevice::isOpen() const {
    std::lock_guard<std::mutex> lock(mDeviceMutex);
    return mDeviceFd.ok();
}

int NanoappDevice::writeCommand(const uint8_t* data, size_t size) {
    if (data == nullptr || size != kNanoappRawFrameSize) {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mDeviceMutex);
    if (mStopping) {
        return -ESHUTDOWN;
    }
    if (!mDeviceFd.ok()) {
        return -ENODEV;
    }

    // nano_chardev defines one command as exactly one 66-byte write. Retrying
    // a short result as another write would turn the suffix into a malformed
    // second command rather than completing the first one.
    const ssize_t rc = TEMP_FAILURE_RETRY(write(mDeviceFd.get(), data, size));
    if (rc < 0) {
        return -errno;
    }
    return rc == static_cast<ssize_t>(size) ? static_cast<int>(rc) : -EIO;
}

bool NanoappDevice::openDevice() {
    android::base::unique_fd fd(open(kNanoappDevice, O_RDWR | O_CLOEXEC | O_NONBLOCK));
    if (!fd.ok()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mDeviceMutex);
    mDeviceFd = std::move(fd);
    LOG(INFO) << "Opened " << kNanoappDevice;
    return true;
}

void NanoappDevice::closeDevice(int expectedFd) {
    std::lock_guard<std::mutex> lock(mDeviceMutex);
    if (mDeviceFd.get() == expectedFd) {
        mDeviceFd.reset();
        LOG(WARNING) << "Closed " << kNanoappDevice << "; waiting for it to return";
    }
}

bool NanoappDevice::waitForStop(int timeoutMs) {
    if (!mStopReadFd.ok()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        return mStopping;
    }

    pollfd stopPoll = {
            .fd = mStopReadFd.get(),
            .events = POLLIN,
            .revents = 0,
    };
    const int rc = TEMP_FAILURE_RETRY(poll(&stopPoll, 1, timeoutMs));
    return mStopping || (rc > 0 && (stopPoll.revents & POLLIN));
}

void NanoappDevice::drainStopSignal() {
    if (!mStopReadFd.ok()) {
        return;
    }

    std::array<uint8_t, 16> buffer{};
    ssize_t rc;
    do {
        rc = TEMP_FAILURE_RETRY(read(mStopReadFd.get(), buffer.data(), buffer.size()));
    } while (rc > 0);
    if (rc < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        PLOG(WARNING) << "Failed to drain nanoapp stop pipe";
    }
}

void NanoappDevice::run() {
    std::array<uint8_t, kNanoappMaxPayloadSize> buffer{};

    while (!mStopping) {
        if (!isOpen() && !openDevice()) {
            waitForStop(kOpenRetryDelayMs);
            continue;
        }

        int deviceFd;
        {
            std::lock_guard<std::mutex> lock(mDeviceMutex);
            deviceFd = mDeviceFd.get();
        }

        pollfd polls[2] = {
                {
                        .fd = mStopReadFd.get(),
                        .events = POLLIN,
                        .revents = 0,
                },
                {
                        .fd = deviceFd,
                        .events = POLLIN,
                        .revents = 0,
                },
        };
        const nfds_t pollCount = mStopReadFd.ok() ? 2 : 1;
        pollfd* firstPoll = mStopReadFd.ok() ? polls : &polls[1];
        const int timeout = mStopReadFd.ok() ? -1 : kOpenRetryDelayMs;
        const int rc = TEMP_FAILURE_RETRY(poll(firstPoll, pollCount, timeout));
        if (rc < 0) {
            const int error = errno;
            PLOG(ERROR) << "Failed to poll " << kNanoappDevice;
            mErrorCallback(-error);
            closeDevice(deviceFd);
            continue;
        }
        if (mStopReadFd.ok() && (polls[0].revents & POLLIN)) {
            break;
        }
        if (polls[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            mErrorCallback(-ENODEV);
            closeDevice(deviceFd);
            continue;
        }
        if (!(polls[1].revents & POLLIN)) {
            continue;
        }

        const ssize_t bytes = TEMP_FAILURE_RETRY(read(deviceFd, buffer.data(), buffer.size()));
        if (bytes > 0) {
            mDataCallback(buffer.data(), static_cast<size_t>(bytes));
        } else if (bytes == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            const int error = bytes == 0 ? ENODEV : errno;
            mErrorCallback(-error);
            closeDevice(deviceFd);
        }
    }
}

}  // namespace xiaomi::keyboard
