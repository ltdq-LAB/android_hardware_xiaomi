/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vendor.lineage.keyboard-service.xiaomi"

#include "KeyboardNanoapp.h"

#include <android-base/logging.h>

#include <cerrno>

namespace xiaomi::keyboard {

KeyboardNanoapp::KeyboardNanoapp() {
    mController.setCallbacks([this](const std::vector<uint8_t>& data) { sendData(data); },
                             [this](int error) { sendError(error); });
    mController.start();
}

KeyboardNanoapp::~KeyboardNanoapp() {
    mController.stop();
    mController.setCallbacks({}, {});
}

ndk::ScopedAStatus KeyboardNanoapp::sendCmd_aidl(const std::vector<uint8_t>& buf,
                                                 int32_t* _aidl_return) {
    if (_aidl_return == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }

    *_aidl_return = mController.sendClientCommand(buf);
    if (*_aidl_return < 0) {
        sendError(*_aidl_return);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus KeyboardNanoapp::setCallback_aidl(const std::shared_ptr<Callback>& callback) {
    std::lock_guard<std::mutex> dispatchLock(mDispatchMutex);
    std::lock_guard<std::mutex> lock(mCallbackMutex);
    mCallback = callback;
    return ndk::ScopedAStatus::ok();
}

void KeyboardNanoapp::clearDeadCallback(const std::shared_ptr<Callback>& callback) {
    std::lock_guard<std::mutex> lock(mCallbackMutex);
    if (mCallback == callback) {
        mCallback.reset();
    }
}

void KeyboardNanoapp::sendData(const std::vector<uint8_t>& data) {
    // Preserve byte-stream ordering when device and Binder error paths notify
    // concurrently, and prevent a replaced callback from being invoked later.
    std::lock_guard<std::mutex> dispatchLock(mDispatchMutex);
    std::shared_ptr<Callback> callback;
    {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        callback = mCallback;
    }
    if (!callback) {
        return;
    }

    const ndk::ScopedAStatus status = callback->dataReceive_aidl(data);
    if (!status.isOk()) {
        LOG(WARNING) << "Removing dead keyboard callback: " << status.getDescription();
        clearDeadCallback(callback);
    }
}

void KeyboardNanoapp::sendError(int error) {
    std::lock_guard<std::mutex> dispatchLock(mDispatchMutex);
    std::shared_ptr<Callback> callback;
    {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        callback = mCallback;
    }
    if (!callback) {
        return;
    }

    const ndk::ScopedAStatus status = callback->errorReceive_aidl(error);
    if (!status.isOk()) {
        LOG(WARNING) << "Removing dead keyboard callback: " << status.getDescription();
        clearDeadCallback(callback);
    }
}

}  // namespace xiaomi::keyboard
