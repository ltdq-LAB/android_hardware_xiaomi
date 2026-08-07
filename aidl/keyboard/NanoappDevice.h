/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <android-base/unique_fd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace xiaomi::keyboard {

class NanoappDevice {
  public:
    using DataCallback = std::function<void(const uint8_t*, size_t)>;
    using ErrorCallback = std::function<void(int)>;

    NanoappDevice(DataCallback dataCallback, ErrorCallback errorCallback);
    ~NanoappDevice();

    NanoappDevice(const NanoappDevice&) = delete;
    NanoappDevice& operator=(const NanoappDevice&) = delete;

    void start();
    void stop();
    int writeCommand(const uint8_t* data, size_t size);
    bool isOpen() const;

  private:
    void run();
    bool openDevice();
    void closeDevice(int expectedFd);
    bool waitForStop(int timeoutMs);
    void drainStopSignal();

    DataCallback mDataCallback;
    ErrorCallback mErrorCallback;
    mutable std::mutex mDeviceMutex;
    android::base::unique_fd mDeviceFd;
    android::base::unique_fd mStopReadFd;
    android::base::unique_fd mStopWriteFd;
    std::atomic_bool mStarted = false;
    std::atomic_bool mStopping = false;
    std::thread mThread;
};

}  // namespace xiaomi::keyboard
