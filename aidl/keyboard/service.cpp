/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vendor.lineage.keyboard-service.xiaomi"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <cstdlib>
#include <memory>
#include <string>

#include "KeyboardNanoapp.h"

using aidl::vendor::xiaomi::hardware::keyboardnanoapp_aidl::IKeyboardNanoapp_aidl;
using xiaomi::keyboard::KeyboardNanoapp;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(4);

    std::shared_ptr<KeyboardNanoapp> service = ndk::SharedRefBase::make<KeyboardNanoapp>();
    const std::string instance = std::string(IKeyboardNanoapp_aidl::descriptor) + "/default";
    const binder_status_t status =
            AServiceManager_addService(service->asBinder().get(), instance.c_str());
    CHECK_EQ(status, STATUS_OK) << "Failed to register " << instance;
    LOG(INFO) << "Registered " << instance;

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
