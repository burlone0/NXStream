#pragma once
#include <stratosphere.hpp>
#include <atomic>
#include "applet_mitm_gbuf.hpp"

namespace ams::mitm::applet {

    struct GameSurface {
        u32 nvmap_id;
        u32 width;
        u32 height;
        u32 stride_px;
        u32 block_h_log2;
        u32 pix_format;
        u32 slot_offset[8];
        u32 num_slots;
        bool armed;
        u32 buf_size;
        u64 color_format;
        u32 layout;
        u32 generation;
        u32 slot_nvmap[8];
    };
    extern GameSurface g_game_surface;

    void CaptureGameSurface(const NvGraphicBufferRaw *gb, s32 slot);

    extern GameSurface g_applet_surface;
    void CaptureAppletSurface(const NvGraphicBufferRaw *gb, s32 slot);
    extern std::atomic<u64> g_applet_target_pid;
    extern std::atomic<u64> g_present_pid;
    extern std::atomic<u64> g_live_pid;
    extern std::atomic<bool> g_album_seed_no_binder;
    extern std::atomic<u64> g_app_present_tick;
    inline bool IsAlbumProgram(u64 tid) { return tid == UINT64_C(0x010000000000100D); }

    extern bool g_vic_armed;
    extern bool g_vic_execute;
    extern bool g_dbg_armed;
    extern u32  g_pmdmnt_rc;

    extern std::atomic<const char *> g_vic_stage;

    extern bool g_dump_armed;
    extern u32  g_probe_delay_s;

    extern bool g_bench_armed;
    extern bool g_nvenc_armed;
    extern bool g_sweep_armed;
    extern bool g_stream_auto;
    extern bool g_matrix_armed;
    extern bool g_grcscan_armed;
    extern bool g_jpgdec_armed;
    extern bool g_nvgrc_armed;
    extern bool g_csc_armed;
    extern bool g_nvframe_armed;
    extern u32  g_nvframe_n;
    extern bool g_nvstream_armed;
    extern u32  g_nvstream_n, g_nvstream_qp;
    extern u32  g_nvstream_gop;
    extern bool g_nvab_armed;
    extern std::atomic<u32> g_early_collect;
    constexpr u32 MidCollectDefault = 1;
    extern std::atomic<u32> g_mid_collect;
    extern std::atomic<u32> g_usb_gop;
    extern std::atomic<u32> g_max_fps;
    extern std::atomic<u32> g_live_frames;
    extern std::atomic<u64> g_live_read_ns, g_live_vic_ns;
    extern bool g_live_armed;
    extern std::atomic<bool> g_detach_on_home;
    extern bool g_nv1080_armed;
    extern u32  g_nv1080_n;
    extern bool g_anyfw_armed;
    extern bool g_cap720_armed;
    extern std::atomic<bool> g_scale1080;
    extern bool g_nvp_armed;
    extern u32  g_nvp_n;
    extern u32  g_matrix_mode;
    extern bool g_stream_armed;
    extern u32  g_stream_w, g_stream_h, g_stream_frames;

    extern std::atomic<u32> g_queue_count;
    extern std::atomic<s32> g_queue_slot;
    extern std::atomic<u64> g_queue_tick;
    extern std::atomic<u32> g_queue_fence_n;
    extern std::atomic<u32> g_queue_transform;
    extern std::atomic<u64> g_app_pid;
    extern std::atomic<u64> g_crop_full_pid;
    extern std::atomic<u32> g_queue_crop_wh;
    extern std::atomic<bool> g_queue_crop_origin0;
    extern std::atomic<u32> g_queue_crop_o;
    extern std::atomic<u32> g_queue_crop_ow;
    extern std::atomic<u32> g_queue_crop_c;
    extern std::atomic<bool> g_viewer_key_request;
    extern std::atomic<bool> g_viewer_can_ask;
    extern std::atomic<bool> g_viewer_crop;
    extern std::atomic<u32> g_size_lock;
    void ViewerLockSize(bool lock);
#define SFT_FILTER_AB_PROBE 0
    extern std::atomic<u64> g_queue_fence[4];
    extern std::atomic<u32> g_queue_seq;
    struct PresentRec {
        std::atomic<u32> seq;
        std::atomic<u32> count;
        std::atomic<s32> slot;
        std::atomic<u32> fence_n;
        std::atomic<u64> fence[4];
        std::atomic<u64> tick;
        std::atomic<u32> transform;
        std::atomic<u32> crop;
        std::atomic<u32> crop_c;
        std::atomic<u32> crop_o;
        std::atomic<u32> crop_ow;
    };
    constexpr u32 PresentRingSize = 8;
    extern PresentRec g_present_ring[PresentRingSize];

    void RecordPresent(s32 slot, bool have_fence, u32 fence_n, const u64 fence[4], u32 transform, const s32 crop[4]);
    extern std::atomic<bool> g_ui_session_active;
    extern std::atomic<bool> g_game_presented_in_ui;

    enum LiveState : u32 {
        LiveState_Starting   = 0,
        LiveState_Off        = 1,
        LiveState_WaitViewer = 2,
        LiveState_WaitGame   = 3,
        LiveState_Streaming  = 4,
        LiveState_Wedged     = 5,
        LiveState_NotArmed   = 6,
        LiveState_Unsupported = 7,
        LiveState_Excluded   = 8,
    };
    extern std::atomic<bool> g_stream_enabled;
    extern std::atomic<u64>  g_app_tid;
    extern std::atomic<u64>  g_vim_program;
    extern std::atomic<bool> g_app_excluded;
    extern std::atomic<bool> g_reconfig_request;
    extern std::atomic<u32>  g_live_state;
    extern std::atomic<bool> g_streaming_now;
    extern std::atomic<u32>  g_live_w, g_live_h, g_live_fps_x10, g_live_game_fps_x10, g_live_sessions;

    void StartVicWorker();

    void RequestVicBlit(s32 slot);

}
