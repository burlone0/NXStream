#pragma once
#include <stratosphere.hpp>
#include <atomic>

namespace ams::mitm::applet {

    extern std::atomic<bool> g_shot_enabled;
    extern std::atomic<u32>  g_shot_combo;
    extern std::atomic<u32>  g_shot_mask;
    extern std::atomic<bool> g_shot_capture;
    extern std::atomic<bool> g_shot_album;
    void NoteAlbumPath();
    extern std::atomic<u32>  g_shot_count, g_shot_fail;

    void StartShotInput();
    void NotifyShot(const char *text);

    bool HomeButtonRecent(u32 within_ms);

    bool TakeShotRequest();
    bool ShotRequestPending();

    constexpr size_t ShotOutBufSize = 128 * 1024;
    bool WriteShot(u8 *out, const u8 *src, size_t src_size, u32 w, u32 h, u32 stride_bytes, u32 bh_log2, bool bgra = false, u32 flip = 0, u32 x0 = 0, u32 y0 = 0);

}
