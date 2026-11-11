#include "capture/screen_capture.hpp"

#include <algorithm>
#include <cstring>

#include "capture/game_capture.hpp"
#include "core/config.hpp"
#include "core/power.hpp"
#include "core/system.hpp"
#include "net/viewer_link.hpp"

namespace {
alignas(0x1000) uint8_t g_capture[CAPSSC_JPEG_BUFFER_SIZE];
uint8_t g_outgoing[CAPSSC_JPEG_BUFFER_SIZE];

uint64_t fnv1a(const uint8_t* data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}
}

bool ScreenCapture::start() {
    mutexInit(&mutex_);
    condvarInit(&cv_);
    return sys::spawn([this] { sendLoop(); }, 0x2F, 0x5000, "screen-send") &&
           sys::spawn([this] { captureLoop(); }, 0x30, 0x6000, "screen-capture");
}

void ScreenCapture::sendLoop() {
    while (true) {
        size_t size;
        uint64_t ts;
        {
            sys::Lock lock(mutex_);
            while (!pending_) condvarWait(&cv_, &mutex_);
            size = outgoingSize_;
            ts = outgoingTs_;
        }
        link_.send(proto::MsgType::VideoJpeg, 0, ts, g_outgoing, size);
        sys::Lock lock(mutex_);
        pending_ = false;
    }
}

void ScreenCapture::captureLoop() {
    if (hosversionBefore(10, 0, 0)) {
        LOG("screen capture needs firmware 10.0.0+");
        return;
    }
    Result rc = 0;
    LOG("screen: caps:sc capture, max %u fps, budget %u kbit/s", static_cast<unsigned>(settings::screenFpsLimit()),
        static_cast<unsigned>(settings::screenBytesPerSecond() * 8 / 1000));
    while (true) {
        while (power::captureHalted()) sys::sleepMs(200);
        if (R_SUCCEEDED(rc = capsscInitialize())) break;
        LOG("caps:sc init failed 0x%x, retrying", rc);
        sys::sleepMs(5000);
    }

    uint64_t lastHash = 0, lastOfferedMs = 0;
    bool showing = false;
    double tokens = settings::screenBytesPerSecond() / 2.0;
    uint64_t lastRefillMs = sys::nowMs(), overBudget = 0;
    uint64_t showingSinceMs = 0, showingFrames = 0, jpegBytes = 0;
    Result lastError = 0;
    bool pausedForSleep = false;

    uint64_t statsStart = sys::nowMs(), captures = 0, captureMsTotal = 0, captureMsMax = 0;
    uint64_t offered = 0, identical = 0, senderBusy = 0;

    while (true) {
        const uint64_t now = sys::nowMs();
        if (power::sleeping && !pausedForSleep) {
            pausedForSleep = true;
            LOG("screen: paused for sleep");
        } else if (!power::sleeping && pausedForSleep) {
            pausedForSleep = false;
            LOG("screen: resumed after sleep");
        }
        if (power::captureHalted() || !link_.connected() || !settings::screenCapture) {
            lastHash = 0;
            showing = false;
            showingSinceMs = 0;
            suspended_ = false;
            sys::sleepMs(200);
            continue;
        }
        const uint64_t video = game_.lastVideoMs(), audio = game_.lastAudioMs();
        const bool videoFlowing = video != 0 && now - video < kGameActiveWindowMs;
        const bool audioFlowing = audio != 0 && now - audio < kGameActiveWindowMs;
        suspended_ = !videoFlowing && !audioFlowing;
        if (videoFlowing || (audioFlowing && video != 0 && now - video < kStillGameMs)) {
            if (showing) {
                LOG("screen: game video is back after %llu ms of screen capture, %llu frames sent",
                    static_cast<unsigned long long>(now - showingSinceMs),
                    static_cast<unsigned long long>(showingFrames));
                showing = false;
            }
            lastHash = 0;
            sys::sleepMs(kIdlePollMs);
            continue;
        }
        if (!showing) {
            showing = true;
            showingSinceMs = now;
            showingFrames = 0;
            if (video != 0)
                LOG("screen: taking over, last game frame %llu ms ago, last game audio %llu ms ago (%s)",
                    static_cast<unsigned long long>(now - video),
                    static_cast<unsigned long long>(audio ? now - audio : 0),
                    audioFlowing ? "still picture" : "game suspended");
            else
                LOG("screen: taking over (no game video yet)");
        }

        u64 size = 0;
        power::screenBusy = true;
        if (power::captureHalted()) {
            power::screenBusy = false;
            sys::sleepMs(200);
            continue;
        }
        rc = capsscCaptureJpegScreenShot(&size, g_capture, sizeof g_capture, ViLayerStack_Default, 100000000);
        power::screenBusy = false;
        const uint64_t took = sys::nowMs() - now;
        if (R_FAILED(rc) || size == 0 || size > sizeof g_capture) {
            if (rc != lastError) LOG("caps:sc capture failed 0x%x", rc);
            lastError = rc;
            sys::sleepMs(500);
            continue;
        }
        ++captures;
        captureMsTotal += took;
        captureMsMax = std::max(captureMsMax, took);

        const uint64_t hash = fnv1a(g_capture, size);
        const double budget = settings::screenBytesPerSecond();
        tokens = std::min<double>(tokens + (now - lastRefillMs) * budget / 1000.0, budget / 2.0);
        lastRefillMs = now;
        if (hash == lastHash && now - lastOfferedMs < kResendMs) {
            ++identical;
        } else if (tokens < static_cast<double>(size) && hash != lastHash && now - lastOfferedMs < kResendMs) {
            ++overBudget;
        } else {
            sys::Lock lock(mutex_);
            if (pending_) {
                ++senderBusy;
            } else {
                std::memcpy(g_outgoing, g_capture, size);
                outgoingSize_ = size;
                outgoingTs_ = now * 1000;
                pending_ = true;
                condvarWakeOne(&cv_);
                lastHash = hash;
                lastOfferedMs = now;
                tokens -= static_cast<double>(size);
                ++offered;
                ++showingFrames;
                jpegBytes += size;
            }
        }

        if (now - statsStart >= kStatsEveryMs) {
            const uint64_t secs = (now - statsStart) / 1000;
            LOG("screen: capture avg %llu ms (max %llu) = max %llu fps | sent %llu fps, jpeg avg %llu KB | "
                "unchanged %llu | network busy %llu | over budget %llu",
                static_cast<unsigned long long>(captureMsTotal / std::max<uint64_t>(captures, 1)),
                static_cast<unsigned long long>(captureMsMax),
                static_cast<unsigned long long>(captureMsTotal ? captures * 1000 / captureMsTotal : 0),
                static_cast<unsigned long long>(offered / std::max<uint64_t>(secs, 1)),
                static_cast<unsigned long long>(offered ? jpegBytes / offered / 1024 : 0),
                static_cast<unsigned long long>(identical), static_cast<unsigned long long>(senderBusy),
                static_cast<unsigned long long>(overBudget));
            statsStart = now;
            captures = captureMsTotal = captureMsMax = offered = identical = senderBusy = jpegBytes = overBudget = 0;
        }

        const uint64_t minIntervalMs = 1000 / std::max<uint32_t>(settings::screenFpsLimit(), 1);
        const uint64_t elapsed = sys::nowMs() - now;
        if (elapsed < minIntervalMs) sys::sleepMs(minIntervalMs - elapsed);
    }
}
