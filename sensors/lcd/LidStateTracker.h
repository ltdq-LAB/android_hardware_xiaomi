/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

namespace android {
namespace hardware {
namespace sensors {
namespace V2_1 {
namespace subhal {
namespace implementation {

/**
 * Tracks the keyboard-cover Hall switch (SW_LID) reported by gpio-keys.
 *
 * The open cover is the precondition for the LCD wake gesture chains:
 * with the cover closed, pen click, double tap and pen pickup must not
 * wake the device.
 */
class LidStateTracker {
  public:
    static LidStateTracker& get();

    /**
     * Returns true while the cover is open, or when the lid switch cannot
     * be determined. Wake gestures are never suppressed by an unknown lid.
     */
    bool isLidOpen();

  private:
    LidStateTracker() = default;

    int findLidFd();

    int mLidFd = -1;
};

}  // namespace implementation
}  // namespace subhal
}  // namespace V2_1
}  // namespace sensors
}  // namespace hardware
}  // namespace android
