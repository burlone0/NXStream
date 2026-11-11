#pragma once

#include <switch.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "capture/h264.hpp"

class ViewerLink;
class SystemAudio;

class GameCapture {
public:
    GameCapture(ViewerLink& link, const SystemAudio& systemAudio) : link_(link), systemAudio_(systemAudio) {}

    bool start();
    uint64_t lastVideoMs() const { return lastVideoMs_.load(); }
    uint64_t lastAudioMs() const { return lastAudioMs_.load(); }

private:
    static constexpr size_t kQueueBytes = 0x100000;
    static constexpr size_t kMaxQueued = 32;
    static constexpr uint64_t kMaxSkipMs = 3000;
    static constexpr uint64_t kStatsEveryMs = 15000;

    struct Queued {
        uint32_t offset;
        uint32_t size;
        uint64_t timestamp;
        uint64_t queuedMs;
        bool idr;
    };

    bool openRecorder();
    void videoLoop();
    void sendLoop();
    void audioLoop();
    void noteKeyframeInterval(bool idr);
    void loadParams();
    void saveParams();

    bool enqueue(const h264::FrameInfo& info, uint64_t timestamp, size_t size, bool withParams);
    bool allocate(size_t size, uint32_t& offset) const;
    void clearQueue();
    void logStats(uint64_t now);

    ViewerLink& link_;
    const SystemAudio& systemAudio_;
    Service videoSrv_{};
    Service audioSrv_{};
    std::atomic<bool> ready_{false};
    std::atomic<uint64_t> lastVideoMs_{0};
    std::atomic<uint64_t> lastAudioMs_{0};
    h264::ParameterSets params_;

    Mutex queueMutex_;
    CondVar queueCv_;
    Queued queue_[kMaxQueued]{};
    size_t head_ = 0, count_ = 0;
    size_t writePos_ = 0;
    bool sending_ = false;
    bool skipping_ = false;
    uint64_t skipStartMs_ = 0;
    uint32_t idrSendEwmaMs_ = 0;
    uint64_t maxWaitMs_ = 200;

    uint64_t statsStartMs_ = 0, statFrames_ = 0, statBytes_ = 0, statSkipped_ = 0, statSkips_ = 0;
    uint64_t statSendMaxMs_ = 0, statQueueMaxMs_ = 0, statMaxFrame_ = 0, statIdr_ = 0, statIdrBytes_ = 0;

    uint64_t keyframeWaitStartMs_ = 0;
    uint32_t framesSinceIdr_ = 0, lastIdrInterval_ = 0, keyframeLogs_ = 0;
    bool seenIdr_ = false;
    uint64_t framesTotal_ = 0;
    uint8_t lastProfile_ = 0, lastLevel_ = 0;
    bool seeded_ = false, noParamsLogged_ = false;
};
