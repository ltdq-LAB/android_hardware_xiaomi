/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Protocol.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace xiaomi::keyboard {
namespace {

constexpr uint8_t kTransportReport = 0x32;
constexpr uint8_t kShortReport = 0x4e;
constexpr uint8_t kLongReport = 0x4f;
constexpr uint8_t kProtocolVersion = 0x31;
constexpr uint8_t kVersionProtocol = 0x30;
constexpr uint8_t kPadAddress = 0x80;
constexpr uint8_t kMcuAddress = 0x18;
constexpr uint8_t kKeyboardAddress = 0x38;
constexpr float kStandardGravity = 9.8f;

int16_t signExtend12(uint16_t value) {
    value &= 0x0fff;
    return static_cast<int16_t>((value & 0x0800) != 0 ? value | 0xf000 : value);
}

uint8_t checksum(const uint8_t* begin, const uint8_t* end) {
    uint8_t value = 0;
    while (begin != end) {
        value = static_cast<uint8_t>(value + *begin++);
    }
    return value;
}

RawCommand makeCommand(uint8_t report, uint8_t protocol, uint8_t target, uint8_t command,
                       const std::vector<uint8_t>& payload) {
    RawCommand frame{};
    if (payload.size() > frame.size() - 9) {
        return frame;
    }

    frame[0] = kTransportReport;
    frame[1] = 0x00;
    frame[2] = report;
    frame[3] = protocol;
    frame[4] = kPadAddress;
    frame[5] = target;
    frame[6] = command;
    frame[7] = static_cast<uint8_t>(payload.size());
    std::copy(payload.begin(), payload.end(), frame.begin() + 8);
    frame[8 + payload.size()] = checksum(frame.data() + 2, frame.data() + 8 + payload.size());
    return frame;
}

bool hasValidChecksum(const uint8_t* data, size_t size) {
    if (size < 7) {
        return false;
    }

    const size_t checksumIndex = 6 + data[5];
    if (checksumIndex >= size) {
        return false;
    }
    return checksum(data, data + checksumIndex) == data[checksumIndex];
}

bool hasVendorHeader(const uint8_t* data, size_t size, uint8_t protocol, uint8_t source,
                     uint8_t command) {
    if (data == nullptr || size < 5) {
        return false;
    }

    switch (data[0]) {
        case 0x22:
        case 0x23:
        case 0x24:
        case 0x26:
            break;
        default:
            return false;
    }

    return data[1] == protocol && data[2] == source && data[3] == kPadAddress && data[4] == command;
}

bool isVendorResponse(const uint8_t* data, size_t size, uint8_t protocol, uint8_t source,
                      uint8_t command) {
    return hasVendorHeader(data, size, protocol, source, command) && hasValidChecksum(data, size);
}

}  // namespace

RawCommand makePogoStatusQuery() {
    return makeCommand(kShortReport, kProtocolVersion, kKeyboardAddress, 0xa1, {0x01});
}

RawCommand makeKeyboardVersionQuery() {
    return makeCommand(kShortReport, kVersionProtocol, kKeyboardAddress, 0x01, {0x00});
}

RawCommand makeMcuVersionQuery() {
    return makeCommand(kShortReport, kVersionProtocol, kMcuAddress, 0x01, {0x00});
}

RawCommand makeHallQuery() {
    RawCommand frame{};
    frame[0] = kTransportReport;
    frame[1] = 0x00;
    frame[2] = kLongReport;
    frame[3] = 0x20;
    frame[4] = kPadAddress;
    frame[5] = kPadAddress;
    frame[6] = 0xe1;
    frame[7] = 0x01;
    frame[8] = 0x00;
    return frame;
}

RawCommand makeKeyboardIdentityQuery(const std::array<uint8_t, 6>& localBluetoothAddress) {
    return makeCommand(
            kShortReport, kProtocolVersion, kKeyboardAddress, 0x52,
            std::vector<uint8_t>(localBluetoothAddress.begin(), localBluetoothAddress.end()));
}

RawCommand makeGsensorQuery() {
    // The stock kernel calls this zero-length 0x52 form when the display resumes.
    return makeCommand(kShortReport, kProtocolVersion, kKeyboardAddress, 0x52, {});
}

RawCommand makeFeatureCommand(uint8_t command, uint8_t value) {
    return makeCommand(kShortReport, kProtocolVersion, kKeyboardAddress, command, {value});
}

RawCommand makeCapsLockLightCommand(bool enabled, IndicatorStyle style) {
    if (style == IndicatorStyle::kXm2022) {
        return makeFeatureCommand(0x26, enabled ? 0x01 : 0x00);
    }
    return makeFeatureCommand(0x2e, enabled ? 0xfd : 0xfc);
}

RawCommand makeMuteLightCommand(bool muted, IndicatorStyle style) {
    return makeFeatureCommand(style == IndicatorStyle::kXm2022 ? 0x26 : 0x2e, muted ? 0xf7 : 0xf3);
}

bool decodeClientCommand(const std::vector<uint8_t>& command, std::vector<uint8_t>* raw) {
    if (raw == nullptr) {
        return false;
    }

    // Accept the stock Binder framing and the old vendor test client's raw form.
    // The kernel transport consumes fixed 66-byte transfers, so zero-pad shorter
    // raw commands before writing them to the character device.
    if (!command.empty() && command.size() <= kNanoappRawFrameSize &&
        command[0] == kTransportReport) {
        raw->assign(command.begin(), command.end());
        raw->resize(kNanoappRawFrameSize, 0);
        return true;
    }
    if (command.size() < 3 || command[0] != 0xaa) {
        return false;
    }

    const size_t length = command[1];
    if (length == 0 || length > kNanoappRawFrameSize || command.size() != length + 2 ||
        command[2] != kTransportReport) {
        return false;
    }

    raw->assign(command.begin() + 2, command.begin() + 2 + length);
    // nano_chardev deliberately accepts only the controller's fixed 66-byte transfer.
    raw->resize(kNanoappRawFrameSize, 0);
    return true;
}

std::vector<uint8_t> wrapDevicePayload(const uint8_t* payload, size_t size) {
    if (payload == nullptr || size == 0 || size > kNanoappMaxPayloadSize) {
        return {};
    }

    std::vector<uint8_t> frame;
    frame.reserve(size + 2);
    frame.push_back(0xaa);
    frame.push_back(static_cast<uint8_t>(size));
    frame.insert(frame.end(), payload, payload + size);
    return frame;
}

std::optional<StatusReport> parseStatusReport(const uint8_t* data, size_t size) {
    // The last consumed field is d18, so the declared payload (d6 onward)
    // must contain at least 13 bytes. The read buffer may be padded beyond
    // the checksum; never treat that padding as authenticated status data.
    if (data == nullptr || size < 20 || data[5] < 13 ||
        !isVendorResponse(data, size, kProtocolVersion, kKeyboardAddress, 0xa2) || data[7] != 0) {
        return std::nullopt;
    }

    StatusReport report;
    report.keyboardStatus = data[9];
    report.batteryMillivolts = static_cast<uint16_t>(data[10] | (data[11] << 8));
    report.uartErrors = static_cast<uint16_t>(data[12] | (data[13] << 8));
    report.ppmErrors = static_cast<uint32_t>(data[14]) | (static_cast<uint32_t>(data[15]) << 8) |
                       (static_cast<uint32_t>(data[16]) << 16) |
                       (static_cast<uint32_t>(data[17]) << 24);
    report.overCurrent = data[18] != 0;
    report.pogoPinHealthy = (report.keyboardStatus & 0x63) == 0x23;
    report.leatherCaseOrTrxError = (report.keyboardStatus & 0x03) == 0x01;
    report.connected = !report.overCurrent && report.pogoPinHealthy;
    report.serial = data[17] & 0x07;
    report.powerUpCount = (data[17] >> 4) & 0x0f;
    return report;
}

std::optional<KeyboardVersionReport> parseKeyboardVersionReport(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 12 || data[5] < 5 ||
        !isVendorResponse(data, size, kVersionProtocol, kKeyboardAddress, 0x01)) {
        return std::nullopt;
    }

    KeyboardVersionReport report;
    report.keyboardVersion = static_cast<uint16_t>((data[7] << 8) | data[6]);
    if (data[5] != 5 && size > 9) {
        report.touchpadVersion = static_cast<uint16_t>((data[9] << 8) | data[8]);
    }
    report.keyboardInfo = data[10];
    if (data[5] >= 12 && size >= 19) {
        report.keyboardType = data[13];
        report.touchpadType = data[14];
        report.bluetooth = data[17] == 1;
    }
    return report;
}

std::optional<std::string> parseMcuVersionReport(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 24 || data[5] < 17 ||
        !isVendorResponse(data, size, kVersionProtocol, kMcuAddress, 0x01)) {
        return std::nullopt;
    }

    std::string version(reinterpret_cast<const char*>(data + 7), 16);
    while (!version.empty() &&
           (version.back() == '\0' || std::isspace(static_cast<unsigned char>(version.back())))) {
        version.pop_back();
    }
    return version;
}

std::optional<uint8_t> parseHallReport(const uint8_t* data, size_t size) {
    constexpr std::array<uint8_t, 6> kHallPrefix = {0x24, 0x20, 0x80, 0x80, 0xe1, 0x01};
    if (data == nullptr || size < 7 || !std::equal(kHallPrefix.begin(), kHallPrefix.end(), data)) {
        return std::nullopt;
    }
    constexpr uint8_t kHallStateMask = 0x11;
    if ((data[6] & ~kHallStateMask) != 0) {
        return std::nullopt;
    }
    return data[6];
}

std::optional<KeyboardIdentityReport> parseKeyboardIdentityReport(const uint8_t* data,
                                                                  size_t size) {
    constexpr size_t kAddressOffset = 19;
    constexpr size_t kAddressSize = 6;
    if (data == nullptr || size <= kAddressOffset + kAddressSize || data[5] < 19 ||
        !isVendorResponse(data, size, kProtocolVersion, kKeyboardAddress, 0x52)) {
        return std::nullopt;
    }

    KeyboardIdentityReport report;
    std::copy_n(data + kAddressOffset, report.bluetoothAddress.size(),
                report.bluetoothAddress.begin());
    return report;
}

std::optional<GsensorReport> parseGsensorReport(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 13 || data[5] < 6 ||
        !isVendorResponse(data, size, kProtocolVersion, kKeyboardAddress, 0x64)) {
        return std::nullopt;
    }

    GsensorReport report;
    report.rawX = signExtend12(static_cast<uint16_t>((data[7] << 4) | (data[6] >> 4)));
    report.rawY = signExtend12(static_cast<uint16_t>((data[9] << 4) | (data[8] >> 4)));
    report.rawZ = signExtend12(static_cast<uint16_t>((data[11] << 4) | (data[10] >> 4)));
    report.xMetersPerSecondSquared = report.rawX * kStandardGravity / 256.0f;
    report.yMetersPerSecondSquared = -report.rawY * kStandardGravity / 256.0f;
    report.zMetersPerSecondSquared = -report.rawZ * kStandardGravity / 256.0f;
    return report;
}

std::optional<SleepReport> parseSleepReport(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 8 || data[5] != 1 ||
        !isVendorResponse(data, size, kProtocolVersion, kKeyboardAddress, 0x28) || data[6] > 1) {
        return std::nullopt;
    }
    return SleepReport{.sleeping = data[6] == 0};
}

std::optional<FeatureEffectReport> parseFeatureEffectReport(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 8 || data[5] != 1) {
        return std::nullopt;
    }
    switch (data[4]) {
        case 0x23:
        case 0x26:
        case 0x2e:
            break;
        default:
            return std::nullopt;
    }
    if (!isVendorResponse(data, size, kProtocolVersion, kKeyboardAddress, data[4])) {
        return std::nullopt;
    }
    return FeatureEffectReport{.command = data[4], .value = data[6]};
}

std::optional<CommandAckReport> parseCommandAckReport(const uint8_t* data, size_t size) {
    // F0 is a special response: d5 is the original command, not a payload
    // length, and d7 is its status. Do not apply the normal length-derived
    // checksum rule. The stock dispatcher accepts it after the same fixed
    // report/protocol/source/target routing check.
    if (size < 8 || !hasVendorHeader(data, size, kProtocolVersion, kKeyboardAddress, 0xf0)) {
        return std::nullopt;
    }
    return CommandAckReport{.command = data[5], .status = data[7]};
}

std::string toHex(const uint8_t* data, size_t size) {
    if (data == nullptr) {
        return {};
    }

    std::string output;
    output.reserve(size * 3);
    char byte[4];
    for (size_t i = 0; i < size; ++i) {
        std::snprintf(byte, sizeof(byte), "%02X", data[i]);
        if (!output.empty()) {
            output.push_back(' ');
        }
        output.append(byte);
    }
    return output;
}

}  // namespace xiaomi::keyboard
