/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/vendor/xiaomi/hardware/keyboardnanoapp_aidl/BnKeyboardNanoapp_aidl.h>
#include <aidl/vendor/xiaomi/hardware/keyboardnanoapp_aidl/INanoappCallback_aidl.h>

#include <memory>
#include <mutex>
#include <vector>

#include "KeyboardController.h"

namespace xiaomi::keyboard {

class KeyboardNanoapp
    : public aidl::vendor::xiaomi::hardware::keyboardnanoapp_aidl::BnKeyboardNanoapp_aidl {
  public:
    KeyboardNanoapp();
    ~KeyboardNanoapp() override;

    ndk::ScopedAStatus sendCmd_aidl(const std::vector<uint8_t>& buf,
                                    int32_t* _aidl_return) override;
    ndk::ScopedAStatus setCallback_aidl(
            const std::shared_ptr<
                    aidl::vendor::xiaomi::hardware::keyboardnanoapp_aidl::INanoappCallback_aidl>&
                    callback) override;

  private:
    using Callback = aidl::vendor::xiaomi::hardware::keyboardnanoapp_aidl::INanoappCallback_aidl;

    void sendData(const std::vector<uint8_t>& data);
    void sendError(int error);
    void clearDeadCallback(const std::shared_ptr<Callback>& callback);

    KeyboardController mController;
    std::mutex mDispatchMutex;
    std::mutex mCallbackMutex;
    std::shared_ptr<Callback> mCallback;
};

}  // namespace xiaomi::keyboard
