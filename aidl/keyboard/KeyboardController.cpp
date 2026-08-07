/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vendor.lineage.keyboard-service.xiaomi"

#include "KeyboardController.h"

#include <android-base/file.h>
#include <android-base/logging.h>

#include <cerrno>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <utility>

namespace xiaomi::keyboard {
namespace {

constexpr char kInputEnablePath[] = "/sys/devices/virtual/nanodev/nanodev0/_inputenable";
constexpr auto kQueryInterval = std::chrono::seconds(2);
constexpr auto kStatusTimeout = std::chrono::seconds(6);
constexpr int kVersionQueryPeriod = 15;
constexpr int kGsensorQueryPeriod = 5;
constexpr auto kCommandSpacing = std::chrono::milliseconds(20);

std::string formatBluetoothAddress(const std::array<uint8_t, 6>& address) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (size_t i = 0; i < address.size(); ++i) {
        if (i != 0) {
            output << ':';
        }
        output << std::setw(2) << static_cast<unsigned int>(address[i]);
    }
    return output.str();
}

}  // namespace

KeyboardController::KeyboardController()
    : mDevice([this](const uint8_t* data, size_t size) { onDeviceData(data, size); },
              [this](int error) { onDeviceError(error); }),
      mHallMonitor([this](bool valid, bool lidOpen, bool tabletOpen) {
          onHallSwitchState(valid, lidOpen, tabletOpen);
      }) {}

KeyboardController::~KeyboardController() {
    stop();
}

void KeyboardController::start() {
    bool expected = false;
    if (!mStarted.compare_exchange_strong(expected, true)) {
        return;
    }

    mStopping = false;
    // Never retain stale input devices across a service or framework restart.
    updateInputGate(false, true);
    mHallMonitor.start();
    mDevice.start();
    mQueryThread = std::thread(&KeyboardController::queryLoop, this);
}

void KeyboardController::stop() {
    if (!mStarted.exchange(false)) {
        return;
    }

    mStopping = true;
    mQueryCondition.notify_all();
    if (mQueryThread.joinable()) {
        mQueryThread.join();
    }
    mHallMonitor.stop();
    mDevice.stop();
    // No reader callback can race this final forced teardown after stop().
    updateInputGate(false, true);
}

void KeyboardController::setCallbacks(DataCallback dataCallback, ErrorCallback errorCallback) {
    std::lock_guard<std::mutex> lock(mCallbackMutex);
    mDataCallback = std::move(dataCallback);
    mErrorCallback = std::move(errorCallback);
}

int KeyboardController::sendClientCommand(const std::vector<uint8_t>& command) {
    std::vector<uint8_t> raw;
    if (!decodeClientCommand(command, &raw)) {
        return -EINVAL;
    }
    return mDevice.writeCommand(raw.data(), raw.size());
}

bool KeyboardController::sendRaw(const RawCommand& command) {
    const int result = mDevice.writeCommand(command.data(), command.size());
    if (result < 0 && result != -ENODEV) {
        LOG(WARNING) << "Nanoapp command failed: " << result;
    }
    return result == static_cast<int>(command.size());
}

void KeyboardController::queryLoop() {
    uint32_t queryCount = 0;
    bool identityQueried = false;
    while (!mStopping) {
        bool sleeping;
        {
            std::lock_guard<std::mutex> lock(mStateMutex);
            sleeping = mHaveSleepState && mKeyboardSleeping;
        }
        if (mDevice.isOpen() && !sleeping) {
            sendRaw(makePogoStatusQuery());
            std::this_thread::sleep_for(kCommandSpacing);
            sendRaw(makeHallQuery());

            bool connected;
            {
                std::lock_guard<std::mutex> lock(mStateMutex);
                connected = mHaveStatus && mStatus.connected;
            }
            if (!connected) {
                queryCount = 0;
                identityQueried = false;
            } else {
                if (queryCount % kGsensorQueryPeriod == 0) {
                    std::this_thread::sleep_for(kCommandSpacing);
                    sendRaw(makeGsensorQuery());
                }
                if (queryCount % kVersionQueryPeriod == 0) {
                    std::this_thread::sleep_for(kCommandSpacing);
                    sendRaw(makeKeyboardVersionQuery());
                    std::this_thread::sleep_for(kCommandSpacing);
                    sendRaw(makeMcuVersionQuery());
                }
                if (!identityQueried) {
                    std::this_thread::sleep_for(kCommandSpacing);
                    // The stock framework uses six zero bytes when a local
                    // Bluetooth address is unavailable. This still requests
                    // the keyboard identity/MAC without inventing an address.
                    identityQueried = sendRaw(makeKeyboardIdentityQuery());
                }
                ++queryCount;
            }

            bool timedOut = false;
            {
                std::lock_guard<std::mutex> lock(mStateMutex);
                const auto now = std::chrono::steady_clock::now();
                const bool statusTimedOut = mHaveStatus && mStatus.connected &&
                                            now - mLastStatusUpdate > kStatusTimeout;
                const bool inputHallFresh =
                        mHaveInputHallState && now - mLastInputHallUpdate <= kStatusTimeout;
                const bool protocolHallFresh =
                        mHaveProtocolHallState && now - mLastProtocolHallUpdate <= kStatusTimeout;
                const bool hallTimedOut = mHaveStatus && mStatus.connected &&
                                          (mHaveInputHallState || mHaveProtocolHallState) &&
                                          !inputHallFresh && !protocolHallFresh;
                timedOut = statusTimedOut || hallTimedOut;
                if (statusTimedOut) {
                    mStatus.connected = false;
                    mHaveStatus = false;
                }
                if (hallTimedOut) {
                    mHaveInputHallState = false;
                    mHaveProtocolHallState = false;
                }
            }
            if (timedOut) {
                LOG(WARNING) << "Keyboard status/Hall state timed out; removing input devices";
                updateInputGate(false);
            }
        } else if (!mDevice.isOpen()) {
            queryCount = 0;
            identityQueried = false;
            {
                std::lock_guard<std::mutex> lock(mStateMutex);
                mHaveStatus = false;
                mHaveProtocolHallState = false;
                mHaveKeyboardIdentity = false;
                mHaveGsensor = false;
                mHaveSleepState = false;
                mKeyboardSleeping = false;
                mStatus.connected = false;
            }
            updateInputGate(false);
        }

        std::unique_lock<std::mutex> lock(mQueryMutex);
        mQueryCondition.wait_for(lock, kQueryInterval, [this] { return mStopping.load(); });
    }
}

void KeyboardController::updateInputGate(bool enabled, bool force) {
    // Serialize sysfs writes and re-read the desired state after acquiring the
    // gate lock. This prevents an old timeout/error action from overriding a
    // newer valid A2 response (or vice versa).
    std::lock_guard<std::mutex> gateLock(mGateMutex);
    bool gateNeedsWrite;
    bool touchpadNeedsWrite;
    {
        std::lock_guard<std::mutex> lock(mStateMutex);
        if (!force) {
            const auto now = std::chrono::steady_clock::now();
            const bool inputHallFresh =
                    mHaveInputHallState && now - mLastInputHallUpdate <= kStatusTimeout;
            const bool protocolHallFresh =
                    mHaveProtocolHallState && now - mLastProtocolHallUpdate <= kStatusTimeout;
            // gpio-keys is the authoritative real-time source. The Nanosic
            // local Hall query is used only while gpio-keys is unavailable.
            const bool hallOpen = inputHallFresh ? mInputHallState == 0x11
                                                 : protocolHallFresh && mProtocolHallState == 0x11;
            const bool awake = !mHaveSleepState || !mKeyboardSleeping;
            enabled = mHaveStatus && mStatus.connected && hallOpen && awake;
        }
        gateNeedsWrite = force || !mInputGateKnown || mInputEnabled != enabled;
        touchpadNeedsWrite = !mTouchpadStateKnown || mTouchpadEnabled != enabled;
        if (!gateNeedsWrite && !touchpadNeedsWrite) {
            return;
        }
    }

    // Stop touch reports before unregistering their Linux input device.
    if (!enabled && touchpadNeedsWrite && mDevice.isOpen() &&
        sendRaw(makeFeatureCommand(0x21, 0))) {
        std::lock_guard<std::mutex> lock(mStateMutex);
        mTouchpadEnabled = false;
        mTouchpadStateKnown = true;
    }

    if (gateNeedsWrite) {
        if (!android::base::WriteStringToFile(enabled ? "1" : "0", kInputEnablePath)) {
            PLOG(WARNING) << "Failed to " << (enabled ? "register" : "remove")
                          << " Nanosic input devices through " << kInputEnablePath;
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mStateMutex);
            mInputEnabled = enabled;
            mInputGateKnown = true;
        }
        LOG(INFO) << "Nanosic input devices " << (enabled ? "registered" : "removed");
    }

    // Enable hardware touch reporting only after its Linux input device exists.
    if (enabled && touchpadNeedsWrite && mDevice.isOpen() && sendRaw(makeFeatureCommand(0x21, 1))) {
        std::lock_guard<std::mutex> lock(mStateMutex);
        mTouchpadEnabled = true;
        mTouchpadStateKnown = true;
    }
}

void KeyboardController::handleStatus(const StatusReport& status) {
    bool changed;
    {
        std::lock_guard<std::mutex> lock(mStateMutex);
        if (mHaveSleepState && mKeyboardSleeping) {
            return;
        }
        changed = !mHaveStatus || mStatus.connected != status.connected ||
                  mStatus.keyboardStatus != status.keyboardStatus ||
                  mStatus.overCurrent != status.overCurrent;
        mStatus = status;
        mHaveStatus = true;
        mLastStatusUpdate = std::chrono::steady_clock::now();
    }

    updateInputGate(status.connected);
    if (changed) {
        LOG(INFO) << "Keyboard status: connected=" << status.connected
                  << " pogo_ok=" << status.pogoPinHealthy << " over_current=" << status.overCurrent
                  << " key_status=0x" << std::hex << static_cast<int>(status.keyboardStatus)
                  << std::dec << " battery_mv=" << status.batteryMillivolts;
    }
}

void KeyboardController::onHallSwitchState(bool valid, bool lidOpen, bool tabletOpen) {
    bool changed;
    {
        std::lock_guard<std::mutex> lock(mStateMutex);
        const uint8_t state = static_cast<uint8_t>((lidOpen ? 0x10 : 0) | (tabletOpen ? 0x01 : 0));
        changed = mHaveInputHallState != valid || (valid && mInputHallState != state);
        mHaveInputHallState = valid;
        if (valid) {
            mInputHallState = state;
            mLastInputHallUpdate = std::chrono::steady_clock::now();
        }
    }
    updateInputGate(false);
    if (changed) {
        LOG(INFO) << "Hall input state: valid=" << valid << " lid_open=" << lidOpen
                  << " tablet_open=" << tabletOpen;
    }
}

void KeyboardController::onDeviceData(const uint8_t* data, size_t size) {
    // The stock service consumes its private debug-level packet locally.
    if (size >= 4 && data[0] == 0x27 && data[2] == 0xff && data[3] == 0xff) {
        return;
    }

    if (const auto status = parseStatusReport(data, size)) {
        handleStatus(*status);
    }
    if (const auto version = parseKeyboardVersionReport(data, size)) {
        std::lock_guard<std::mutex> lock(mStateMutex);
        mKeyboardVersion = *version;
        LOG(INFO) << "Keyboard version=0x" << std::hex << version->keyboardVersion << " touchpad=0x"
                  << version->touchpadVersion << std::dec;
    }
    if (const auto version = parseMcuVersionReport(data, size)) {
        std::lock_guard<std::mutex> lock(mStateMutex);
        mMcuVersion = *version;
        LOG(INFO) << "Keyboard MCU version=" << mMcuVersion;
    }
    if (const auto identity = parseKeyboardIdentityReport(data, size)) {
        bool changed;
        {
            std::lock_guard<std::mutex> lock(mStateMutex);
            changed = !mHaveKeyboardIdentity ||
                      mKeyboardIdentity.bluetoothAddress != identity->bluetoothAddress;
            mKeyboardIdentity = *identity;
            mHaveKeyboardIdentity = true;
        }
        if (changed) {
            LOG(INFO) << "Keyboard Bluetooth address="
                      << formatBluetoothAddress(identity->bluetoothAddress);
        }
    }
    if (const auto gsensor = parseGsensorReport(data, size)) {
        {
            std::lock_guard<std::mutex> lock(mStateMutex);
            mGsensor = *gsensor;
            mHaveGsensor = true;
        }
        LOG(VERBOSE) << "Keyboard GSensor m/s^2: x=" << gsensor->xMetersPerSecondSquared
                     << " y=" << gsensor->yMetersPerSecondSquared
                     << " z=" << gsensor->zMetersPerSecondSquared;
    }
    if (const auto sleep = parseSleepReport(data, size)) {
        bool changed;
        {
            std::lock_guard<std::mutex> lock(mStateMutex);
            changed = !mHaveSleepState || mKeyboardSleeping != sleep->sleeping;
            mHaveSleepState = true;
            mKeyboardSleeping = sleep->sleeping;
            if (sleep->sleeping) {
                // Never retain a stale pogo result while active queries are
                // intentionally paused for keyboard sleep.
                mHaveStatus = false;
                mStatus.connected = false;
            } else {
                // Firmware may reset touch reporting while asleep; force a
                // fresh 0x21 write after connection is confirmed again.
                mTouchpadStateKnown = false;
            }
        }
        updateInputGate(false);
        if (changed) {
            LOG(INFO) << "Keyboard sleep state=" << sleep->sleeping;
        }
    }
    if (const auto effect = parseFeatureEffectReport(data, size)) {
        LOG(INFO) << "Keyboard feature applied: command=0x" << std::hex
                  << static_cast<int>(effect->command) << " value=0x"
                  << static_cast<int>(effect->value) << std::dec;
    }
    if (const auto ack = parseCommandAckReport(data, size)) {
        if (ack->status == 0) {
            LOG(INFO) << "Keyboard command ACK: command=0x" << std::hex
                      << static_cast<int>(ack->command) << " status=0x0" << std::dec;
        } else {
            LOG(WARNING) << "Keyboard command rejected: command=0x" << std::hex
                         << static_cast<int>(ack->command) << " status=0x"
                         << static_cast<int>(ack->status) << std::dec;
        }
    }
    if (const auto hall = parseHallReport(data, size)) {
        bool changed;
        {
            std::lock_guard<std::mutex> lock(mStateMutex);
            changed = !mHaveProtocolHallState || mProtocolHallState != *hall;
            mProtocolHallState = *hall;
            mHaveProtocolHallState = true;
            mLastProtocolHallUpdate = std::chrono::steady_clock::now();
        }
        updateInputGate(false);
        if (changed) {
            LOG(INFO) << "Keyboard Hall state=0x" << std::hex << static_cast<int>(*hall)
                      << std::dec;
        }
    }

    const std::vector<uint8_t> frame = wrapDevicePayload(data, size);
    DataCallback callback;
    {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        callback = mDataCallback;
    }
    if (callback && !frame.empty()) {
        callback(frame);
    }
}

void KeyboardController::onDeviceError(int error) {
    {
        std::lock_guard<std::mutex> lock(mStateMutex);
        mHaveStatus = false;
        mHaveProtocolHallState = false;
        mHaveKeyboardIdentity = false;
        mHaveGsensor = false;
        mHaveSleepState = false;
        mKeyboardSleeping = false;
        mStatus.connected = false;
        mInputGateKnown = false;
        mTouchpadStateKnown = false;
    }
    updateInputGate(false);
    ErrorCallback callback;
    {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        callback = mErrorCallback;
    }
    if (callback) {
        callback(error);
    }
}

}  // namespace xiaomi::keyboard
