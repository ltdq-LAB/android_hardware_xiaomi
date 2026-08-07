/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <vendor/xiaomi/hw/touchfeature/1.0/ITouchFeature.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace vendor::xiaomi::hw::touchfeature::V1_0::implementation {

class TouchFeature final : public ITouchFeature {
  public:
    TouchFeature();

    ::android::hardware::Return<int32_t> setModeValue(int32_t touchId, int32_t ControlMode,
                                                      int32_t ModeValue) override;
    ::android::hardware::Return<int32_t> getModeCurValue(int32_t touchId,
                                                         int32_t ControlMode) override;
    ::android::hardware::Return<int32_t> getModeMaxValue(int32_t touchId,
                                                         int32_t ControlMode) override;
    ::android::hardware::Return<int32_t> getModeMinValue(int32_t touchId,
                                                         int32_t ControlMode) override;
    ::android::hardware::Return<int32_t> getModeDefaultValue(int32_t touchId,
                                                             int32_t ControlMode) override;
    ::android::hardware::Return<int32_t> modeReset(int32_t touchId, int32_t ControlMode) override;
    ::android::hardware::Return<void> getModeValue(int32_t touchId, int32_t mode,
                                                   getModeValue_cb _hidl_cb) override;
    ::android::hardware::Return<int32_t> setModeLongValue(
            int32_t touchId, int32_t ControlMode, int32_t ValueLen,
            const ::android::hardware::hidl_vec<int32_t>& ValueBuf) override;
    ::android::hardware::Return<void> getTouchEvent(getTouchEvent_cb _hidl_cb) override;
    ::android::hardware::Return<void> getHashChain(getHashChain_cb _hidl_cb) override;

  private:
    static constexpr std::size_t kStockModeCount = 25;

    int32_t getCurrentValueLocked(int32_t mode);
    int32_t setModeValueLocked(int32_t mode, int32_t value);
    int32_t resetModeLocked(int32_t mode);

    std::array<int32_t, kStockModeCount> currentValues_;
    std::mutex lock_;
};

}  // namespace vendor::xiaomi::hw::touchfeature::V1_0::implementation
