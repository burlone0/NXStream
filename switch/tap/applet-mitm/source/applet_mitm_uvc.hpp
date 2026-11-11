#pragma once
#include <stratosphere.hpp>
#include <atomic>

namespace ams::mitm::applet {

    extern bool g_uvc_mode;
    extern bool g_uvc_raw;
    extern std::atomic<bool> g_uvc_streaming;
    extern std::atomic<bool> g_uvc_new_consumer;

    bool UvcSetupDevice();

    void UvcFrameSize(u32 *w, u32 *h);

    size_t UvcRawPack(u8 *dst, const u8 *src, size_t len, u32 frame_no);

    void UvcQueueFrame(const u8 *buf, size_t len);

    void UvcWaitSent();

    bool UvcHostStalled();

    bool UvcHostReading(u32 timeout_ms);

    u32 UvcDroppedCount();

    void UvcLogStats(const char *who);

}
