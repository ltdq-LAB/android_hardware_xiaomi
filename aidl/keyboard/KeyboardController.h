/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "HallInputMonitor.h"
#include "NanoappDevice.h"
#include "Protocol.h"

namespace xiaomi::keyboard {

class KeyboardController {
  public:
    using DataCallback = std::function<void(const std::vector<uint8_t>&)>;
    using ErrorCallback = std::function<void(int)>;

    KeyboardController();
    ~KeyboardController();

    KeyboardController(const KeyboardController&) = delete;
    KeyboardController& operator=(const KeyboardController&) = delete;

    void start();
    void stop();
    int sendClientCommand(const std::vector<uint8_t>& command);
    void setCallbacks(DataCallback dataCallback, ErrorCallback errorCallback);

  private:
    void queryLoop();
    void onDeviceData(const uint8_t* data, size_t size);
    void onDeviceError(int error);
    void onHallSwitchState(bool valid, bool lidOpen, bool tabletOpen);
    bool sendRaw(const RawCommand& command);
    void updateInputGate(bool enabled, bool force = false);
    void handleStatus(const StatusReport& status);

    NanoappDevice mDevice;
    HallInputMonitor mHallMonitor;
    std::mutex mCallbackMutex;
    DataCallback mDataCallback;
    ErrorCallback mErrorCallback;

    std::mutex mStateMutex;
    StatusReport mStatus;
    KeyboardVersionReport mKeyboardVersion;
    KeyboardIdentityReport mKeyboardIdentity;
    GsensorReport mGsensor;
    std::string mMcuVersion;
    bool mHaveStatus = false;
    bool mHaveKeyboardIdentity = false;
    bool mHaveGsensor = false;
    bool mHaveSleepState = false;
    bool mKeyboardSleeping = false;
    bool mHaveInputHallState = false;
    bool mHaveProtocolHallState = false;
    uint8_t mInputHallState = 0xff;
    uint8_t mProtocolHallState = 0xff;
    bool mInputGateKnown = false;
    bool mInputEnabled = false;
    bool mTouchpadStateKnown = false;
    bool mTouchpadEnabled = false;
    std::chrono::steady_clock::time_point mLastStatusUpdate;
    std::chrono::steady_clock::time_point mLastInputHallUpdate;
    std::chrono::steady_clock::time_point mLastProtocolHallUpdate;
    std::mutex mGateMutex;

    std::atomic_bool mStarted = false;
    std::atomic_bool mStopping = false;
    std::mutex mQueryMutex;
    std::condition_variable mQueryCondition;
    std::thread mQueryThread;
};

}  // namespace xiaomi::keyboard
