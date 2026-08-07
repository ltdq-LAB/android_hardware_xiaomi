/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vendor.lineage.keyboard-client"

#include <aidl/vendor/xiaomi/hardware/keyboardnanoapp_aidl/BnNanoappCallback_aidl.h>
#include <aidl/vendor/xiaomi/hardware/keyboardnanoapp_aidl/IKeyboardNanoapp_aidl.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Protocol.h"

using aidl::vendor::xiaomi::hardware::keyboardnanoapp_aidl::BnNanoappCallback_aidl;
using aidl::vendor::xiaomi::hardware::keyboardnanoapp_aidl::IKeyboardNanoapp_aidl;
using xiaomi::keyboard::RawCommand;

namespace {

constexpr uint32_t kResponseStatus = 1U << 0;
constexpr uint32_t kResponseKeyboardVersion = 1U << 1;
constexpr uint32_t kResponseMcuVersion = 1U << 2;
constexpr uint32_t kResponseHall = 1U << 3;
constexpr uint32_t kResponseIdentity = 1U << 4;
constexpr uint32_t kResponseGsensor = 1U << 5;
constexpr uint32_t kResponseSleep = 1U << 6;

class ClientCallback : public BnNanoappCallback_aidl {
  public:
    ndk::ScopedAStatus dataReceive_aidl(const std::vector<uint8_t>& buf) override {
        const uint32_t response = printFrame(buf);
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mFrames.push_back(buf);
            mSeenResponses |= response;
        }
        mCondition.notify_all();
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus errorReceive_aidl(int32_t errorCode) override {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mLastError = errorCode;
        }
        std::cerr << "nanoapp error: " << errorCode << '\n';
        mCondition.notify_all();
        return ndk::ScopedAStatus::ok();
    }

    bool waitForResponses(uint32_t requiredResponses, size_t fallbackCount,
                          std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mMutex);
        const auto received = [&] {
            const bool complete = requiredResponses != 0 ? (mSeenResponses & requiredResponses) ==
                                                                   requiredResponses
                                                         : mFrames.size() >= fallbackCount;
            return complete || mLastError != 0;
        };
        if (!mCondition.wait_for(lock, timeout, received) || mLastError != 0) {
            return false;
        }
        return requiredResponses != 0 ? (mSeenResponses & requiredResponses) == requiredResponses
                                      : mFrames.size() >= fallbackCount;
    }

    bool waitForFeatureResult(uint8_t command, uint8_t value, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mMutex);
        const auto result = [&] {
            for (const auto& frame : mFrames) {
                if (frame.size() < 3 || frame[0] != 0xaa ||
                    static_cast<size_t>(frame[1]) + 2 > frame.size()) {
                    continue;
                }
                const auto ack =
                        xiaomi::keyboard::parseCommandAckReport(frame.data() + 2, frame[1]);
                if (ack && ack->command == command) {
                    return ack->status == 0 ? 1 : -1;
                }
                const auto effect =
                        xiaomi::keyboard::parseFeatureEffectReport(frame.data() + 2, frame[1]);
                if (effect && effect->command == command && effect->value == value) {
                    return 1;
                }
            }
            return mLastError != 0 ? -1 : 0;
        };
        if (!mCondition.wait_for(lock, timeout, [&] { return result() != 0; }) || mLastError != 0) {
            return false;
        }
        return result() > 0;
    }

  private:
    static uint32_t printFrame(const std::vector<uint8_t>& frame) {
        std::cout << xiaomi::keyboard::toHex(frame.data(), frame.size()) << '\n';
        if (frame.size() < 3 || frame[0] != 0xaa ||
            static_cast<size_t>(frame[1]) + 2 > frame.size()) {
            return 0;
        }

        const uint8_t* payload = frame.data() + 2;
        const size_t size = frame[1];
        if (const auto status = xiaomi::keyboard::parseStatusReport(payload, size)) {
            std::cout << "connected=" << status->connected << " pogo_ok=" << status->pogoPinHealthy
                      << " over_current=" << status->overCurrent << " keyboard_status=0x"
                      << std::hex << static_cast<int>(status->keyboardStatus) << std::dec
                      << " battery_mv=" << status->batteryMillivolts
                      << " uart_errors=" << status->uartErrors
                      << " ppm_errors=" << status->ppmErrors << '\n';
            return kResponseStatus;
        } else if (const auto version =
                           xiaomi::keyboard::parseKeyboardVersionReport(payload, size)) {
            std::cout << "keyboard_version=0x" << std::hex << version->keyboardVersion
                      << " touchpad_version=0x" << version->touchpadVersion << std::dec
                      << " keyboard_type=" << static_cast<int>(version->keyboardType)
                      << " touchpad_type=" << static_cast<int>(version->touchpadType)
                      << " bluetooth=" << version->bluetooth << '\n';
            return kResponseKeyboardVersion;
        } else if (const auto version = xiaomi::keyboard::parseMcuVersionReport(payload, size)) {
            std::cout << "mcu_version=" << *version << '\n';
            return kResponseMcuVersion;
        } else if (const auto hall = xiaomi::keyboard::parseHallReport(payload, size)) {
            std::cout << "hall_n=" << ((*hall >> 4) & 1) << " hall_s=" << (*hall & 1) << '\n';
            return kResponseHall;
        } else if (const auto identity =
                           xiaomi::keyboard::parseKeyboardIdentityReport(payload, size)) {
            std::cout << "keyboard_mac=";
            for (size_t i = 0; i < identity->bluetoothAddress.size(); ++i) {
                if (i != 0) std::cout << ':';
                std::cout << std::hex << std::setfill('0') << std::setw(2)
                          << static_cast<int>(identity->bluetoothAddress[i]);
            }
            std::cout << std::dec << '\n';
            return kResponseIdentity;
        } else if (const auto gsensor = xiaomi::keyboard::parseGsensorReport(payload, size)) {
            std::cout << "gsensor_m_s2=" << gsensor->xMetersPerSecondSquared << ','
                      << gsensor->yMetersPerSecondSquared << ',' << gsensor->zMetersPerSecondSquared
                      << " raw=" << gsensor->rawX << ',' << gsensor->rawY << ',' << gsensor->rawZ
                      << '\n';
            return kResponseGsensor;
        } else if (const auto sleep = xiaomi::keyboard::parseSleepReport(payload, size)) {
            std::cout << "sleeping=" << sleep->sleeping << '\n';
            return kResponseSleep;
        } else if (const auto effect = xiaomi::keyboard::parseFeatureEffectReport(payload, size)) {
            std::cout << "feature_command=0x" << std::hex << static_cast<int>(effect->command)
                      << " value=0x" << static_cast<int>(effect->value) << std::dec << '\n';
        } else if (const auto ack = xiaomi::keyboard::parseCommandAckReport(payload, size)) {
            std::cout << "ack_command=0x" << std::hex << static_cast<int>(ack->command)
                      << " status=0x" << static_cast<int>(ack->status) << std::dec << '\n';
        }
        return 0;
    }

    std::mutex mMutex;
    std::condition_variable mCondition;
    std::vector<std::vector<uint8_t>> mFrames;
    uint32_t mSeenResponses = 0;
    int mLastError = 0;
};

void usage(const char* program) {
    std::cerr << "Usage: " << program << " COMMAND [VALUE]\n"
              << "  status                 query pogo/keyboard status\n"
              << "  version                query keyboard, touchpad and MCU versions\n"
              << "  hall                   query keyboard-cover Hall GPIOs\n"
              << "  identity               query the keyboard Bluetooth address\n"
              << "  gsensor                request one keyboard GSensor sample\n"
              << "  touchpad 0|1           disable or enable the touchpad\n"
              << "  backlight 0..100       set keyboard backlight level\n"
              << "  power 0|1              request keyboard sleep (0) or wake (1)\n"
              << "  caps STYLE 0|1         set Caps indicator; STYLE is legacy or xm2022\n"
              << "  mute STYLE 0|1         set mute indicator; STYLE is legacy or xm2022\n"
              << "  raw BYTE...            send hexadecimal bytes (AA framing optional)\n";
}

bool parseByte(const char* value, uint8_t* result, int base = 0) {
    if (value == nullptr || result == nullptr) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value, &end, base);
    if (errno != 0 || end == value || *end != '\0' || parsed > 0xff) {
        return false;
    }
    *result = static_cast<uint8_t>(parsed);
    return true;
}

std::vector<uint8_t> binderFrame(const RawCommand& raw) {
    std::vector<uint8_t> frame;
    frame.reserve(raw.size() + 2);
    frame.push_back(0xaa);
    frame.push_back(static_cast<uint8_t>(raw.size()));
    frame.insert(frame.end(), raw.begin(), raw.end());
    return frame;
}

bool send(const std::shared_ptr<IKeyboardNanoapp_aidl>& service, const RawCommand& raw) {
    int32_t result = 0;
    const ndk::ScopedAStatus status = service->sendCmd_aidl(binderFrame(raw), &result);
    if (!status.isOk()) {
        std::cerr << "Binder error: " << status.getDescription() << '\n';
        return false;
    }
    if (result != static_cast<int32_t>(raw.size())) {
        std::cerr << "write failed: " << result << '\n';
        return false;
    }
    return true;
}

bool parseIndicatorStyle(const char* value, xiaomi::keyboard::IndicatorStyle* style) {
    if (value == nullptr || style == nullptr) {
        return false;
    }
    if (std::string(value) == "legacy") {
        *style = xiaomi::keyboard::IndicatorStyle::kLegacy;
        return true;
    }
    if (std::string(value) == "xm2022") {
        *style = xiaomi::keyboard::IndicatorStyle::kXm2022;
        return true;
    }
    return false;
}

bool validRawCommand(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) {
        return false;
    }
    if (bytes[0] == 0x32) {
        return bytes.size() <= xiaomi::keyboard::kNanoappRawFrameSize;
    }
    if (bytes.size() < 3 || bytes[0] != 0xaa || bytes[1] == 0 ||
        bytes[1] > xiaomi::keyboard::kNanoappRawFrameSize) {
        return false;
    }
    return bytes.size() == static_cast<size_t>(bytes[1]) + 2 && bytes[2] == 0x32;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    ABinderProcess_setThreadPoolMaxThreadCount(1);
    ABinderProcess_startThreadPool();
    const std::string instance = std::string(IKeyboardNanoapp_aidl::descriptor) + "/default";
    std::shared_ptr<IKeyboardNanoapp_aidl> service = IKeyboardNanoapp_aidl::fromBinder(
            ndk::SpAIBinder(AServiceManager_waitForService(instance.c_str())));
    if (!service) {
        std::cerr << "Service unavailable: " << instance << '\n';
        return EXIT_FAILURE;
    }

    std::shared_ptr<ClientCallback> callback = ndk::SharedRefBase::make<ClientCallback>();
    const ndk::ScopedAStatus callbackStatus = service->setCallback_aidl(callback);
    if (!callbackStatus.isOk()) {
        std::cerr << "Unable to register callback: " << callbackStatus.getDescription() << '\n';
        return EXIT_FAILURE;
    }

    const std::string command = argv[1];
    size_t expectedFrames = 1;
    uint32_t requiredResponses = 0;
    bool waitForResponse = true;
    bool waitForEffect = false;
    uint8_t expectedEffectCommand = 0;
    uint8_t expectedEffectValue = 0;
    bool ok = false;
    if (command == "status") {
        if (argc != 2) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        requiredResponses = kResponseStatus;
        ok = send(service, xiaomi::keyboard::makePogoStatusQuery());
    } else if (command == "version") {
        if (argc != 2) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        expectedFrames = 2;
        requiredResponses = kResponseKeyboardVersion | kResponseMcuVersion;
        ok = send(service, xiaomi::keyboard::makeKeyboardVersionQuery()) &&
             send(service, xiaomi::keyboard::makeMcuVersionQuery());
    } else if (command == "hall") {
        if (argc != 2) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        requiredResponses = kResponseHall;
        ok = send(service, xiaomi::keyboard::makeHallQuery());
    } else if (command == "identity") {
        if (argc != 2) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        requiredResponses = kResponseIdentity;
        ok = send(service, xiaomi::keyboard::makeKeyboardIdentityQuery());
    } else if (command == "gsensor") {
        if (argc != 2) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        requiredResponses = kResponseGsensor;
        ok = send(service, xiaomi::keyboard::makeGsensorQuery());
    } else if (command == "touchpad" || command == "backlight" || command == "power") {
        uint8_t value;
        if (argc != 3 || !parseByte(argv[2], &value, 10)) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        if ((command == "backlight" && value > 100) || (command != "backlight" && value > 1)) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        uint8_t feature = 0;
        if (command == "touchpad") feature = 0x21;
        if (command == "backlight") feature = 0x23;
        if (command == "power") feature = 0x25;
        ok = send(service, xiaomi::keyboard::makeFeatureCommand(feature, value));
        if (command == "backlight") {
            waitForEffect = true;
            expectedEffectCommand = feature;
            expectedEffectValue = value;
        } else {
            // 0x21 and 0x25 have no reliable effect notification. A complete
            // 66-byte transport write is the only synchronous acknowledgement.
            waitForResponse = false;
        }
    } else if (command == "caps" || command == "mute") {
        xiaomi::keyboard::IndicatorStyle style;
        uint8_t value;
        if (argc != 4 || !parseIndicatorStyle(argv[2], &style) || !parseByte(argv[3], &value, 10) ||
            value > 1) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        const RawCommand raw =
                command == "caps" ? xiaomi::keyboard::makeCapsLockLightCommand(value != 0, style)
                                  : xiaomi::keyboard::makeMuteLightCommand(value != 0, style);
        expectedEffectCommand = raw[6];
        expectedEffectValue = raw[8];
        waitForEffect = true;
        ok = send(service, raw);
    } else if (command == "raw") {
        if (argc < 3) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        std::vector<uint8_t> bytes;
        for (int i = 2; i < argc; ++i) {
            uint8_t value;
            if (!parseByte(argv[i], &value, 16)) {
                std::cerr << "Invalid hexadecimal byte: " << argv[i] << '\n';
                return EXIT_FAILURE;
            }
            bytes.push_back(value);
        }
        if (!validRawCommand(bytes)) {
            std::cerr
                    << "Raw command must be a 1..66 byte 32 payload or an exact AA length frame\n";
            return EXIT_FAILURE;
        }
        int32_t result = 0;
        const ndk::ScopedAStatus status = service->sendCmd_aidl(bytes, &result);
        ok = status.isOk() && result > 0;
        if (!ok) {
            std::cerr << (status.isOk() ? "write failed: " + std::to_string(result)
                                        : status.getDescription())
                      << '\n';
        }
    } else {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!ok) {
        return EXIT_FAILURE;
    }
    if (!waitForResponse) {
        return EXIT_SUCCESS;
    }
    const bool received =
            waitForEffect
                    ? callback->waitForFeatureResult(expectedEffectCommand, expectedEffectValue,
                                                     std::chrono::milliseconds(1500))
                    : callback->waitForResponses(requiredResponses, expectedFrames,
                                                 std::chrono::milliseconds(1500));
    if (!received) {
        std::cerr << "No successful matching response\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
