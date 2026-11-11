#pragma once

#include <atomic>

#include "core/config.hpp"

namespace power {
inline std::atomic<bool> sleeping{false};
inline std::atomic<bool> audrecBusy{false};
inline std::atomic<bool> screenBusy{false};

inline bool halted() { return !settings::enabled.load(); }
inline bool captureHalted() { return halted() || sleeping.load(); }
inline bool captureIdle() { return !audrecBusy.load() && !screenBusy.load(); }

bool start();
}
