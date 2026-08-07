/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "../v2/Sensor.h"
#include "LidStateTracker.h"

namespace android {
namespace hardware {
namespace sensors {
namespace V2_1 {
namespace subhal {
namespace implementation {

/*
 * The LCD wake gesture chains are gated on the keyboard-cover Hall switch:
 * with the cover closed, pen click, double tap and pen pickup must not wake
 * the device, so the open cover is the precondition for these wake paths.
 */
class LidGatedOneShotSensor : public SysfsPollingOneShotSensor {
  public:
    LidGatedOneShotSensor(int32_t sensorHandle, ISensorsEventCallback* callback,
                          const std::string& pollPath, const std::string& enablePath,
                          const std::string& name, const std::string& typeAsString,
                          SensorType type)
        : SysfsPollingOneShotSensor(sensorHandle, callback, pollPath, enablePath, name,
                                    typeAsString, type) {}

    bool shouldDeliverEvent() override { return LidStateTracker::get().isLidOpen(); }
};

class LcdDoubleTapSensor : public LidGatedOneShotSensor {
  public:
    LcdDoubleTapSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
        : LidGatedOneShotSensor(
                  sensorHandle, callback, "/sys/class/touch/touch_dev/gesture_double_tap_state",
                  "/sys/class/touch/touch_dev/gesture_double_tap_subscribed",
                  "Double Tap Sensor", "org.lineageos.sensor.double_tap",
                  static_cast<SensorType>(static_cast<int32_t>(SensorType::DEVICE_PRIVATE_BASE) +
                                          1)) {}
};

class LcdSingleTapSensor : public LidGatedOneShotSensor {
  public:
    LcdSingleTapSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
        : LidGatedOneShotSensor(
                  sensorHandle, callback, "/sys/class/touch/touch_dev/gesture_single_tap_state",
                  "/sys/class/touch/touch_dev/gesture_single_tap_subscribed",
                  "Stylus Quick Note Sensor", "org.lineageos.sensor.stylus_quick_note",
                  static_cast<SensorType>(static_cast<int32_t>(SensorType::DEVICE_PRIVATE_BASE) +
                                          2)) {}
};

// Pen charging Hall edges use the wake sensor; cover/keyboard Hall stays on SW_LID.
class LcdPenDetachSensor : public LidGatedOneShotSensor {
  public:
    LcdPenDetachSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
        : LidGatedOneShotSensor(sensorHandle, callback,
                                "/sys/class/touch/touch_dev/gesture_pen_detach_state",
                                "/sys/class/touch/touch_dev/gesture_pen_detach_enabled",
                                "Pen Detach Wake Gesture", "android.sensor.wake_gesture",
                                SensorType::WAKE_GESTURE) {
        mSensorInfo.maxRange = 1.0f;
    }

    void fillEventData(Event& event) override { event.u.scalar = 1; }
};

}  // namespace implementation
}  // namespace subhal
}  // namespace V2_1
}  // namespace sensors
}  // namespace hardware
}  // namespace android
