/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "TouchFeatureHalService"

#include "TouchFeature.h"

#include <hidl/HidlTransportSupport.h>
#include <log/log.h>

using ::android::OK;
using ::android::sp;
using ::android::hardware::configureRpcThreadpool;
using ::android::hardware::joinRpcThreadpool;
using ::vendor::xiaomi::hw::touchfeature::V1_0::ITouchFeature;
using ::vendor::xiaomi::hw::touchfeature::V1_0::implementation::TouchFeature;

int main() {
    configureRpcThreadpool(1, true);

    sp<ITouchFeature> service = new TouchFeature();
    if (service->registerAsService("default") != OK) {
        ALOGE("failed to register ITouchFeature/default");
        return 1;
    }

    joinRpcThreadpool();
    return 1;
}
