#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace probe {
struct Step {
    uint32_t kbps;
    uint32_t fps;
    uint32_t keyInterval;
};

inline constexpr Step kSteps[5] = {{5000, 30, 15}, {10000, 30, 15}, {10000, 60, 30}, {15000, 30, 15}, {15000, 60, 30}};

inline constexpr uint32_t kLoadPercent = 130, kStepMs = 15000, kAckEveryMs = 250, kBaselineMs = 5000;
inline constexpr int kMaxSends = 1024, kMaxAcks = 128;
inline constexpr uint32_t kSkipWaitMs = 200, kIdrRatio = 4, kMaxDropPermille = 10, kMinRatePercent = 90,
                          kMinWorstSecondPercent = 60, kMaxAckP95Ms = 250, kMaxSendMs = 1000;

constexpr uint32_t frameBytes(const Step& s, bool idr) {
    const uint32_t avg = s.kbps * 1000 / 8 * kLoadPercent / 100 / s.fps;
    const uint32_t p = avg * s.keyInterval / (s.keyInterval - 1 + kIdrRatio);
    return idr ? kIdrRatio * p : p;
}

struct Samples {
    uint16_t sendMs[kMaxSends];
    int sends = 0;
    uint16_t ackMs[kMaxAcks];
    int acks = 0;
    int acksExpected = 0;
    int skips = 0;
    uint32_t frames = 0;
    uint32_t dropped = 0;
    uint64_t bytes = 0;
    uint32_t durationMs = 0;
    uint32_t worstSecondBytes = 0;
    uint32_t offeredKbps = 0;

    void addSend(uint32_t ms) {
        if (sends < kMaxSends) sendMs[sends++] = static_cast<uint16_t>(std::min<uint32_t>(ms, 65535));
    }
    void addAck(uint32_t ms) {
        if (acks < kMaxAcks) ackMs[acks++] = static_cast<uint16_t>(std::min<uint32_t>(ms, 65535));
    }
};

inline uint32_t percentile(const uint16_t* v, int n, int pct) {
    if (n <= 0) return 0;
    n = std::min(n, kMaxSends);
    std::array<uint16_t, kMaxSends> a;
    std::copy(v, v + n, a.begin());
    std::sort(a.begin(), a.begin() + n);
    const int rank = std::max((pct * n + 99) / 100, 1);
    return a[std::min(rank, n) - 1];
}

constexpr uint32_t achievedKbps(const Samples& s) {
    return s.durationMs == 0 ? 0 : static_cast<uint32_t>(s.bytes * 8 / s.durationMs);
}

inline bool passed(const Samples& s) {
    if (s.frames == 0 || s.sends <= 0 || s.acksExpected <= 0) return false;
    if (static_cast<uint64_t>(s.dropped) * 1000 > static_cast<uint64_t>(s.frames) * kMaxDropPermille) return false;
    if (static_cast<uint64_t>(achievedKbps(s)) * 100 < static_cast<uint64_t>(s.offeredKbps) * kMinRatePercent)
        return false;
    if (static_cast<uint64_t>(s.worstSecondBytes) * 8 / 1000 * 100 <
        static_cast<uint64_t>(s.offeredKbps) * kMinWorstSecondPercent)
        return false;
    if (*std::max_element(s.sendMs, s.sendMs + s.sends) > kMaxSendMs) return false;
    if (s.acks * 4 < s.acksExpected * 3) return false;
    return percentile(s.ackMs, s.acks, 95) < kMaxAckP95Ms;
}

inline int pick(const int8_t result[5]) {
    int best = -1;
    for (int i = 0; i < 5 && result[i] == 1; ++i) best = i;
    return best;
}

constexpr int stepVerdict(int passes, int fails) { return passes >= 2 ? 1 : fails >= 2 ? 0 : -1; }

constexpr uint32_t ewma(uint32_t prev, uint32_t sample) { return prev == 0 ? sample : (prev * 7 + sample) / 8; }

constexpr uint32_t adaptiveWaitMs(uint32_t idrSendEwmaMs) {
    return std::clamp<uint32_t>(2 * idrSendEwmaMs + 100, 200, 400);
}
}
