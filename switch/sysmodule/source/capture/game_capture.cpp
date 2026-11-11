#include "capture/game_capture.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "capture/grcd_session.h"
#include "capture/system_audio.hpp"
#include "core/config.hpp"
#include "core/power.hpp"
#include "core/system.hpp"
#include "net/probe_core.hpp"
#include "net/viewer_link.hpp"

namespace {
constexpr Result kTooBigForBuffer = 0xcd4;
constexpr Result kNotInitialized = 0x3e8d4;
constexpr const char* kParamsPath = "sdmc:/config/nxstream/h264params.bin";
constexpr const char* kParamsTmpPath = "sdmc:/config/nxstream/h264params.tmp";
alignas(0x1000) uint8_t g_videoBuffer[0x80000];
alignas(0x1000) uint8_t g_audioBuffer[0x1000];
uint8_t g_queue[0x100000];
}

bool GameCapture::start() {
    static_assert(sizeof g_queue == kQueueBytes);
    mutexInit(&queueMutex_);
    condvarInit(&queueCv_);
    return sys::spawn([this] { videoLoop(); }, 0x2B, 0x6000, "game-video") &&
           sys::spawn([this] { sendLoop(); }, 0x2C, 0x5000, "game-send") &&
           sys::spawn([this] { audioLoop(); }, 0x2A, 0x5000, "game-audio");
}

bool GameCapture::openRecorder() {
    Result rc = grcdSessionOpen(&videoSrv_);
    if (R_SUCCEEDED(rc)) rc = grcdSessionOpen(&audioSrv_);
    if (R_FAILED(rc)) {
        LOG("grc:d: open failed 0x%x", rc);
        return false;
    }
    LOG("grc:d: sessions open, begin deferred to the first 0x3e8d4");
    return true;
}

void GameCapture::videoLoop() {
    while (!link_.connected() || !settings::gameCapture) sys::sleepMs(250);
    LOG("grc:d: first viewer connected, starting recorder");

    if (!openRecorder()) {
        LOG("game capture disabled; HOME/screen capture still works");
        return;
    }
    ready_ = true;
    if (!params_.ready()) loadParams();

    Result lastError = 0;
    uint32_t failures = 0, emptyTransfers = 0, badFormats = 0;
    bool beginCalled = false;
    bool pausedForSleep = false;
    while (true) {
        if (power::captureHalted() || !link_.connected() || !settings::gameCapture) {
            {
                sys::Lock lock(queueMutex_);
                clearQueue();
                skipping_ = false;
            }
            keyframeWaitStartMs_ = 0;
            failures = 0;
            lastError = 0;
            if (power::sleeping && !pausedForSleep) {
                pausedForSleep = true;
                LOG("grc:d: paused for sleep");
            }
            sys::sleepMs(200);
            continue;
        }
        if (pausedForSleep) {
            pausedForSleep = false;
            LOG("grc:d: resumed after sleep");
        }
        u32 frames = 0, size = 0;
        u64 timestamp = 0;
        const Result rc = grcdSessionTransfer(&videoSrv_, GrcStream_Video, g_videoBuffer, sizeof g_videoBuffer, &frames,
                                              &size, &timestamp);
        if (rc == kNotInitialized) {
            if (beginCalled) {
                sys::sleepMs(100);
                continue;
            }
            beginCalled = true;
            LOG("grc:d: transfer returned 0x3e8d4 (not initialized), calling begin once");
            const Result begin = grcdSessionBegin(&videoSrv_);
            if (R_FAILED(begin)) {
                LOG("grc:d: begin failed 0x%x - is SysDVR or another recorder client installed?", begin);
                LOG("game capture disabled; HOME/screen capture still works");
                ready_ = false;
                return;
            }
            LOG("grc:d: begin ok, recorder stream started");
            continue;
        }
        if (R_FAILED(rc)) {
            if (rc != lastError || failures % 100 == 0)
                LOG("grc:d video transfer failed 0x%x (module %u, description %u), %u in a row", rc, R_MODULE(rc),
                    R_DESCRIPTION(rc), static_cast<unsigned>(failures + 1));
            lastError = rc;
            ++failures;
            sys::sleepMs(rc == kTooBigForBuffer ? 10 : 100);
            continue;
        }
        if (failures) {
            if (failures > 1 || lastError != kTooBigForBuffer)
                LOG("grc:d video resumed after %u failed transfers", static_cast<unsigned>(failures));
            failures = 0;
            lastError = 0;
        }
        if (size == 0 || size > sizeof g_videoBuffer) {
            if (++emptyTransfers == 1 || emptyTransfers % 100 == 0)
                LOG("grc:d video: transfer ok but size %u (%u in a row)", static_cast<unsigned>(size),
                    static_cast<unsigned>(emptyTransfers));
            sys::sleepMs(100);
            continue;
        }
        emptyTransfers = 0;
        if (!settings::gameCapture) continue;

        if (!h264::normalizeAnnexB(g_videoBuffer, size)) {
            if (++badFormats == 1 || badFormats % 100 == 0)
                LOG("grc:d video: unknown bitstream format, %u B (%u in a row)", static_cast<unsigned>(size),
                    badFormats);
            sys::sleepMs(10);
            continue;
        }
        badFormats = 0;
        const h264::FrameInfo info = params_.scan(g_videoBuffer, size);
        noteKeyframeInterval(info.idr);
        if (info.changed && params_.ready()) {
            if (seeded_) {
                LOG("grc:d: real SPS/PPS differ from the ones loaded from SD, replacing them");
                seeded_ = false;
            }
            saveParams();
        }
        if (info.hasSps && params_.ready()) {
            const auto& sps = params_.sps();
            if (sps.size() >= 4 && (sps[1] != lastProfile_ || sps[3] != lastLevel_)) {
                lastProfile_ = sps[1];
                lastLevel_ = sps[3];
                LOG("grc:d: SPS %zu B profile_idc %u constraints 0x%02x level_idc %u, PPS %zu B", sps.size(), sps[1],
                    sps[2], sps[3], params_.pps().size());
            }
        }
        if (++framesTotal_ == 1)
            LOG("grc:d: first frame %u B, idr %d, %u frames in transfer, timestamp %llu", static_cast<unsigned>(size),
                info.idr, static_cast<unsigned>(frames), static_cast<unsigned long long>(timestamp));

        if (!link_.connected()) {
            keyframeWaitStartMs_ = 0;
            sys::Lock lock(queueMutex_);
            clearQueue();
            skipping_ = false;
            continue;
        }
        if (!params_.ready()) {
            if (!noParamsLogged_) {
                noParamsLogged_ = true;
                LOG("grc:d: no SPS/PPS yet (game started before the first viewer and none saved), "
                    "showing screen capture until the game restarts");
            }
            continue;
        }
        if (noParamsLogged_) {
            noParamsLogged_ = false;
            LOG("grc:d: SPS/PPS available, game video resumes");
        }
        if (link_.probing()) {
            {
                sys::Lock lock(queueMutex_);
                clearQueue();
                skipping_ = false;
            }
            keyframeWaitStartMs_ = 0;
            continue;
        }
        if (link_.needsKeyframe() && !info.idr) {
            if (keyframeWaitStartMs_ == 0) keyframeWaitStartMs_ = sys::nowMs();
            continue;
        }
        if (keyframeWaitStartMs_ != 0) {
            LOG("grc:d: viewer got its first keyframe after %llu ms",
                static_cast<unsigned long long>(sys::nowMs() - keyframeWaitStartMs_));
            keyframeWaitStartMs_ = 0;
        }
        lastVideoMs_ = sys::nowMs();

        const bool withParams = info.idr && !(info.hasSps && info.hasPps) && params_.ready();
        bool queued;
        {
            sys::Lock lock(queueMutex_);
            queued = enqueue(info, timestamp, size, withParams);
        }
        if (queued && info.idr) link_.keyframeSent();
    }
}

void GameCapture::loadParams() {
    FILE* f = std::fopen(kParamsPath, "rb");
    if (!f) {
        LOG("grc:d: no saved SPS/PPS on SD (%s)", kParamsPath);
        return;
    }
    uint8_t buf[1024];
    const size_t n = std::fread(buf, 1, sizeof buf, f);
    std::fclose(f);
    std::vector<uint8_t> sps, pps;
    const char* why = h264::decodeSaved(buf, n, settings::recorderFps, settings::recorderBitrate, sps, pps);
    if (why) {
        LOG("grc:d: saved SPS/PPS ignored: %s", why);
        return;
    }
    params_.seed(std::move(sps), std::move(pps));
    seeded_ = true;
    const auto& s = params_.sps();
    LOG("grc:d: SPS/PPS loaded from SD (stream started before the first viewer): "
        "SPS %zu B profile_idc %u level_idc %u, PPS %zu B",
        s.size(), s.size() >= 4 ? s[1] : 0, s.size() >= 4 ? s[3] : 0, params_.pps().size());
}

void GameCapture::saveParams() {
    const auto data = h264::encodeSaved(settings::recorderFps, settings::recorderBitrate, params_.sps(), params_.pps());
    bool ok = false;
    if (FILE* f = std::fopen(kParamsTmpPath, "wb")) {
        ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
        ok = std::fclose(f) == 0 && ok;
    }
    if (ok) {
        std::remove(kParamsPath);
        ok = std::rename(kParamsTmpPath, kParamsPath) == 0;
    }
    LOG("grc:d: SPS/PPS %s %s (%zu B)", ok ? "saved to" : "FAILED to save to", kParamsPath, data.size());
}

bool GameCapture::enqueue(const h264::FrameInfo& info, uint64_t timestamp, size_t size, bool withParams) {
    const uint64_t now = sys::nowMs();
    if (statsStartMs_ == 0) statsStartMs_ = now;

    const size_t total = size + (withParams ? params_.annexB().size() : 0);
    uint32_t offset = 0;
    const size_t waiting = sending_ ? (head_ + 1) % kMaxQueued : head_;
    const bool late = count_ > (sending_ ? 1u : 0u) && now - queue_[waiting].queuedMs > maxWaitMs_;
    if (!skipping_ && (late || count_ == kMaxQueued || !allocate(total, offset))) {
        clearQueue();
        skipping_ = true;
        skipStartMs_ = now;
        ++statSkips_;
    }
    if (skipping_) {
        if (!info.idr && now - skipStartMs_ < kMaxSkipMs) {
            ++statSkipped_;
            logStats(now);
            return false;
        }
        if (count_ == kMaxQueued || !allocate(total, offset)) {
            ++statSkipped_;
            logStats(now);
            return false;
        }
        skipping_ = false;
    }

    uint8_t* dst = g_queue + offset;
    if (withParams) {
        const auto& ps = params_.annexB();
        std::memcpy(dst, ps.data(), ps.size());
        dst += ps.size();
    }
    std::memcpy(dst, g_videoBuffer, size);
    size_t tail = head_ + count_;
    if (tail >= kMaxQueued) tail -= kMaxQueued;
    queue_[tail] = Queued{offset, static_cast<uint32_t>(total), timestamp, now, info.idr};
    ++count_;
    writePos_ = offset + total;
    ++statFrames_;
    statBytes_ += total;
    statMaxFrame_ = std::max<uint64_t>(statMaxFrame_, total);
    if (info.idr) {
        ++statIdr_;
        statIdrBytes_ += total;
    }
    condvarWakeOne(&queueCv_);
    logStats(now);
    return true;
}

bool GameCapture::allocate(size_t size, uint32_t& offset) const {
    if (size > kQueueBytes) return false;
    if (count_ == 0) {
        offset = 0;
        return true;
    }
    const size_t oldest = queue_[head_].offset;
    if (writePos_ > oldest) {
        if (writePos_ + size <= kQueueBytes) {
            offset = static_cast<uint32_t>(writePos_);
            return true;
        }
        if (size <= oldest) {
            offset = 0;
            return true;
        }
        return false;
    }
    if (writePos_ + size <= oldest) {
        offset = static_cast<uint32_t>(writePos_);
        return true;
    }
    return false;
}

void GameCapture::clearQueue() {
    if (sending_ && count_ > 0) {
        count_ = 1;
        writePos_ = queue_[head_].offset + queue_[head_].size;
    } else {
        count_ = 0;
        writePos_ = 0;
    }
}

void GameCapture::sendLoop() {
    while (true) {
        Queued e;
        {
            sys::Lock lock(queueMutex_);
            while (count_ == 0) condvarWait(&queueCv_, &queueMutex_);
            e = queue_[head_];
            sending_ = true;
        }
        const bool probingBefore = link_.probing();
        const uint64_t start = sys::nowMs();
        link_.send(proto::MsgType::VideoH264, e.idr ? proto::kFlagKeyframe : 0, e.timestamp, g_queue + e.offset,
                   e.size);
        const bool realSend = !probingBefore && !link_.probing();
        const uint64_t end = sys::nowMs();

        sys::Lock lock(queueMutex_);
        sending_ = false;
        if (count_ > 0 && queue_[head_].offset == e.offset) {
            head_ = head_ + 1 == kMaxQueued ? 0 : head_ + 1;
            if (--count_ == 0) writePos_ = 0;
        }
        if (e.idr && realSend) {
            idrSendEwmaMs_ = probe::ewma(idrSendEwmaMs_, static_cast<uint32_t>(end - start));
            maxWaitMs_ = probe::adaptiveWaitMs(idrSendEwmaMs_);
        }
        statSendMaxMs_ = std::max(statSendMaxMs_, end - start);
        statQueueMaxMs_ = std::max(statQueueMaxMs_, end - e.queuedMs);
    }
}

void GameCapture::logStats(uint64_t now) {
    if (now - statsStartMs_ < kStatsEveryMs) return;
    const uint64_t secs = std::max<uint64_t>((now - statsStartMs_) / 1000, 1);
    LOG("grc:d video: %llu fps, %llu kbit/s, frame avg %llu B max %llu B, %llu keyframes avg %llu B | "
        "slowest send %llu ms, longest wait %llu ms, max wait %u ms | skipped %llu frames in %llu slowdowns",
        static_cast<unsigned long long>(statFrames_ / secs),
        static_cast<unsigned long long>(statBytes_ * 8 / 1000 / secs),
        static_cast<unsigned long long>(statFrames_ ? statBytes_ / statFrames_ : 0),
        static_cast<unsigned long long>(statMaxFrame_), static_cast<unsigned long long>(statIdr_),
        static_cast<unsigned long long>(statIdr_ ? statIdrBytes_ / statIdr_ : 0),
        static_cast<unsigned long long>(statSendMaxMs_), static_cast<unsigned long long>(statQueueMaxMs_),
        static_cast<unsigned>(maxWaitMs_), static_cast<unsigned long long>(statSkipped_),
        static_cast<unsigned long long>(statSkips_));
    statsStartMs_ = now;
    statFrames_ = statBytes_ = statSkipped_ = statSkips_ = statSendMaxMs_ = statQueueMaxMs_ = 0;
    statMaxFrame_ = statIdr_ = statIdrBytes_ = 0;
}

void GameCapture::noteKeyframeInterval(bool idr) {
    ++framesSinceIdr_;
    if (!idr) return;
    if (seenIdr_ && keyframeLogs_ < 8 && framesSinceIdr_ != lastIdrInterval_) {
        LOG("grc:d: keyframe every %u frames", static_cast<unsigned>(framesSinceIdr_));
        lastIdrInterval_ = framesSinceIdr_;
        ++keyframeLogs_;
    }
    seenIdr_ = true;
    framesSinceIdr_ = 0;
}

void GameCapture::audioLoop() {
    Result lastError = 0;
    uint64_t statsStart = 0, packets = 0, bytesSent = 0, suppressed = 0;
    uint32_t emptyTransfers = 0;
    bool pausedForSleep = false;
    while (true) {
        if (power::captureHalted() || !ready_.load() || !link_.connected() || !settings::gameCapture) {
            lastError = 0;
            if (power::sleeping && !pausedForSleep) {
                pausedForSleep = true;
                LOG("grc:d audio: paused for sleep");
            }
            sys::sleepMs(100);
            continue;
        }
        if (pausedForSleep) {
            pausedForSleep = false;
            LOG("grc:d audio: resumed after sleep");
        }
        u32 frames = 0, size = 0;
        u64 timestamp = 0;
        const Result rc = grcdSessionTransfer(&audioSrv_, GrcStream_Audio, g_audioBuffer, sizeof g_audioBuffer, &frames,
                                              &size, &timestamp);
        if (R_FAILED(rc)) {
            if (rc != lastError && rc != kNotInitialized) LOG("grc:d audio transfer failed 0x%x", rc);
            lastError = rc;
            sys::sleepMs(100);
            continue;
        }
        if (size == 0 || size > sizeof g_audioBuffer) {
            if (++emptyTransfers == 1 || emptyTransfers % 100 == 0)
                LOG("grc:d audio: transfer ok but size %u (%u in a row)", static_cast<unsigned>(size),
                    static_cast<unsigned>(emptyTransfers));
            sys::sleepMs(100);
            continue;
        }
        emptyTransfers = 0;
        lastAudioMs_ = sys::nowMs();
        if (!link_.connected() || !settings::audio) continue;
        const uint64_t now = sys::nowMs();
        if (statsStart == 0) statsStart = now;
        if (systemAudio_.active()) {
            ++suppressed;
        } else {
            ++packets;
            bytesSent += size;
            link_.send(proto::MsgType::Audio, 0, timestamp, g_audioBuffer, size);
        }
        if (now - statsStart >= kStatsEveryMs) {
            const uint64_t secs = std::max<uint64_t>((now - statsStart) / 1000, 1);
            LOG("grc:d audio: %llu packets/s sent, %llu kbit/s, %llu replaced by audrec, last packet %u B",
                static_cast<unsigned long long>(packets / secs),
                static_cast<unsigned long long>(bytesSent * 8 / 1000 / secs),
                static_cast<unsigned long long>(suppressed), static_cast<unsigned>(size));
            statsStart = now;
            packets = bytesSent = suppressed = 0;
        }
    }
}
