#pragma once
#include <stratosphere.hpp>
#include <atomic>

namespace ams::mitm::applet {

    struct Stats {
        std::atomic<u32> sessions;
        std::atomic<u32> getdisp;
        std::atomic<u32> relay;
        std::atomic<u32> txns;
    };
    extern Stats g_stats;

    void LogInit();
    extern bool g_release_mode;
    extern u32 g_arm_probe_rc;
    void LogLine(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

    void SetThreadBasePriority(s32 prio);
    s32 ThreadBasePriority();
    constexpr s32 ServerThreadPriority = 12;
    void LogDefer(bool on);
    bool LogQuiet();
    void LogFlushHeld();
    void LogFlushStale();
    void LogWriteHeldNow();

    #define SFT_ABORT_UNLESS(expr) do {                                                                   \
        const ::ams::Result sft_abort_rc_ = (expr);                                                       \
        if (R_FAILED(sft_abort_rc_)) {                                                                    \
            ::ams::mitm::applet::LogLine("FATAL: %s failed, rc=0x%x (%s:%d) - stopping the console here", \
                                         #expr, sft_abort_rc_.GetValue(), __FILE__, __LINE__);            \
            ::ams::mitm::applet::LogWriteHeldNow();                      \
            R_ABORT_UNLESS(sft_abort_rc_);                                                                \
        }                                                                                                 \
    } while (0)

    void LogMark(const char *what);

    bool UsbReady();
    bool UsbIsSuperSpeed();
    bool UsbSendBuffer(const void *buf, size_t len, size_t *out_sent);

    bool UsbPostAsync(const void *buf, size_t len, u32 *out_urb);
    bool UsbWaitAsync(u32 urb, size_t *out_sent);
    bool UsbViewerSaidBye();
    void SetScreenOffNow(bool on);
    void UsbByeStop();
    void StartUsbOutReader();
    void UsbOutPark();
    void UsbOutUnpark();

    bool UsbViewerPresent(u32 timeout_ms);
    void ViewerKeepAlive();
    enum ViewerReason : u32 {
        ViewerReason_None        = 0,
        ViewerReason_NoGame      = 1,
        ViewerReason_GameHidden  = 2,
        ViewerReason_AppletOpen  = 3,
        ViewerReason_Off         = 4,
        ViewerReason_Excluded    = 5,
        ViewerReason_Unsupported = 6,
        ViewerReason_Wedged      = 7,
        ViewerReason_NoMemory    = 8,
        ViewerReason_NotFound    = 9,
        ViewerReason_Starting    = 10,
        ViewerReason_Keyboard    = 11,
        ViewerReason_TooBig      = 12,
        ViewerReason_UsbSlow     = 13,
    };
    void SetViewerReason(ViewerReason r);
    void UsbFollowStreamSwitch();
    void NoteUsbLeftAlone();

    template<typename Manager>
    void LoopProcessSafely(Manager &m, const char *who) {
        u32 failures = 0;
        while (auto *h = m.WaitSignaled()) {
            if (const Result r = m.Process(h); R_FAILED(r)) {
                if (++failures <= 20) { LogLine("%s: a request could not be handled (rc=0x%x) - turned away, still running", who, r.GetValue()); }
            }
        }
    }

    void NoteThread(const os::ThreadType *t);
    void LogStackUse();

    void UiProbeStep();

}
