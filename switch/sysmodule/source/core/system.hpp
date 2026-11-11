#pragma once

#include <switch.h>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace sys {
inline uint64_t nowMs() { return armTicksToNs(armGetSystemTick()) / 1000000ull; }
inline void sleepMs(uint64_t ms) { svcSleepThread(static_cast<int64_t>(ms) * 1000000ll); }

void logOpen();
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

bool spawn(std::function<void()> fn, int priority, size_t stackSize, const char* name);

class Lock {
public:
    explicit Lock(Mutex& m) : m_(m) { mutexLock(&m_); }
    ~Lock() { mutexUnlock(&m_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    Mutex& m_;
};
}

#define LOG(...) ::sys::log(__VA_ARGS__)
