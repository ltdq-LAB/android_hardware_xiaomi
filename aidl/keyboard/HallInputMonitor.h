/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <android-base/unique_fd.h>

#include <atomic>
#include <functional>
#include <thread>

namespace xiaomi::keyboard {

class HallInputMonitor {
  public:
    using StateCallback = std::function<void(bool valid, bool lidOpen, bool tabletOpen)>;

    explicit HallInputMonitor(StateCallback callback);
    ~HallInputMonitor();

    HallInputMonitor(const HallInputMonitor&) = delete;
    HallInputMonitor& operator=(const HallInputMonitor&) = delete;

    void start();
    void stop();

  private:
    android::base::unique_fd findSwitchDevice(bool* lidOpen, bool* tabletOpen);
    void run();

    StateCallback mCallback;
    android::base::unique_fd mStopReadFd;
    android::base::unique_fd mStopWriteFd;
    std::atomic_bool mStarted = false;
    std::atomic_bool mStopping = false;
    std::thread mThread;
};

}  // namespace xiaomi::keyboard
