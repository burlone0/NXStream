#include "capture/system_audio.hpp"

#include <switch.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "core/config.hpp"
#include "core/power.hpp"
#include "core/system.hpp"
#include "net/viewer_link.hpp"

namespace {
constexpr size_t kBufferBytes = 0x1800;
constexpr size_t kSlotBytes = 0x3000;
constexpr size_t kBufferCount = 6;

alignas(0x1000) uint8_t g_samples[kBufferCount][kSlotBytes];
FinalOutputRecorderBuffer g_buffers[kBufferCount];
int16_t g_pcm[kBufferBytes];

AudrecRecorder g_recorder;
Event g_event;
FinalOutputRecorderParameterInternal g_format;

inline int16_t clamp16(int v) { return static_cast<int16_t>(std::clamp(v, -32768, 32767)); }

inline int sampleAt(const uint8_t* data, size_t index) {
    switch (g_format.sample_format) {
        case PcmFormat_Int16: {
            int16_t s;
            std::memcpy(&s, data + index * 2, 2);
            return s;
        }
        case PcmFormat_Int32: {
            int32_t s;
            std::memcpy(&s, data + index * 4, 4);
            return s >> 16;
        }
        case PcmFormat_Float: {
            float f;
            std::memcpy(&f, data + index * 4, 4);
            return static_cast<int>(std::clamp(f, -1.0f, 1.0f) * 32767.0f);
        }
        default:
            return 0;
    }
}

size_t bytesPerSample() {
    switch (g_format.sample_format) {
        case PcmFormat_Int16: return 2;
        case PcmFormat_Int32:
        case PcmFormat_Float: return 4;
        default: return 0;
    }
}

size_t toStereo16(const uint8_t* data, size_t size) {
    const size_t channels = g_format.channel_count;
    const size_t frames = size / (channels * bytesPerSample());
    for (size_t f = 0; f < frames; ++f) {
        const size_t base = f * channels;
        int l, r;
        if (channels == 1) {
            l = r = sampleAt(data, base);
        } else if (channels >= 6) {
            const int c = sampleAt(data, base + 2) * 7 / 10;
            l = sampleAt(data, base) + c + sampleAt(data, base + 4) * 7 / 10;
            r = sampleAt(data, base + 1) + c + sampleAt(data, base + 5) * 7 / 10;
        } else {
            l = sampleAt(data, base);
            r = sampleAt(data, base + 1);
        }
        g_pcm[f * 2] = clamp16(l);
        g_pcm[f * 2 + 1] = clamp16(r);
    }
    return frames * 4;
}

bool append(size_t i) {
    FinalOutputRecorderBuffer& b = g_buffers[i];
    b = {};
    b.sample_buffer_ptr = reinterpret_cast<u64>(g_samples[i]);
    b.sample_buffer_capacity = kBufferBytes;
    b.data_size = kBufferBytes;
    b.data_offset = 0;
    return R_SUCCEEDED(audrecRecorderAppendFinalOutputRecorderBuffer(&g_recorder, reinterpret_cast<u64>(&b), &b));
}
}

bool SystemAudio::active() const {
    const uint64_t last = lastBufferMs_.load();
    return last != 0 && sys::nowMs() - last < 500;
}

bool SystemAudio::start() {
    return sys::spawn([this] { loop(); }, 0x2A, 0x5000, "system-audio");
}

bool SystemAudio::open() {
    Result rc = audrecInitialize();
    if (R_FAILED(rc)) {
        LOG("audrec: service unavailable 0x%x - game audio only", rc);
        return false;
    }

    FinalOutputRecorderParameter want{48000, 2};
    rc = audrecOpenFinalOutputRecorder(&g_recorder, &want, 0, &g_format);
    if (R_FAILED(rc)) {
        LOG("audrec: open failed 0x%x - game audio only", rc);
        audrecExit();
        return false;
    }
    LOG("audrec: final output %u Hz, %u ch, format %u, %zu buffers of %zu B", g_format.sample_rate,
        g_format.channel_count, g_format.sample_format, kBufferCount, kBufferBytes);

    const bool supported = g_format.sample_rate == 48000 && g_format.channel_count >= 1 &&
                           g_format.channel_count <= 6 && bytesPerSample() != 0;
    if (!supported) {
        LOG("audrec: unsupported format - game audio only");
    } else if (R_FAILED(rc = audrecRecorderRegisterBufferEvent(&g_recorder, &g_event))) {
        LOG("audrec: buffer event failed 0x%x", rc);
    } else {
        bool queued = true;
        for (size_t i = 0; i < kBufferCount && queued; ++i) queued = append(i);
        if (!queued) {
            LOG("audrec: append buffer failed");
        } else if (R_FAILED(rc = audrecRecorderStart(&g_recorder))) {
            LOG("audrec: start failed 0x%x", rc);
        } else {
            return true;
        }
        eventClose(&g_event);
    }
    audrecRecorderClose(&g_recorder);
    audrecExit();
    return false;
}

void SystemAudio::close() {
    audrecRecorderStop(&g_recorder);
    audrecRecorderClose(&g_recorder);
    eventClose(&g_event);
    audrecExit();
}

void SystemAudio::loop() {
    bool pausedForSleep = false;
    while (true) {
        while (power::captureHalted() || !link_.connected() || !settings::audio) sys::sleepMs(250);
        if (pausedForSleep) {
            pausedForSleep = false;
            LOG("audrec: resumed after sleep");
        }
        power::audrecBusy = true;
        if (power::captureHalted()) {
            power::audrecBusy = false;
            continue;
        }
        if (!open()) {
            power::audrecBusy = false;
            while (!power::captureHalted() && link_.connected() && settings::audio) sys::sleepMs(250);
            continue;
        }
        LOG("audrec: recorder opened, viewer connected (HOME sounds are not available to any service)");
        const bool bufferLost = record();
        close();
        power::audrecBusy = false;
        lastBufferMs_ = 0;
        if (power::sleeping) {
            pausedForSleep = true;
            LOG("audrec: closed for sleep");
        } else if (bufferLost) {
            LOG("audrec: buffer lost, reopening the recorder in 1 s");
            sys::sleepMs(1000);
        } else {
            LOG("audrec: recorder closed, %s", power::halted() ? "module disabled" : "viewer gone or audio off");
        }
    }
}

bool SystemAudio::record() {
    Result lastError = 0;
    uint64_t lastBufferAt = sys::nowMs();
    uint64_t statsStart = lastBufferAt, buffers = 0, pcmOut = 0, events = 0;
    uint32_t instantWaits = 0;
    int peak = 0;
    bool silent = false;
    while (true) {
        if (power::captureHalted() || !link_.connected() || !settings::audio) return false;
        if (!silent && sys::nowMs() - lastBufferAt > 10000) {
            LOG("audrec: no buffers released for 10 s");
            silent = true;
        }
        const uint64_t waitStart = sys::nowMs();
        if (R_FAILED(eventWait(&g_event, 200000000ull))) {
            if (sys::nowMs() - waitStart < 100) {
                if (++instantWaits == 1 || instantWaits % 100 == 0)
                    LOG("audrec: event wait failed at once (%u in a row)", static_cast<unsigned>(instantWaits));
                sys::sleepMs(100);
            }
            continue;
        }
        instantWaits = 0;
        ++events;

        u64 released[kBufferCount] = {};
        u64 count = kBufferCount;
        u64 releasedNs = 0;
        const Result rc =
            audrecRecorderGetReleasedFinalOutputRecorderBuffers(&g_recorder, released, &count, &releasedNs);
        if (R_FAILED(rc)) {
            if (rc != lastError) LOG("audrec: get released failed 0x%x", rc);
            lastError = rc;
            sys::sleepMs(50);
            continue;
        }

        for (u64 n = 0; n < count; ++n) {
            const auto* done = reinterpret_cast<const FinalOutputRecorderBuffer*>(released[n]);
            const size_t i = static_cast<size_t>(done - g_buffers);
            if (i >= kBufferCount) {
                LOG("audrec: released buffer pointer out of range, buffer lost");
                return true;
            }
            lastBufferAt = sys::nowMs();
            lastBufferMs_ = lastBufferAt;
            if (silent) {
                LOG("audrec: buffers released again");
                silent = false;
            }

            size_t size = g_buffers[i].data_size;
            const size_t offset = g_buffers[i].data_offset;
            if (offset + size > kBufferBytes) size = kBufferBytes - std::min(offset, kBufferBytes);

            if (size > 0 && link_.connected() && settings::audio) {
                const size_t pcmBytes = toStereo16(g_samples[i] + offset, size);
                for (size_t k = 0; k < pcmBytes / 2; ++k) peak = std::max(peak, std::abs(static_cast<int>(g_pcm[k])));
                if (pcmBytes) link_.send(proto::MsgType::Audio, 0, sys::nowMs() * 1000, g_pcm, pcmBytes);
                ++buffers;
                pcmOut += pcmBytes;
            }
            const uint64_t now = sys::nowMs();
            if (now - statsStart >= 15000) {
                const uint64_t secs = std::max<uint64_t>((now - statsStart) / 1000, 1);
                LOG("audrec: %llu buffers/s, %llu kbit/s sent, %llu events, peak %d of 32767",
                    static_cast<unsigned long long>(buffers / secs),
                    static_cast<unsigned long long>(pcmOut * 8 / 1000 / secs),
                    static_cast<unsigned long long>(events), peak);
                statsStart = now;
                buffers = pcmOut = events = 0;
                peak = 0;
            }
            if (!append(i)) {
                LOG("audrec: re-append buffer %u failed, buffer lost", static_cast<unsigned>(i));
                return true;
            }
        }
    }
}
