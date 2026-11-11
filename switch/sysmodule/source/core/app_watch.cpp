#include "core/app_watch.hpp"

#include <switch.h>

#include <algorithm>
#include <cstring>

#include "capture/screen_capture.hpp"
#include "core/app_info.hpp"
#include "core/power.hpp"
#include "core/system.hpp"
#include "net/viewer_link.hpp"

namespace {
constexpr size_t kStackBytes = 0x4000;
constexpr uint64_t kTickMs = 1000;
constexpr uint64_t kInitRetryMs = 5000;
constexpr uint64_t kSuspendHoldMs = 3000;
constexpr uint32_t kUnknownElapsed = 0xFFFFFFFFu;
constexpr uint8_t kStateNone = 0;
constexpr uint8_t kStateForeground = 1;
constexpr uint8_t kStateSuspended = 2;
constexpr uint8_t kStateHomebrew = 3;
constexpr uint64_t kAlbumTid = 0x010000000000100Dull;
constexpr uint32_t kCmdGetProcessInfo = 65000;

struct HblOut {
    u64 programId;
    u8 storageId;
    u8 pad[7];
    u64 keysHeld;
    u64 flags;
};
static_assert(sizeof(HblOut) == 0x20);

alignas(0x1000) uint8_t g_stack[kStackBytes];
alignas(0x1000) NsApplicationControlData g_control;
char g_names[16][0x200];
Thread g_thread;
ViewerLink* g_link = nullptr;
const ScreenCapture* g_screen = nullptr;

char g_name[appinfo::kMaxName];
size_t g_nameLen = 0;

bool wallClock(uint64_t& seconds) {
    u64 t = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_UserSystemClock, &t))) return false;
    seconds = t;
    return true;
}

bool queryHbl(u64 pid, Result& rcOut) {
    Handle h = INVALID_HANDLE;
    HblOut out{};
    rcOut = serviceDispatchInOut(pmdmntGetServiceSession(), kCmdGetProcessInfo, pid, out,
                                 .out_handle_attrs = {SfOutHandleAttr_HipcCopy}, .out_handles = &h);
    if (h != INVALID_HANDLE) svcCloseHandle(h);
    return R_SUCCEEDED(rcOut) && (out.flags & 1);
}

void readName(uint64_t tid) {
    g_nameLen = 0;
    Result rc = nsInitialize();
    if (R_FAILED(rc)) {
        LOG("app: %016llx name unavailable (0x%x)", static_cast<unsigned long long>(tid), rc);
        return;
    }
    u64 actual = 0;
    rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, appinfo::baseTid(tid), &g_control,
                                     sizeof g_control, &actual);
    nsExit();
    if (R_FAILED(rc) || actual < sizeof(NacpStruct)) {
        LOG("app: %016llx name unavailable (0x%x)", static_cast<unsigned long long>(tid), R_FAILED(rc) ? rc : 0u);
        return;
    }
    for (int i = 0; i < 16; ++i) {
        std::memcpy(g_names[i], g_control.nacp.lang[i].name, sizeof g_names[i]);
        g_names[i][sizeof g_names[i] - 1] = '\0';
    }
    const size_t index = appinfo::pickName(g_names);
    if (index >= 16) {
        LOG("app: %016llx name unavailable (0x%x)", static_cast<unsigned long long>(tid), 0u);
        return;
    }
    g_nameLen = appinfo::clampUtf8(g_names[index], sizeof g_names[index], appinfo::kMaxName);
    std::memcpy(g_name, g_names[index], g_nameLen);
    LOG("app: %016llx name read (0x%x)", static_cast<unsigned long long>(tid), 0u);
}

void threadMain(void*) {
    bool pmdmnt = false, pminfo = false, timeOk = false;
    uint64_t lastInitMs = 0;
    uint64_t curTid = 0, launchSec = 0, suspendedSinceMs = 0;
    bool launchKnown = false;
    u64 observedPid = 0, failedPid = 0;
    bool hblActive = false, hblLaunchKnown = false;
    uint64_t hblLaunchSec = 0;
    uint64_t sentTid = ~0ull;
    uint8_t sentState = 0xFF;
    uint32_t sentSerial = 0;
    uint32_t initFailures = 0;
    uint8_t buffer[appinfo::kHeaderBytes + appinfo::kMaxName];

    while (true) {
        sys::sleepMs(kTickMs);
        if (power::captureHalted()) continue;

        if (!pmdmnt || !pminfo || !timeOk) {
            const uint64_t now = sys::nowMs();
            if (lastInitMs == 0 || now - lastInitMs >= kInitRetryMs) {
                lastInitMs = std::max<uint64_t>(now, 1);
                Result rc = 0, rc2 = 0, rc3 = 0;
                if (!pmdmnt) pmdmnt = R_SUCCEEDED(rc = pmdmntInitialize());
                if (!pminfo) pminfo = R_SUCCEEDED(rc2 = pminfoInitialize());
                if (!timeOk) timeOk = R_SUCCEEDED(rc3 = timeInitialize());
                if (!pmdmnt || !pminfo || !timeOk) {
                    if (++initFailures == 1 || initFailures % 12 == 0)
                        LOG("app: service init failed (pmdmnt 0x%x, pminfo 0x%x, time 0x%x), retrying in 5 s", rc,
                            rc2, rc3);
                }
            }
            if (!pmdmnt || !pminfo) continue;
        }

        u64 pid = 0, tid = 0;
        Result pidRc = pmdmntGetApplicationProcessId(&pid);
        if (pidRc == MAKERESULT(15, 1)) {
            tid = 0;
        } else if (R_FAILED(pidRc)) {
            continue;
        } else if (R_FAILED(pminfoGetProgramId(&tid, pid))) {
            continue;
        }

        const bool hasApp = pidRc == 0;
        u64 obsPid = hasApp ? pid : 0;
        if (!hasApp) {
            u64 albumPid = 0;
            if (R_SUCCEEDED(pmdmntGetProcessId(&albumPid, kAlbumTid))) obsPid = albumPid;
        }
        if (obsPid != observedPid) {
            bool hbl = false;
            if (obsPid != 0) {
                Result hrc = 0;
                hbl = queryHbl(obsPid, hrc);
                if (R_FAILED(hrc)) {
                    if (obsPid != failedPid) LOG("app: hbl query failed 0x%x", hrc);
                    failedPid = obsPid;
                    continue;
                }
            }
            observedPid = obsPid;
            if (hbl && !hblActive) {
                hblActive = true;
                uint64_t seconds = 0;
                hblLaunchKnown = timeOk && wallClock(seconds);
                if (hblLaunchKnown) hblLaunchSec = seconds;
                LOG("app: hbl detected (%s)", hasApp ? "application" : "album");
            } else if (!hbl && hblActive) {
                hblActive = false;
                hblLaunchKnown = false;
                LOG("app: hbl ended");
            }
        }

        if (!hblActive && tid != curTid) {
            curTid = tid;
            g_nameLen = 0;
            launchKnown = false;
            suspendedSinceMs = 0;
            if (tid != 0) {
                uint64_t seconds = 0;
                if (timeOk && wallClock(seconds)) {
                    launchSec = seconds;
                    launchKnown = true;
                }
                readName(tid);
            }
        }

        const uint64_t now = sys::nowMs();
        if (!hblActive && curTid != 0 && g_screen->gameSuspended()) {
            if (suspendedSinceMs == 0) suspendedSinceMs = now;
        } else {
            suspendedSinceMs = 0;
        }
        const uint64_t effTid = hblActive ? 0 : curTid;
        const uint8_t state = hblActive ? kStateHomebrew : curTid == 0 ? kStateNone
                              : (suspendedSinceMs != 0 && now - suspendedSinceMs >= kSuspendHoldMs) ? kStateSuspended
                                                                                                     : kStateForeground;

        const uint32_t serial = g_link->sessionSerial();
        if (!g_link->connected()) continue;
        if (effTid == sentTid && state == sentState && serial == sentSerial) continue;

        uint32_t elapsed = kUnknownElapsed;
        uint64_t seconds = 0;
        const bool known = hblActive ? hblLaunchKnown : (curTid != 0 && launchKnown);
        const uint64_t startSec = hblActive ? hblLaunchSec : launchSec;
        if (known && timeOk && wallClock(seconds) && seconds >= startSec && seconds - startSec < kUnknownElapsed)
            elapsed = static_cast<uint32_t>(seconds - startSec);

        const size_t size = appinfo::encode(buffer, sizeof buffer, state, effTid, elapsed, g_name,
                                            hblActive ? 0 : g_nameLen);
        if (size == 0) continue;
        if (g_link->sendAppInfo(buffer, size)) {
            sentTid = effTid;
            sentState = state;
            sentSerial = serial;
            LOG("app: sent %016llx state %u", static_cast<unsigned long long>(effTid), static_cast<unsigned>(state));
        }
    }
}
}

namespace appwatch {
bool start(ViewerLink& link, const ScreenCapture& screen) {
    g_link = &link;
    g_screen = &screen;
    Result rc = threadCreate(&g_thread, threadMain, nullptr, g_stack, sizeof g_stack, 0x3B, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&g_thread);
    if (R_FAILED(rc)) {
        LOG("app: thread start failed 0x%x", rc);
        return false;
    }
    return true;
}
}
