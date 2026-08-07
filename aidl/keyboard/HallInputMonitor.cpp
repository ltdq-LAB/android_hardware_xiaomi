/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vendor.lineage.keyboard-service.xiaomi"

#include "HallInputMonitor.h"

#include <android-base/logging.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace xiaomi::keyboard {
namespace {

constexpr char kInputDirectory[] = "/dev/input";
constexpr auto kRescanInterval = std::chrono::seconds(1);
constexpr size_t kBitsPerWord = sizeof(unsigned long) * 8;
constexpr size_t kSwitchWordCount = (SW_MAX / kBitsPerWord) + 1;

bool testBit(const std::array<unsigned long, kSwitchWordCount>& bits, unsigned int bit) {
    return (bits[bit / kBitsPerWord] & (1UL << (bit % kBitsPerWord))) != 0;
}

}  // namespace

HallInputMonitor::HallInputMonitor(StateCallback callback) : mCallback(std::move(callback)) {
    int pipeFds[2];
    if (pipe2(pipeFds, O_CLOEXEC | O_NONBLOCK) == 0) {
        mStopReadFd.reset(pipeFds[0]);
        mStopWriteFd.reset(pipeFds[1]);
    } else {
        PLOG(ERROR) << "Failed to create Hall monitor stop pipe";
    }
}

HallInputMonitor::~HallInputMonitor() {
    stop();
}

void HallInputMonitor::start() {
    bool expected = false;
    if (!mStarted.compare_exchange_strong(expected, true)) {
        return;
    }
    mStopping = false;
    mThread = std::thread(&HallInputMonitor::run, this);
}

void HallInputMonitor::stop() {
    if (!mStarted.exchange(false)) {
        return;
    }

    mStopping = true;
    if (mStopWriteFd.ok()) {
        const uint8_t value = 1;
        if (write(mStopWriteFd.get(), &value, sizeof(value)) < 0 && errno != EAGAIN) {
            PLOG(WARNING) << "Failed to interrupt Hall input monitor";
        }
    }
    if (mThread.joinable()) {
        mThread.join();
    }
}

android::base::unique_fd HallInputMonitor::findSwitchDevice(bool* lidOpen, bool* tabletOpen) {
    DIR* rawDirectory = opendir(kInputDirectory);
    if (rawDirectory == nullptr) {
        return {};
    }
    std::unique_ptr<DIR, decltype(&closedir)> directory(rawDirectory, closedir);

    while (const dirent* entry = readdir(directory.get())) {
        if (std::strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        const std::string path = std::string(kInputDirectory) + "/" + entry->d_name;
        android::base::unique_fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
        if (!fd.ok()) {
            continue;
        }

        // The Nanosic driver also advertises SW_LID/SW_TABLET_MODE on its
        // wakeup-only input device but never reports those switches. Select
        // the real gpio-keys device explicitly before looking at capabilities.
        std::array<char, 128> name{};
        if (ioctl(fd.get(), EVIOCGNAME(name.size()), name.data()) < 0 ||
            std::strcmp(name.data(), "gpio-keys") != 0) {
            continue;
        }

        std::array<unsigned long, kSwitchWordCount> supported{};
        if (ioctl(fd.get(), EVIOCGBIT(EV_SW, sizeof(supported)), supported.data()) < 0 ||
            !testBit(supported, SW_LID) || !testBit(supported, SW_TABLET_MODE)) {
            continue;
        }

        std::array<unsigned long, kSwitchWordCount> state{};
        if (ioctl(fd.get(), EVIOCGSW(sizeof(state)), state.data()) < 0) {
            continue;
        }

        // gpio-keys reports a set switch bit while the active-low Hall input
        // is asserted (cover closed/folded). Android's open state is inverse.
        *lidOpen = !testBit(state, SW_LID);
        *tabletOpen = !testBit(state, SW_TABLET_MODE);

        LOG(INFO) << "Monitoring Hall switches on " << name.data() << " (" << path << ")";
        return fd;
    }
    return {};
}

void HallInputMonitor::run() {
    while (!mStopping) {
        bool lidOpen = false;
        bool tabletOpen = false;
        android::base::unique_fd inputFd = findSwitchDevice(&lidOpen, &tabletOpen);
        if (!inputFd.ok()) {
            if (mStopReadFd.ok()) {
                pollfd stopPoll = {.fd = mStopReadFd.get(), .events = POLLIN, .revents = 0};
                TEMP_FAILURE_RETRY(poll(&stopPoll, 1, kRescanInterval.count() * 1000));
            } else {
                std::this_thread::sleep_for(kRescanInterval);
            }
            continue;
        }

        mCallback(true, lidOpen, tabletOpen);
        bool rescan = false;
        while (!mStopping && !rescan) {
            pollfd polls[2] = {
                    {.fd = mStopReadFd.get(), .events = POLLIN, .revents = 0},
                    {.fd = inputFd.get(), .events = POLLIN, .revents = 0},
            };
            const nfds_t pollCount = mStopReadFd.ok() ? 2 : 1;
            pollfd* firstPoll = mStopReadFd.ok() ? polls : &polls[1];
            const int rc = TEMP_FAILURE_RETRY(poll(firstPoll, pollCount, 1000));
            if (rc < 0) {
                PLOG(WARNING) << "Failed to poll Hall input device";
                rescan = true;
                continue;
            }
            if (rc == 0) {
                // Keep a valid, unchanged switch snapshot fresh so the
                // controller can distinguish it from a stalled data source.
                mCallback(true, lidOpen, tabletOpen);
                continue;
            }
            if (mStopReadFd.ok() && (polls[0].revents & POLLIN)) {
                return;
            }
            if (polls[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                rescan = true;
                continue;
            }
            if (!(polls[1].revents & POLLIN)) {
                continue;
            }

            std::array<input_event, 8> events{};
            const ssize_t bytes = TEMP_FAILURE_RETRY(
                    read(inputFd.get(), events.data(), events.size() * sizeof(input_event)));
            if (bytes <= 0) {
                if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    continue;
                }
                rescan = true;
                continue;
            }

            bool changed = false;
            const size_t count = static_cast<size_t>(bytes) / sizeof(input_event);
            for (size_t i = 0; i < count; ++i) {
                if (events[i].type != EV_SW) {
                    continue;
                }
                if (events[i].code == SW_LID) {
                    lidOpen = events[i].value == 0;
                    changed = true;
                } else if (events[i].code == SW_TABLET_MODE) {
                    tabletOpen = events[i].value == 0;
                    changed = true;
                }
            }
            if (changed) {
                mCallback(true, lidOpen, tabletOpen);
            }
        }
        if (rescan) {
            mCallback(false, false, false);
        }
    }
}

}  // namespace xiaomi::keyboard
