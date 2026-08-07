/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace xiaomi::keyboard {

constexpr size_t kNanoappRawFrameSize = 66;
constexpr size_t kNanoappMaxPayloadSize = 128;

using RawCommand = std::array<uint8_t, kNanoappRawFrameSize>;

struct StatusReport {
    bool connected = false;
    bool pogoPinHealthy = false;
    bool overCurrent = false;
    bool leatherCaseOrTrxError = false;
    uint8_t keyboardStatus = 0;
    uint16_t batteryMillivolts = 0;
    uint16_t uartErrors = 0;
    uint32_t ppmErrors = 0;
    uint8_t serial = 0;
    uint8_t powerUpCount = 0;
};

struct KeyboardVersionReport {
    uint16_t keyboardVersion = 0;
    uint16_t touchpadVersion = 0;
    uint8_t keyboardInfo = 0;
    uint8_t keyboardType = 0;
    uint8_t touchpadType = 0;
    bool bluetooth = false;
};

struct KeyboardIdentityReport {
    std::array<uint8_t, 6> bluetoothAddress{};
};

struct GsensorReport {
    int16_t rawX = 0;
    int16_t rawY = 0;
    int16_t rawZ = 0;
    float xMetersPerSecondSquared = 0.0f;
    float yMetersPerSecondSquared = 0.0f;
    float zMetersPerSecondSquared = 0.0f;
};

struct SleepReport {
    bool sleeping = false;
};

struct FeatureEffectReport {
    uint8_t command = 0;
    uint8_t value = 0;
};

struct CommandAckReport {
    uint8_t command = 0;
    uint8_t status = 0;
};

enum class IndicatorStyle {
    kLegacy,
    kXm2022,
};

RawCommand makePogoStatusQuery();
RawCommand makeKeyboardVersionQuery();
RawCommand makeMcuVersionQuery();
RawCommand makeHallQuery();
RawCommand makeKeyboardIdentityQuery(const std::array<uint8_t, 6>& localBluetoothAddress = {});
RawCommand makeGsensorQuery();
RawCommand makeFeatureCommand(uint8_t command, uint8_t value);
RawCommand makeCapsLockLightCommand(bool enabled, IndicatorStyle style);
RawCommand makeMuteLightCommand(bool muted, IndicatorStyle style);

bool decodeClientCommand(const std::vector<uint8_t>& command, std::vector<uint8_t>* raw);
std::vector<uint8_t> wrapDevicePayload(const uint8_t* payload, size_t size);

std::optional<StatusReport> parseStatusReport(const uint8_t* payload, size_t size);
std::optional<KeyboardVersionReport> parseKeyboardVersionReport(const uint8_t* payload,
                                                                size_t size);
std::optional<std::string> parseMcuVersionReport(const uint8_t* payload, size_t size);
std::optional<uint8_t> parseHallReport(const uint8_t* payload, size_t size);
std::optional<KeyboardIdentityReport> parseKeyboardIdentityReport(const uint8_t* payload,
                                                                  size_t size);
std::optional<GsensorReport> parseGsensorReport(const uint8_t* payload, size_t size);
std::optional<SleepReport> parseSleepReport(const uint8_t* payload, size_t size);
std::optional<FeatureEffectReport> parseFeatureEffectReport(const uint8_t* payload, size_t size);
std::optional<CommandAckReport> parseCommandAckReport(const uint8_t* payload, size_t size);

std::string toHex(const uint8_t* data, size_t size);

}  // namespace xiaomi::keyboard
