#pragma once

#include <switch.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

class GameCapture;
class ViewerLink;

class ScreenCapture {
public:
    ScreenCapture(ViewerLink& link, const GameCapture& game) : link_(link), game_(game) {}
    bool start();

    bool gameSuspended() const { return suspended_.load(); }

private:
    static constexpr uint64_t kGameActiveWindowMs = 150;
    static constexpr uint64_t kStillGameMs = 2000;
    static constexpr uint64_t kIdlePollMs = 16;
    static constexpr uint64_t kResendMs = 1000;
    static constexpr uint64_t kStatsEveryMs = 15000;

    void captureLoop();
    void sendLoop();

    ViewerLink& link_;
    const GameCapture& game_;
    std::atomic<bool> suspended_{false};

    Mutex mutex_;
    CondVar cv_;
    bool pending_ = false;
    size_t outgoingSize_ = 0;
    uint64_t outgoingTs_ = 0;
};
