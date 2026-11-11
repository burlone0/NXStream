#include "net/link_probe.hpp"

#include <switch.h>

#include <algorithm>
#include <cstdio>

#include "core/config.hpp"
#include "core/power.hpp"
#include "core/system.hpp"
#include "net/probe_core.hpp"
#include "net/viewer_link.hpp"

namespace {
constexpr const char* kResultPath = "sdmc:/config/nxstream/probe_result.ini";
constexpr const char* kResultTmpPath = "sdmc:/config/nxstream/probe_result.tmp";
constexpr size_t kStackBytes = 0x4000;
constexpr int kPendingSlots = 16;
constexpr int kBuckets = 16;
constexpr uint32_t kBaselineBytes = 1024;
constexpr uint32_t kLateAckMs = 500;
constexpr uint32_t kCapacityMs = 5000;
constexpr uint32_t kCapacityFrameBytes = 64 * 1024;

enum class Outcome { Done, Lost, Off };

struct Pending {
    uint32_t seq;
    uint64_t startMs;
    bool used;
};

alignas(0x1000) uint8_t g_stack[kStackBytes];
Thread g_thread;
ViewerLink* g_link = nullptr;
Mutex g_mutex;
probe::Samples g_samples;
probe::Samples g_snap;
Pending g_pending[kPendingSlots];
uint32_t g_bucket[kBuckets];
bool g_collecting = false;
uint32_t g_seq = 0;

const probe::Samples& snapshot() {
    sys::Lock lock(g_mutex);
    g_snap = g_samples;
    return g_snap;
}

void clearSamples() {
    g_samples.sends = g_samples.acks = g_samples.acksExpected = g_samples.skips = 0;
    g_samples.frames = g_samples.dropped = g_samples.durationMs = g_samples.worstSecondBytes = 0;
    g_samples.offeredKbps = 0;
    g_samples.bytes = 0;
}

void resetSamples() {
    sys::Lock lock(g_mutex);
    clearSamples();
}

void writeResult(uint32_t id, const char* state, int step, int runNo, const char* reason, bool hasPick, int pick,
                 const probe::Samples& s, int64_t capacity) {
    char buf[256];
    size_t n = 0;
    bool fits = true;
    auto append = [&](int ret) {
        if (!fits) return;
        if (ret < 0 || static_cast<size_t>(ret) >= sizeof buf - n) {
            fits = false;
            return;
        }
        n += static_cast<size_t>(ret);
    };
    append(std::snprintf(buf, sizeof buf, "id=%u\nstate=%s\nstep=%d\nrun=%d\n", static_cast<unsigned>(id), state, step,
                             runNo));
    if (reason && fits) append(std::snprintf(buf + n, sizeof buf - n, "reason=%s\n", reason));
    if (hasPick && fits) append(std::snprintf(buf + n, sizeof buf - n, "pick=%d\n", pick));
    if (fits)
        append(std::snprintf(buf + n, sizeof buf - n, "send_p99=%u\nack_p95=%u\n",
                             static_cast<unsigned>(probe::percentile(s.sendMs, s.sends, 99)),
                             static_cast<unsigned>(probe::percentile(s.ackMs, s.acks, 95))));
    if (capacity >= 0 && fits) append(std::snprintf(buf + n, sizeof buf - n, "capacity_kbps=%u\n", static_cast<unsigned>(capacity)));
    if (!fits) {
        LOG("probe: result too long");
        return;
    }
    bool ok = false;
    if (FILE* f = std::fopen(kResultTmpPath, "wb")) {
        ok = std::fwrite(buf, 1, n, f) == n;
        ok = std::fclose(f) == 0 && ok;
    }
    if (ok) {
        std::remove(kResultPath);
        ok = std::rename(kResultTmpPath, kResultPath) == 0;
    }
    if (!ok) LOG("probe: result file not written");
}

bool interrupted(Outcome& out) {
    if (power::halted() || power::captureHalted()) {
        out = Outcome::Off;
        return true;
    }
    if (!g_link->connected()) {
        out = Outcome::Lost;
        return true;
    }
    return false;
}

bool waitLateAcks(Outcome& out) {
    const uint64_t waitStart = sys::nowMs();
    while (sys::nowMs() - waitStart < kLateAckMs) {
        {
            sys::Lock lock(g_mutex);
            if (g_samples.acks >= g_samples.acksExpected) break;
        }
        if (interrupted(out)) return false;
        sys::sleepMs(10);
    }
    return true;
}

bool runBaseline(Outcome& out) {
    {
        sys::Lock lock(g_mutex);
        clearSamples();
        for (auto& p : g_pending) p.used = false;
        g_collecting = true;
    }
    const uint64_t t0 = sys::nowMs();
    bool aborted = false;
    for (uint64_t k = 0; k * probe::kAckEveryMs < probe::kBaselineMs; ++k) {
        const uint64_t sched = t0 + k * probe::kAckEveryMs;
        uint64_t now = sys::nowMs();
        if (now < sched) {
            sys::sleepMs(sched - now);
            now = sys::nowMs();
        }
        if (interrupted(out)) {
            aborted = true;
            break;
        }
        const uint32_t seq = ++g_seq;
        {
            sys::Lock lock(g_mutex);
            ++g_samples.acksExpected;
            g_pending[g_samples.acksExpected % kPendingSlots] = Pending{seq, now, true};
        }
        if (!g_link->sendProbe(seq, true, kBaselineBytes)) {
            out = Outcome::Lost;
            aborted = true;
            break;
        }
    }
    if (!aborted) aborted = !waitLateAcks(out);
    sys::Lock lock(g_mutex);
    g_collecting = false;
    return !aborted && g_samples.acks > 0;
}

bool runStep(const probe::Step& step, Outcome& out) {
    {
        sys::Lock lock(g_mutex);
        clearSamples();
        g_samples.offeredKbps = step.kbps * probe::kLoadPercent / 100;
        for (auto& p : g_pending) p.used = false;
        for (auto& b : g_bucket) b = 0;
        g_collecting = true;
    }
    const uint64_t t0 = sys::nowMs();
    uint64_t nextAckMs = 0;
    bool aborted = false;
    bool skipping = false;
    for (uint64_t k = 0;; ++k) {
        const uint64_t offset = k * 1000 / step.fps;
        if (offset >= probe::kStepMs) break;
        const uint64_t sched = t0 + offset;
        uint64_t now = sys::nowMs();
        if (now < sched) {
            sys::sleepMs(sched - now);
            now = sys::nowMs();
        }
        if (interrupted(out)) {
            aborted = true;
            break;
        }

        const bool idr = k % step.keyInterval == 0;
        if (now > sched + probe::kSkipWaitMs) skipping = true;
        else if (idr) skipping = false;
        {
            sys::Lock lock(g_mutex);
            ++g_samples.frames;
            if (skipping) {
                ++g_samples.dropped;
                g_samples.skips = static_cast<int>(g_samples.dropped);
            }
        }
        if (skipping) continue;

        const bool reply = offset >= nextAckMs;
        const uint32_t seq = ++g_seq;
        if (reply) {
            nextAckMs += probe::kAckEveryMs;
            sys::Lock lock(g_mutex);
            ++g_samples.acksExpected;
            Pending& slot = g_pending[g_samples.acksExpected % kPendingSlots];
            slot = Pending{seq, now, true};
        }
        const uint32_t bytes = probe::frameBytes(step, idr);
        const bool ok = g_link->sendProbe(seq, reply, bytes);
        {
            sys::Lock lock(g_mutex);
            g_bucket[std::min<uint64_t>(offset / 1000, kBuckets - 1)] += bytes;
            g_samples.bytes += bytes;
            g_samples.addSend(static_cast<uint32_t>(sys::nowMs() - now));
        }
        if (!ok) {
            out = Outcome::Lost;
            aborted = true;
            break;
        }
    }
    {
        sys::Lock lock(g_mutex);
        g_samples.durationMs = static_cast<uint32_t>(std::max<uint64_t>(sys::nowMs() - t0, 1));
        uint32_t worst = 0xffffffffu;
        for (int b = 0; b < static_cast<int>(probe::kStepMs / 1000) && b < kBuckets; ++b)
            worst = std::min(worst, g_bucket[b]);
        g_samples.worstSecondBytes = worst;
    }
    if (!aborted) aborted = !waitLateAcks(out);
    sys::Lock lock(g_mutex);
    g_collecting = false;
    return !aborted && probe::passed(g_samples);
}

void logBaseline() {
    const probe::Samples& s = snapshot();
    uint32_t mx = 0;
    for (int i = 0; i < s.acks; ++i) mx = std::max<uint32_t>(mx, s.ackMs[i]);
    LOG("probe: baseline acks %d/%d p50 %u p95 %u max %u ms", s.acks, s.acksExpected,
        static_cast<unsigned>(probe::percentile(s.ackMs, s.acks, 50)),
        static_cast<unsigned>(probe::percentile(s.ackMs, s.acks, 95)), static_cast<unsigned>(mx));
}

void logStep(int index, int runNo, const probe::Step& step, bool pass) {
    const probe::Samples& s = snapshot();
    uint32_t mx = 0, amx = 0;
    for (int i = 0; i < s.sends; ++i) mx = std::max<uint32_t>(mx, s.sendMs[i]);
    for (int i = 0; i < s.acks; ++i) amx = std::max<uint32_t>(amx, s.ackMs[i]);
    LOG("probe: step %d/5 run %d %u kbit/s %u fps: frames %u dropped %u, sent %d, achieved %u of offered %u kbit/s "
        "(worst 1 s %u kbit/s), send p50/p90/p99/max %u/%u/%u/%u ms, acks %d/%d p50/p95/max %u/%u/%u ms -> %s",
        index + 1, runNo, static_cast<unsigned>(step.kbps), static_cast<unsigned>(step.fps),
        static_cast<unsigned>(s.frames), static_cast<unsigned>(s.dropped), s.sends,
        static_cast<unsigned>(probe::achievedKbps(s)), static_cast<unsigned>(s.offeredKbps),
        static_cast<unsigned>(s.worstSecondBytes * 8 / 1000),
        static_cast<unsigned>(probe::percentile(s.sendMs, s.sends, 50)),
        static_cast<unsigned>(probe::percentile(s.sendMs, s.sends, 90)),
        static_cast<unsigned>(probe::percentile(s.sendMs, s.sends, 99)), static_cast<unsigned>(mx), s.acks,
        s.acksExpected, static_cast<unsigned>(probe::percentile(s.ackMs, s.acks, 50)),
        static_cast<unsigned>(probe::percentile(s.ackMs, s.acks, 95)), static_cast<unsigned>(amx),
        pass ? "pass" : "fail");
}

bool runCapacity(Outcome& out, uint32_t& kbps) {
    uint64_t total = 0;
    uint64_t perSecond[kCapacityMs / 1000] = {};
    const uint64_t t0 = sys::nowMs();
    while (true) {
        const uint64_t elapsed = sys::nowMs() - t0;
        if (elapsed >= kCapacityMs) break;
        if (interrupted(out)) return false;
        if (!g_link->sendProbe(++g_seq, false, kCapacityFrameBytes)) {
            out = Outcome::Lost;
            return false;
        }
        total += kCapacityFrameBytes;
        perSecond[std::min<uint64_t>((sys::nowMs() - t0) / 1000, kCapacityMs / 1000 - 1)] += kCapacityFrameBytes;
    }
    const uint64_t took = std::max<uint64_t>(sys::nowMs() - t0, 1);
    uint64_t worst = perSecond[0];
    for (uint64_t v : perSecond) worst = std::min(worst, v);
    kbps = static_cast<uint32_t>(total * 8 / took);
    LOG("probe: capacity %u kbit/s (worst 1 s %u kbit/s)", static_cast<unsigned>(kbps),
        static_cast<unsigned>(worst * 8 / 1000));
    return true;
}

void fail(uint32_t id, int step, int runNo, Outcome out) {
    const char* reason = out == Outcome::Off ? "off" : "lost";
    LOG("probe: failed, reason %s", reason);
    writeResult(id, "failed", step, runNo, reason, false, 0, snapshot(), -1);
}

void run(uint32_t id) {
    LOG("probe: request %u", static_cast<unsigned>(id));
    resetSamples();
    if (power::halted() || power::captureHalted()) {
        LOG("probe: failed, reason off");
        writeResult(id, "failed", 1, 1, "off", false, 0, snapshot(), -1);
        return;
    }
    if (!g_link->connected()) {
        LOG("probe: failed, reason nopc");
        writeResult(id, "failed", 1, 1, "nopc", false, 0, snapshot(), -1);
        return;
    }

    int8_t result[5] = {-1, -1, -1, -1, -1};
    writeResult(id, "running", 1, 1, nullptr, false, 0, snapshot(), -1);
    const uint64_t startMs = sys::nowMs();
    g_link->setProbing(true);
    Outcome out = Outcome::Done;
    int tested = 1;
    const bool baseOk = runBaseline(out);
    if (out == Outcome::Done) {
        logBaseline();
        if (!baseOk) out = Outcome::Lost;
    }
    if (out != Outcome::Done) {
        g_link->setProbing(false);
        fail(id, 1, 1, out);
        return;
    }
    for (int i = 0; i < 5 && out == Outcome::Done; ++i) {
        int passes = 0, fails = 0, runNo = 0, verdict = -1;
        while (verdict == -1 && out == Outcome::Done) {
            ++runNo;
            const bool pass = runStep(probe::kSteps[i], out);
            if (out != Outcome::Done) break;
            logStep(i, runNo, probe::kSteps[i], pass);
            (pass ? passes : fails)++;
            verdict = probe::stepVerdict(passes, fails);
            writeResult(id, "running", i + 1, verdict == -1 ? runNo + 1 : runNo, nullptr, false, 0, snapshot(), -1);
        }
        if (out != Outcome::Done) {
            fail(id, i + 1, runNo, out);
            break;
        }
        result[i] = static_cast<int8_t>(verdict);
        tested = i + 1;
        if (verdict != 1) break;
    }
    int64_t capacity = -1;
    if (out == Outcome::Done) {
        uint32_t kbps = 0;
        if (runCapacity(out, kbps)) capacity = kbps;
        else fail(id, tested, 1, out);
    }
    g_link->setProbing(false);
    if (out != Outcome::Done) return;
    const int best = probe::pick(result);
    const unsigned total = static_cast<unsigned>((sys::nowMs() - startMs) / 1000);
    if (best >= 0)
        LOG("probe: pick %d (%u kbit/s %u fps), total %u s", best, static_cast<unsigned>(probe::kSteps[best].kbps),
            static_cast<unsigned>(probe::kSteps[best].fps), total);
    else
        LOG("probe: pick -1 (none), total %u s", total);
    writeResult(id, "done", tested, 1, nullptr, true, best, snapshot(), capacity);
}

void threadMain(void*) {
    uint32_t handled = settings::probeRequest.load();
    while (true) {
        sys::sleepMs(250);
        const uint32_t request = settings::probeRequest.load();
        if (request == handled) continue;
        handled = request;
        if (request != 0) run(request);
    }
}
}

namespace linkprobe {
bool start(ViewerLink& link) {
    g_link = &link;
    mutexInit(&g_mutex);
    Result rc = threadCreate(&g_thread, threadMain, nullptr, g_stack, sizeof g_stack, 0x30, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&g_thread);
    if (R_FAILED(rc)) {
        LOG("probe: thread start failed 0x%x", rc);
        return false;
    }
    return true;
}

void onAck(uint32_t seq) {
    sys::Lock lock(g_mutex);
    if (!g_collecting) return;
    for (auto& p : g_pending) {
        if (!p.used || p.seq != seq) continue;
        p.used = false;
        g_samples.addAck(static_cast<uint32_t>(sys::nowMs() - p.startMs));
        return;
    }
}
}
