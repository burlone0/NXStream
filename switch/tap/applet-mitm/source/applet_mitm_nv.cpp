#include "applet_mitm_nv.hpp"
#include "applet_mitm_sm.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_dmdbg.hpp"
#include "applet_mitm_uisource.hpp"
#include "vic40_config.hpp"
#include "nvenc_drv_h264.h"
#include "applet_mitm_nvjpg.hpp"
#include "nvjpg_drv.h"
#include "applet_mitm_clk.hpp"
#include "nvjpg_dec_control.h"
#include "nvenc_grc_idr.h"
#include "nvenc_grc_hdrs.h"
#include "vic_csc_bt709.h"
#include "nvenc_grc_p.h"
#include "nvenc_1080p.h"
#include "applet_mitm_dbgpump.hpp"
#include "applet_mitm_shot.hpp"
#include "applet_mitm_pace.hpp"
#include "applet_mitm_control.hpp"
#include "applet_mitm_donor.hpp"
#include "applet_mitm_audio.hpp"
#include "applet_mitm_uvc.hpp"
#include "applet_mitm_net.hpp"
#include "applet_mitm_slotmatch.hpp"
#include "applet_mitm_idlework.hpp"
#include "applet_mitm_gop.hpp"
#include "applet_mitm_ovlprobe.hpp"
#include "applet_mitm_overlay.hpp"
#include "applet_mitm_vpad.hpp"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <utility>

namespace ams::mitm::applet {

    bool SendConsoleInfoIfDue();

    constinit GameSurface g_game_surface = {};
    constinit GameSurface g_applet_surface = {};
    constinit std::atomic<u64> g_applet_target_pid{0};
    constinit std::atomic<u64> g_present_pid{0};
    constinit std::atomic<u64> g_live_pid{0};
    constinit std::atomic<u64> g_app_present_tick{0};

    namespace {
        bool WebMenuUp() {
            const u64 lp = g_app_present_tick.load(std::memory_order_relaxed);
            if (lp != 0 && armTicksToNs(armGetSystemTick() - lp) < UINT64_C(500000000)) { return false; }
            return WebAppletOpen();
        }
    }
    constinit const GameSurface *g_session_surface = &g_game_surface;
    constinit bool g_vic_armed   = false;
    constinit bool g_vic_execute = false;
    constinit std::atomic<const char *> g_vic_stage{"idle"};
    constinit u64 g_game_aruid = 0;
    constinit bool g_dbg_armed = false;
    constinit u32  g_pmdmnt_rc = 0xFFFFFFFF;

    constinit bool g_bench_armed   = false;
    constinit bool g_nvenc_armed   = false;
    constinit bool g_sweep_armed   = false;
    constinit bool g_stream_auto   = true;
    constinit bool g_matrix_armed  = false;
    constinit bool g_grcscan_armed = false;
    constinit bool g_jpgdec_armed  = false;
    constinit bool g_nvgrc_armed   = false;
    constinit bool g_csc_armed     = false;
    constinit bool g_nvframe_armed = false;
    constinit u32  g_nvframe_n     = 120;
    constinit bool g_nvstream_armed = false;
    constinit u32  g_nvstream_n    = 3600;
    constinit u32  g_nvstream_qp   = 20;
    constinit u32  g_nvstream_gop  = 0;
    constinit bool g_live_armed    = false;
    constinit std::atomic<bool> g_detach_on_home{true};
    constinit u32 g_home_detach_ms   = 200;
    constinit u32 g_home_fallback_ms = 5000;
    constinit bool g_nv1080_armed  = false;
    constinit bool g_cap720_armed  = false;
    constinit std::atomic<bool> g_scale1080{false};
    constinit bool g_anyfw_armed   = false;
    constinit u32  g_nv1080_n      = 120;
    constinit bool g_nvp_armed     = false;
    constinit u32  g_nvp_n         = 30;
    constinit u32  g_matrix_mode   = 0;
    constinit bool g_stream_armed  = false;
    constinit u32  g_stream_w      = 480;
    constinit u32  g_stream_h      = 270;
    constinit u32  g_stream_frames = 600;

    namespace {

        constinit std::atomic<bool> g_blit_done{false};

        constexpr size_t VicCfgSize = 0x4000;
        constexpr size_t VicCmdSize = 0x1000;
        constexpr size_t VicDstSize = 0x340000;
        constexpr size_t VicSrcSize = 0x10000;
        constexpr size_t VicBufsEnd  = 0x360000;
        constexpr size_t VicBounceOff = 0x400000;
        constexpr size_t VicBounceSize = 0x200000;
        constexpr size_t VicBounceOverlayOff = 0xC0000;
        constexpr size_t VicBaseHeap = VicBounceOff + VicBounceSize;
        static_assert(VicBufsEnd <= VicBounceOff);
        constexpr size_t FbSlotBytes     = 8847360;
        constexpr size_t FbBlockRowStage = FbSlotBytes;
        constexpr size_t StreamStageSize = 0x220000;
        constinit size_t g_vic_heap_size = 0;

        constinit size_t g_ind_size = 0;

        constinit uintptr_t g_vic_heap    = 0;
        constinit bool g_engine_wedged = false;
        constinit u8       *g_vic_cfg_buf = nullptr;
        constinit u8       *g_vic_cmd_buf = nullptr;
        constinit u8       *g_vic_dst_buf = nullptr;
        constinit u8       *g_vic_src_buf = nullptr;
        constinit u8       *g_ind_buf     = nullptr;
        constexpr size_t AudioStageSize = 32 * 1024;
        constexpr size_t CaptureTailBytes = ShotOutBufSize + AudioStageSize + AudioRingBytes;
        constexpr size_t MinCaptureBytes = FbBlockRowStage + 3 * StreamStageSize + CaptureTailBytes;
        constexpr size_t SmallestDonorBytes = 22 * 1024 * 1024 - 0x40000;
        static_assert(VicBaseHeap + MinCaptureBytes <= SmallestDonorBytes,
                      "the donor's smallest size must hold the engine buffers and the capture region");
        constinit u8       *g_shot_out    = nullptr;
        constinit u8       *g_audio_stage = nullptr;
        constinit u8       *g_audio_ring  = nullptr;
        void SetCaptureTail(u8 *tail) {
            g_shot_out    = tail;
            g_audio_stage = tail != nullptr ? tail + ShotOutBufSize : nullptr;
            g_audio_ring  = tail != nullptr ? tail + ShotOutBufSize + AudioStageSize : nullptr;
        }
        constinit u8       *g_stage_buf   = nullptr;
        constinit u8       *g_stream_stage[2] = { nullptr, nullptr };

        alignas(0x1000) constinit u8 g_usb_hdr[0x1000] = {};

        void LogPools(const char *when) {
            static const char *const names[4] = { "Application", "Applet", "System", "SystemUnsafe" };
            for (u64 pool = 0; pool < 4; ++pool) {
                u64 tot = 0, used = 0;
                const auto r1 = ::ams::svc::GetSystemInfo(std::addressof(tot),  ::ams::svc::SystemInfoType_TotalPhysicalMemorySize, ::ams::svc::InvalidHandle, pool);
                const auto r2 = ::ams::svc::GetSystemInfo(std::addressof(used), ::ams::svc::SystemInfoType_UsedPhysicalMemorySize,  ::ams::svc::InvalidHandle, pool);
                if (R_FAILED(r1) || R_FAILED(r2)) {
                    LogLine("   [%s] pool %llu %-12s rc=%x/%x", when,
                            static_cast<unsigned long long>(pool), names[pool], r1.GetValue(), r2.GetValue());
                    continue;
                }
                LogLine("   [%s] pool %llu %-12s used=%8llu KB  free=%9lld KB", when,
                        static_cast<unsigned long long>(pool), names[pool],
                        static_cast<unsigned long long>(used / 1024),
                        static_cast<long long>((static_cast<s64>(tot) - static_cast<s64>(used)) / 1024));
            }
        }

        constinit bool g_donor_mode = false;

        void SetVicLayout(uintptr_t addr, size_t size) {
            g_vic_heap_size = size;
            g_vic_heap    = addr;
            g_vic_cfg_buf = reinterpret_cast<u8 *>(addr + 0x0000);
            g_vic_cmd_buf = reinterpret_cast<u8 *>(addr + 0x4000);
            g_vic_dst_buf = reinterpret_cast<u8 *>(addr + 0x10000);
            g_vic_src_buf = reinterpret_cast<u8 *>(addr + 0x350000);
            g_ind_buf = nullptr;
            SetCaptureTail(nullptr);
            g_ind_size = 0;
            g_stage_buf = nullptr;
            g_stream_stage[0] = g_stream_stage[1] = nullptr;
            std::memset(reinterpret_cast<void *>(addr), 0, VicBaseHeap);
        }

        bool AllocVicHeap() {
            if (g_vic_heap != 0) { return true; }

            {
                uintptr_t daddr = 0;
                size_t dsize = 0;
                if (DonorObtain(std::addressof(daddr), std::addressof(dsize))) {
                    if (dsize >= VicBaseHeap + MinCaptureBytes) {
                        g_donor_mode = true;
                        SetVicLayout(daddr, VicBaseHeap);
                        g_vic_heap_size = dsize;
                        LogLine("   VIC heap from the donor: %zu MB at 0x%lx (engine buffers %zu MB, capture region %zu MB)",
                                dsize >> 20, static_cast<unsigned long>(daddr), VicBaseHeap >> 20, (dsize - VicBaseHeap) >> 20);
                        return true;
                    }
                    LogLine("   donor gave only %zu MB (streaming needs %zu)", dsize >> 20, (VicBaseHeap + MinCaptureBytes + 0xFFFFF) >> 20);
                    DonorRelease();
                } else {
                    LogLine("   no donor");
                }
                if (g_release_mode) { LogLine("   not streaming: no memory from the donor (is atmosphere/contents/4200000000004E55 installed?)"); return false; }
            }

            size_t want = 0;
            {
                u64 t = 0, u = 0;
                ::ams::svc::GetInfo(std::addressof(t), ::ams::svc::InfoType_TotalMemorySize, ::ams::svc::PseudoHandle::CurrentProcess, 0);
                ::ams::svc::GetInfo(std::addressof(u), ::ams::svc::InfoType_UsedMemorySize,  ::ams::svc::PseudoHandle::CurrentProcess, 0);
                LogLine("   [probe, BEFORE heap grab] process total=%llu KB used=%llu KB free=%lld KB",
                        static_cast<unsigned long long>(t / 1024),
                        static_cast<unsigned long long>(u / 1024),
                        static_cast<long long>((static_cast<s64>(t) - static_cast<s64>(u)) / 1024));
            }
            LogPools("before heap grab");
            want = VicBaseHeap;
            if (const auto rc = os::SetMemoryHeapSize(want); R_FAILED(rc)) {
                LogLine("   SetMemoryHeapSize(%zu MB) rc=0x%x - no heap", want / (1024 * 1024), rc.GetValue());
                return false;
            }
            uintptr_t addr = 0;
            if (const auto rc = os::AllocateMemoryBlock(std::addressof(addr), want); R_FAILED(rc)) {
                LogLine("   AllocateMemoryBlock(%zu MB) FAILED rc=0x%x", want / (1024 * 1024), rc.GetValue());
                return false;
            }
            LogPools("after heap grab");
            g_vic_heap_size = want;
            g_vic_heap    = addr;
            g_vic_cfg_buf = reinterpret_cast<u8 *>(addr + 0x0000);
            g_vic_cmd_buf = reinterpret_cast<u8 *>(addr + 0x4000);
            g_vic_dst_buf = reinterpret_cast<u8 *>(addr + 0x10000);
            g_vic_src_buf = reinterpret_cast<u8 *>(addr + 0x350000);
            g_ind_buf = nullptr;
            SetCaptureTail(nullptr);
            g_ind_size = 0;
            g_stage_buf = nullptr;
            g_stream_stage[0] = g_stream_stage[1] = nullptr;
            std::memset(reinterpret_cast<void *>(addr), 0, want);
            LogLine("   VIC heap %zu MB at 0x%lx (engine buffers only; the capture region is taken per stream): cfg=%p cmd=%p dst=%p src=%p",
                    want / (1024 * 1024), static_cast<unsigned long>(addr),
                    static_cast<void *>(g_vic_cfg_buf), static_cast<void *>(g_vic_cmd_buf),
                    static_cast<void *>(g_vic_dst_buf), static_cast<void *>(g_vic_src_buf));
            return true;
        }

        constinit u32 g_capture_handles = 0;

        bool CaptureAcquire(const char *who) {
            if (g_ind_buf != nullptr) { return true; }
            if (g_vic_heap == 0) { return false; }
            if (g_donor_mode) {
                const size_t cap = g_vic_heap_size - VicBaseHeap - CaptureTailBytes;
                g_ind_buf  = reinterpret_cast<u8 *>(g_vic_heap + VicBaseHeap);
                g_ind_size = cap;
                SetCaptureTail(g_ind_buf + cap);
                g_stage_buf = g_ind_buf + FbBlockRowStage;
                g_stream_stage[0] = g_stage_buf + StreamStageSize;
                g_stream_stage[1] = g_stream_stage[0] + StreamStageSize;
                std::memset(g_ind_buf, 0, cap);
                LogLine("   %s: capture memory (donor): %zu MB at %p", who, cap / (1024 * 1024), static_cast<void *>(g_ind_buf));
                return true;
            }
            size_t total = 0;
            for (const size_t sz : { 42_MB, 38_MB, 34_MB, 30_MB, 26_MB, 22_MB, 18_MB, 14_MB, 12_MB }) {
                if (R_SUCCEEDED(os::SetMemoryHeapSize(sz))) { total = sz; break; }
            }
            if (total == 0) { LogLine("   %s: capture memory: no heap size accepted - the Applet pool is short (an applet open?)", who); return false; }
            const size_t block = total - VicBaseHeap;
            const size_t cap = block - CaptureTailBytes;
            uintptr_t addr = 0;
            if (const auto rc = os::AllocateMemoryBlock(std::addressof(addr), block); R_FAILED(rc)) {
                LogLine("   %s: capture memory: AllocateMemoryBlock(%zu MB) rc=0x%x", who, cap / (1024 * 1024), rc.GetValue());
                static_cast<void>(os::SetMemoryHeapSize(VicBaseHeap));
                return false;
            }
            g_ind_buf  = reinterpret_cast<u8 *>(addr);
            g_ind_size = cap;
            SetCaptureTail(g_ind_buf + cap);
            g_stage_buf = g_ind_buf + FbBlockRowStage;
            g_stream_stage[0] = g_stage_buf + StreamStageSize;
            g_stream_stage[1] = g_stream_stage[0] + StreamStageSize;
            g_vic_heap_size = total;
            std::memset(g_ind_buf, 0, cap);
            LogLine("   %s: capture memory taken: %zu MB at %p (heap %zu MB)", who, cap / (1024 * 1024), static_cast<void *>(g_ind_buf), total / (1024 * 1024));
            return true;
        }

        void CaptureRelease(const char *who) {
            if (g_ind_buf == nullptr) { return; }
            static bool said_kept = false;
            if (g_engine_wedged || g_capture_handles != 0) {
                if (!said_kept) {
                    LogLine("   %s: capture memory kept (%s)", who, g_engine_wedged ? "an engine job never completed" : "an nvmap handle on it is still alive");
                    said_kept = true;
                }
                return;
            }
            if (g_donor_mode) {
                g_ind_buf = nullptr;
                SetCaptureTail(nullptr);
                g_ind_size = 0;
                g_stage_buf = nullptr;
                g_stream_stage[0] = g_stream_stage[1] = nullptr;
                LogLine("   %s: capture memory (donor) released", who);
                return;
            }
            os::FreeMemoryBlock(reinterpret_cast<uintptr_t>(g_ind_buf), g_ind_size + CaptureTailBytes);
            const auto rc = os::SetMemoryHeapSize(VicBaseHeap);
            LogLine("   %s: capture memory given back (%zu MB), heap back to %zu MB rc=0x%x", who, g_ind_size / (1024 * 1024), VicBaseHeap / (1024 * 1024), rc.GetValue());
            g_ind_buf = nullptr;
            SetCaptureTail(nullptr);
            g_ind_size = 0;
            g_stage_buf = nullptr;
            g_stream_stage[0] = g_stream_stage[1] = nullptr;
            g_vic_heap_size = VicBaseHeap;
        }

        constexpr u32 DstW        = 64;
        constexpr u32 DstH        = 64;
        constexpr u32 DstStridePx = 256;
        constexpr u32 DstPitch    = DstStridePx * 4;
        constexpr u32 DstSize     = DstPitch * DstH;
        static_assert(DstSize <= VicDstSize);

        constinit ::Service        g_nv_srv  = {};
        constinit os::NativeHandle g_nv_tmem_handle = os::InvalidNativeHandle;
        constinit u64 g_nv_tmem_size = 0;

        constexpr u32 MakeIowr(u32 type, u32 nr, u32 size) {
            return (UINT32_C(3) << 30) | (size << 16) | (type << 8) | nr;
        }
        constexpr u32 MakeIow(u32 type, u32 nr, u32 size) {
            return (UINT32_C(1) << 30) | (size << 16) | (type << 8) | nr;
        }
        constexpr u32 NvmapIocCreate = MakeIowr(0x01, 0x01, 8);
        constexpr u32 NvmapIocFromId = MakeIowr(0x01, 0x03, 8);
        constexpr u32 NvmapIocAlloc  = MakeIowr(0x01, 0x04, 32);
        constexpr u32 NvmapIocGetId  = MakeIowr(0x01, 0x0E, 8);
        constexpr u32 NvmapIocFree   = MakeIowr(0x01, 0x05, 24);
        constexpr u32 NvmapIocIsOwnedByAruid = MakeIow(0x01, 0x13, 16);

        constexpr u32 NvHostIocChannelGetSyncpoint  = MakeIowr(0x00, 0x02, 8);
        constexpr u32 NvHostIocChannelSetSubmitTo   = MakeIow (0x00, 0x07, 4);
        constexpr u32 NvHostIocChannelSetNvmapFd    = MakeIow (0x48, 0x01, 4);
        constexpr u32 NvHostIocChannelMapCmdBuf     = MakeIowr(0x00, 0x09, 20);
        constexpr u32 NvHostIocChannelUnmapCmdBuf   = MakeIowr(0x00, 0x0A, 20);
        constexpr u32 NvHostIocChannelMapCmdBufEx   = MakeIowr(0x00, 0x25, 20);
        constexpr u32 NvHostIocCtrlSyncptRead       = MakeIowr(0x00, 0x14, 8);
        constexpr u32 NvHostIocCtrlSyncptWait       = MakeIowr(0x00, 0x16, 12);
        constexpr u32 NvHostIocCtrlSyncptIncrW      = MakeIow (0x00, 0x15, 4);
        constexpr u32 NvHostIocCtrlSyncptIncrWR     = MakeIowr(0x00, 0x15, 4);

        struct MapCmdBufArgs {
            u32 num_handles;
            u32 reserved;
            u8  is_compr;
            u8  padding[3];
            u32 handle_id_in;
            u32 phys_addr_out;
        };
        static_assert(sizeof(MapCmdBufArgs) == 20);
        static_assert(__builtin_offsetof(MapCmdBufArgs, handle_id_in) == 12);

        constinit bool g_vic_quiet = false;

        void VicStage(const char *s) {
            g_vic_stage.store(s, std::memory_order_relaxed);
            if (!g_vic_quiet) { LogMark(s); }
        }

        ::Result NvIoctl(u32 fd, u32 request, void *argp, size_t argsz, u32 *out_err) {
            const struct { u32 fd; u32 request; } in = { fd, request };
            u32 error = 0;
            const ::Result rc = serviceDispatchInOut(std::addressof(g_nv_srv), 1, in, error,
                .buffer_attrs = {
                    SfBufferAttr_HipcAutoSelect | SfBufferAttr_In,
                    SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out,
                },
                .buffers = { { argp, argsz }, { argp, argsz } },
            );
            *out_err = error;
            return rc;
        }

        ::Result NvOpen(const char *path, u32 *out_fd, u32 *out_err) {
            struct { u32 fd; u32 error; } out = {};
            const ::Result rc = serviceDispatchOut(std::addressof(g_nv_srv), 0, out,
                .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
                .buffers      = { { path, std::strlen(path) } });
            *out_fd  = out.fd;
            *out_err = out.error;
            return rc;
        }

        ::Result NvSetAruidWithoutCheck(u64 aruid, u32 *out_err) {
            u32 err = 0;
            const ::Result rc = serviceDispatchInOut(std::addressof(g_nv_srv), 7, aruid, err);
            *out_err = err;
            return rc;
        }

        bool NvIsOwnedByAruid(u32 fd, u64 aruid, u32 handle) {
            struct { u64 aruid; u32 handle; u32 pad; } a = { aruid, handle, 0 };
            u32 nverr = 0;
            const ::Result rc = NvIoctl(fd, NvmapIocIsOwnedByAruid, std::addressof(a), sizeof(a), std::addressof(nverr));
            return R_SUCCEEDED(rc) && nverr == 0;
        }

        void NvClose(u32 fd) {
            const struct { u32 fd; } in = { fd };
            u32 e = 0;
            serviceDispatchInOut(std::addressof(g_nv_srv), 2, in, e);
        }

        ::Result NvmapOwn(u32 fd, void *cpu, u32 size, u8 kind, u32 *out_handle, u32 *out_id, bool cacheable = false);

        constinit u32 g_guard_nvmap_fd = 0;
        constinit u32 g_guard_next = 0x9000;

        bool MapCmdBuffer(u32 chan_fd, u32 handle, u32 *out_addr, const char *what, u8 is_compr = 0, u32 req = 0);

        bool AbsorbDeviceZero(u32 chan_fd, u32 req) {
            if (g_guard_nvmap_fd == 0 || g_vic_heap == 0 || g_guard_next >= 0x10000) { return false; }
            u32 gh = 0, gid = 0;
            if (R_FAILED(NvmapOwn(g_guard_nvmap_fd, reinterpret_cast<u8 *>(g_vic_heap + g_guard_next), 0x1000, 0, std::addressof(gh), std::addressof(gid), false))) { return false; }
            g_guard_next += 0x1000;
            MapCmdBufArgs a = {};
            a.num_handles  = 1;
            a.handle_id_in = gh;
            u32 nverr = 0;
            const ::Result rc = NvIoctl(chan_fd, req, std::addressof(a), sizeof(a), std::addressof(nverr));
            LogLine("   guard page mapped at 0x%x (rc=0x%x nverr=%u) - device address 0 is held from now on", a.phys_addr_out, rc, nverr);
            return R_SUCCEEDED(rc) && nverr == 0 && a.phys_addr_out == 0;
        }

        bool MapCmdBuffer(u32 chan_fd, u32 handle, u32 *out_addr, const char *what, u8 is_compr, u32 req) {
            MapCmdBufArgs a = {};
            a.num_handles  = 1;
            a.is_compr     = is_compr;
            a.handle_id_in = handle;
            u32 nverr = 0;
            if (req == 0) { req = NvHostIocChannelMapCmdBuf; }
            const ::Result rc = NvIoctl(chan_fd, req, std::addressof(a), sizeof(a), std::addressof(nverr));
            LogLine("   MAP_CMD_BUFFER(%s handle=%u compr=%u req=0x%08x) rc=0x%x nverr=%u -> phys=0x%x",
                    what, handle, is_compr, req, rc, nverr, a.phys_addr_out);
            if (R_FAILED(rc) || nverr != 0) { return false; }
            if (a.phys_addr_out == 0 && AbsorbDeviceZero(chan_fd, req)) {
                MapCmdBufArgs u = {};
                u.num_handles  = 1;
                u.handle_id_in = handle;
                u32 ue = 0;
                NvIoctl(chan_fd, NvHostIocChannelUnmapCmdBuf, std::addressof(u), sizeof(u), std::addressof(ue));
                a = {};
                a.num_handles  = 1;
                a.is_compr     = is_compr;
                a.handle_id_in = handle;
                nverr = 0;
                const ::Result rc2 = NvIoctl(chan_fd, req, std::addressof(a), sizeof(a), std::addressof(nverr));
                LogLine("   MAP_CMD_BUFFER(%s) again -> phys=0x%x (rc=0x%x nverr=%u)", what, a.phys_addr_out, rc2, nverr);
                if (R_FAILED(rc2) || nverr != 0) { return false; }
            }
            *out_addr = a.phys_addr_out;
            return true;
        }

        void UnmapCmdBuffer(u32 chan_fd, u32 handle) {
            MapCmdBufArgs a = {};
            a.num_handles  = 1;
            a.handle_id_in = handle;
            u32 nverr = 0;
            NvIoctl(chan_fd, NvHostIocChannelUnmapCmdBuf, std::addressof(a), sizeof(a), std::addressof(nverr));
        }

        bool NvmapFree(u32 fd, u32 handle);

        void HoldDeviceZero(u32 chan_fd) {
            if (g_guard_nvmap_fd == 0 || g_vic_heap == 0 || g_guard_next >= 0x10000) { return; }
            u32 gh = 0, gid = 0;
            if (R_FAILED(NvmapOwn(g_guard_nvmap_fd, reinterpret_cast<u8 *>(g_vic_heap + g_guard_next), 0x1000, 0, std::addressof(gh), std::addressof(gid), false))) { return; }
            MapCmdBufArgs a = {};
            a.num_handles  = 1;
            a.handle_id_in = gh;
            u32 nverr = 0;
            const ::Result rc = NvIoctl(chan_fd, NvHostIocChannelMapCmdBuf, std::addressof(a), sizeof(a), std::addressof(nverr));
            if (R_SUCCEEDED(rc) && nverr == 0 && a.phys_addr_out == 0) {
                g_guard_next += 0x1000;
                LogLine("   device address 0 was free - a guard page holds it now");
                return;
            }
            if (R_SUCCEEDED(rc) && nverr == 0) { UnmapCmdBuffer(chan_fd, gh); }
            static_cast<void>(NvmapFree(g_guard_nvmap_fd, gh));
        }

        ::Result NvmapOwn(u32 fd, void *cpu, u32 size, u8 kind, u32 *out_handle, u32 *out_id, bool cacheable) {
            u32 nverr = 0;
            struct { u32 size; u32 handle; } cr = { size, 0 };
            ::Result rc = NvIoctl(fd, NvmapIocCreate, std::addressof(cr), sizeof(cr), std::addressof(nverr));
            if (R_FAILED(rc) || nverr != 0) { LogLine("   nvmapOwn CREATE fail rc=0x%x nverr=%u", rc, nverr); return rc; }

            struct {
                u32 handle; u32 heapmask; u32 flags; u32 align;
                u8 kind; u8 pad[7]; u64 addr;
            } al = {};
            al.handle = cr.handle;
            al.flags  = cacheable ? 1u : 0u;
            al.align  = 0x1000;
            al.kind   = kind;
            al.addr   = reinterpret_cast<u64>(cpu);
            rc = NvIoctl(fd, NvmapIocAlloc, std::addressof(al), sizeof(al), std::addressof(nverr));
            if (R_FAILED(rc) || nverr != 0) { LogLine("   nvmapOwn ALLOC fail rc=0x%x nverr=%u", rc, nverr); return rc; }

            armDCacheFlush(cpu, size);
            if (!cacheable) {
                const auto sma = svc::SetMemoryAttribute(reinterpret_cast<uintptr_t>(cpu), size,
                                                         svc::MemoryAttribute_Uncached,
                                                         svc::MemoryAttribute_Uncached);
                LogLine("   SetMemoryAttribute(%p, 0x%x, uncached) rc=0x%x", cpu, size, sma.GetValue());
            } else {
                LogLine("   nvmapOwn(%p, 0x%x) kept CACHEABLE - caller must flush before submit", cpu, size);
            }

            struct { u32 id; u32 handle; } gi = { 0, cr.handle };
            rc = NvIoctl(fd, NvmapIocGetId, std::addressof(gi), sizeof(gi), std::addressof(nverr));
            *out_handle = cr.handle;
            *out_id     = gi.id;
            LogLine("   nvmapOwn ok handle=%u id=%u kind=%u size=0x%x", cr.handle, gi.id, kind, size);
            return rc;
        }

        bool NvmapFree(u32 fd, u32 handle) {
            struct { u32 handle; u32 pad; u64 address; u32 size; u32 flags; } a = { handle, 0, 0, 0, 0 };
            static_assert(sizeof(a) == 24);
            u32 nverr = 0;
            const ::Result rc = NvIoctl(fd, NvmapIocFree, std::addressof(a), sizeof(a), std::addressof(nverr));
            if (R_FAILED(rc) || nverr != 0) {
                LogLine("   nvmap FREE(handle %u) rc=0x%x nverr=%u - failed", handle, rc, nverr);
                return false;
            }
            if (a.address == 0 && a.size == 0) {
                LogLine("   nvmap FREE(handle %u): our handle is gone, the memory is still referenced elsewhere", handle);
                return false;
            }
            return true;
        }

        enum class VicJob { Fill, BlitSelf, BlitGame, BlitStrip };

        struct SrcDesc { u32 w, h, stride_px, blk_kind, blk_h_log2, pixfmt, cache_w, rect_w, rect_h; u32 rect_x = 0, rect_y = 0; bool bgra = false; };

        constexpr SrcDesc SelfSrc { DstW, DstH, DstStridePx, vic::BLK_KIND_PITCH, 0, vic::PIXFMT_A8B8G8R8, vic::CACHE_WIDTH_64Bx4, DstW, DstH };

        struct OutDesc { u32 w, h, stride_px, fmt; u32 blk_kind = vic::BLK_KIND_PITCH; u32 blk_h_log2 = 0; };

        constexpr OutDesc SelfOut { DstW, DstH, DstStridePx, vic::PIXFMT_A8B8G8R8 };

        constexpr u32 StripW = 1920, StripH = 128;
        constexpr SrcDesc StripSrc { StripW, StripH, StripW,
                                     vic::BLK_KIND_GENERIC_16Bx2, 4, vic::PIXFMT_A8B8G8R8,
                                     vic::CACHE_WIDTH_64Bx4, StripW, StripH };
        static_assert(StripSrc.rect_w > 0 && StripSrc.rect_h > 0,
                      "a zero rect underflows to 0xFFFFFFFF in FillBlitConfig");
        constinit SrcDesc g_strip_src = StripSrc;

        struct StripVariant { const char *name; u32 src_fmt; u32 out_fmt; u32 rect_w, rect_h; };
        constexpr StripVariant StripVariants[] = {
            { "BEST",    vic::PIXFMT_A8R8G8B8, vic::PIXFMT_A8B8G8R8, StripW, StripH },
            { "ONE2ONE", vic::PIXFMT_A8R8G8B8, vic::PIXFMT_A8B8G8R8, 480,    32     },
        };
        constexpr u32 StripVariantCount = sizeof(StripVariants) / sizeof(StripVariants[0]);

        constexpr OutDesc StripOut { 480, 32, 512, vic::PIXFMT_A8B8G8R8 };
        constinit OutDesc g_strip_out = StripOut;
        constinit u32 g_strip_flip = 0;
        constexpr u32 StripOutSize = StripOut.stride_px * 4 * StripOut.h;
        static_assert(StripOutSize <= DstSize);

        void FillOutputConfig(vic::VicConfigStruct *c, const OutDesc &out) {
            c->outputConfig.TargetRectLeft   = 0;
            c->outputConfig.TargetRectTop    = 0;
            c->outputConfig.TargetRectRight  = out.w - 1;
            c->outputConfig.TargetRectBottom = out.h - 1;

            c->outputSurfaceConfig.OutPixelFormat   = out.fmt;
            c->outputSurfaceConfig.OutBlkKind       = out.blk_kind;
            c->outputSurfaceConfig.OutBlkHeight     = out.blk_h_log2;
            c->outputSurfaceConfig.OutSurfaceWidth  = out.w - 1;
            c->outputSurfaceConfig.OutSurfaceHeight = out.h - 1;
            c->outputSurfaceConfig.OutLumaWidth     = out.stride_px - 1;
            c->outputSurfaceConfig.OutLumaHeight    = out.h - 1;
            if (vic::IsNv12(out.fmt)) {
                c->outputSurfaceConfig.OutChromaWidth  = (out.stride_px / 2) - 1;
                c->outputSurfaceConfig.OutChromaHeight = (out.h / 2) - 1;

                auto s20 = [](int v) -> u64 { return static_cast<u64>(static_cast<u32>(v) & 0xFFFFFu); };
                auto &m = c->outColorMatrixStruct;
                if (!g_matrix_armed) { m.matrix_enable = 0; } else {
                m.matrix_coeff00 = s20(  66); m.matrix_coeff10 = s20( -38); m.matrix_coeff20 = s20( 112);
                m.matrix_coeff01 = s20( 129); m.matrix_coeff11 = s20( -74); m.matrix_coeff21 = s20( -94);
                m.matrix_coeff02 = s20(  25); m.matrix_coeff12 = s20( 112); m.matrix_coeff22 = s20( -18);
                m.matrix_coeff03 = s20(4096); m.matrix_coeff13 = s20(32768); m.matrix_coeff23 = s20(32768);
                m.matrix_r_shift = 8;
                m.matrix_enable  = 1;
                }
                if (g_matrix_mode == 2) {
                    m.matrix_enable = 0;
                    auto &sm = c->slotStruct[0].colorMatrixStruct;
                    sm.matrix_coeff00 = s20(  66); sm.matrix_coeff10 = s20( -38); sm.matrix_coeff20 = s20( 112);
                    sm.matrix_coeff01 = s20( 129); sm.matrix_coeff11 = s20( -74); sm.matrix_coeff21 = s20( -94);
                    sm.matrix_coeff02 = s20(  25); sm.matrix_coeff12 = s20( 112); sm.matrix_coeff22 = s20( -18);
                    sm.matrix_coeff03 = s20(4096); sm.matrix_coeff13 = s20(32768); sm.matrix_coeff23 = s20(32768);
                    sm.matrix_r_shift = 8;
                    sm.matrix_enable  = 1;
                }
            } else {
                c->outColorMatrixStruct.matrix_enable = 0;
                c->outputSurfaceConfig.OutChromaWidth   = 16383;
                c->outputSurfaceConfig.OutChromaHeight  = 16383;
            }
        }

        constexpr u32 Nv12PlaneRows(u32 rows, const OutDesc &out) {
            const u32 bh = (out.blk_kind == vic::BLK_KIND_PITCH) ? 1u : (8u << out.blk_h_log2);
            return (rows + bh - 1) / bh * bh;
        }
        constexpr u32 Nv12LumaBytes(const OutDesc &out) { return out.stride_px * Nv12PlaneRows(out.h, out); }
        constexpr u32 Nv12ChromaBytes(const OutDesc &out) { return out.stride_px * Nv12PlaneRows(out.h / 2, out); }
        static_assert(Nv12LumaBytes(OutDesc{ 1280, 720, 1280, vic::PIXFMT_Y8_U8V8_N420, vic::BLK_KIND_GENERIC_16Bx2, 2 }) == 1280 * 736);
        static_assert(Nv12ChromaBytes(OutDesc{ 1280, 720, 1280, vic::PIXFMT_Y8_U8V8_N420, vic::BLK_KIND_GENERIC_16Bx2, 2 }) == 1280 * 384);
        static_assert(Nv12LumaBytes(OutDesc{ 640, 360, 640, vic::PIXFMT_Y8_U8V8_N420 }) == 640 * 360, "pitch output unchanged");

        void FillClearConfig(vic::VicConfigStruct *c) {
            std::memset(c, 0, sizeof(*c));
            FillOutputConfig(c, SelfOut);
            c->outputConfig.BackgroundAlpha = 1023;
            c->outputConfig.BackgroundR     = 768;
            c->outputConfig.BackgroundG     = 512;
            c->outputConfig.BackgroundB     = 256;
        }

        constinit std::atomic<bool> g_enlarge_2tap{true};
        constinit std::atomic<bool> g_enlarged_window{false};

        void FillBlitConfig(vic::VicConfigStruct *c, const SrcDesc &src, const OutDesc &out) {
            std::memset(c, 0, sizeof(*c));

            FillOutputConfig(c, out);
            c->outputConfig.BackgroundAlpha  = 1023;
            c->outputConfig.BackgroundR      = 1023;
            c->outputConfig.BackgroundG      = 1023;
            c->outputConfig.BackgroundB      = 1023;

            vic::SlotConfig *slot = std::addressof(c->slotStruct[0].slotConfig);
            slot->SlotEnable          = 1;
            slot->CurrentFieldEnable  = 1;
            slot->PlanarAlpha         = 1023;
            slot->ConstantAlpha       = 1;
            slot->SourceRectLeft      = static_cast<u64>(src.rect_x) << 16;
            slot->SourceRectRight     = static_cast<u64>(src.rect_x + src.rect_w - 1) << 16;
            slot->SourceRectTop       = static_cast<u64>(src.rect_y) << 16;
            slot->SourceRectBottom    = static_cast<u64>(src.rect_y + src.rect_h - 1) << 16;
            slot->DestRectLeft        = 0;
            slot->DestRectRight       = out.w - 1;
            slot->DestRectTop         = 0;
            slot->DestRectBottom      = out.h - 1;
            slot->SoftClampHigh       = 1023;
            if (out.w > src.rect_w || out.h > src.rect_h) {
                g_enlarged_window.store(true, std::memory_order_relaxed);
                const u64 taps = g_enlarge_2tap.load(std::memory_order_relaxed) ? 1 : 0;
                slot->FilterLengthX = out.w > src.rect_w ? taps : 0;
                slot->FilterLengthY = out.h > src.rect_h ? taps : 0;
                static bool said = false;
                if (!said) { said = true; LogLine("   vic: enlarging %ux%u to %ux%u - 2-tap filter", src.rect_w, src.rect_h, out.w, out.h); }
            }

            vic::SlotSurfaceConfig *s = std::addressof(c->slotStruct[0].slotSurfaceConfig);
            s->SlotPixelFormat   = src.pixfmt;
            s->SlotBlkKind       = src.blk_kind;
            s->SlotBlkHeight     = src.blk_h_log2;
            s->SlotCacheWidth    = src.cache_w;
            s->SlotSurfaceWidth  = src.w - 1;
            s->SlotSurfaceHeight = src.h - 1;
            s->SlotLumaWidth     = src.stride_px - 1;
            s->SlotLumaHeight    = src.h - 1;
            s->SlotChromaWidth   = 16383;
            s->SlotChromaHeight  = 16383;
        }

        struct BlendSlot {
            u32 addr;
            SrcDesc src;
            u32 dst_l, dst_t, dst_r, dst_b;
            u32 k1;
            u8 src_c, dst_c, src_a, dst_a;
            bool constant_alpha;
            bool slot0_factors;
        };
        constinit const BlendSlot *g_ovl_slots = nullptr;
        constinit u32 g_ovl_nslots = 0;
        constexpr u32 MaxOverlaySlots = 2;

        void FillSlot(vic::VicConfigStruct *c, u32 idx, const BlendSlot &b) {
            vic::SlotConfig *slot = std::addressof(c->slotStruct[idx].slotConfig);
            slot->SlotEnable          = 1;
            slot->CurrentFieldEnable  = 1;
            slot->PlanarAlpha         = 1023;
            slot->ConstantAlpha       = b.constant_alpha ? 1 : 0;
            slot->SourceRectLeft      = 0;
            slot->SourceRectRight     = static_cast<u64>(b.src.rect_w - 1) << 16;
            slot->SourceRectTop       = 0;
            slot->SourceRectBottom    = static_cast<u64>(b.src.rect_h - 1) << 16;
            slot->DestRectLeft        = b.dst_l;
            slot->DestRectRight       = b.dst_r;
            slot->DestRectTop         = b.dst_t;
            slot->DestRectBottom      = b.dst_b;
            slot->SoftClampHigh       = 1023;

            vic::SlotSurfaceConfig *s = std::addressof(c->slotStruct[idx].slotSurfaceConfig);
            s->SlotPixelFormat   = b.src.pixfmt;
            s->SlotBlkKind       = b.src.blk_kind;
            s->SlotBlkHeight     = b.src.blk_h_log2;
            s->SlotCacheWidth    = b.src.cache_w;
            s->SlotSurfaceWidth  = b.src.w - 1;
            s->SlotSurfaceHeight = b.src.h - 1;
            s->SlotLumaWidth     = b.src.stride_px - 1;
            s->SlotLumaHeight    = b.src.h - 1;
            s->SlotChromaWidth   = 16383;
            s->SlotChromaHeight  = 16383;

            vic::BlendingSlotStruct *bl = std::addressof(c->slotStruct[idx].blendingSlotStruct);
            bl->AlphaK1 = b.k1;
            bl->SrcFactCMatchSelect = b.src_c;
            bl->DstFactCMatchSelect = b.dst_c;
            bl->SrcFactAMatchSelect = b.src_a;
            bl->DstFactAMatchSelect = b.dst_a;
            if (b.slot0_factors) {
                vic::BlendingSlotStruct *b0 = std::addressof(c->slotStruct[0].blendingSlotStruct);
                b0->AlphaK1 = 1023;
                b0->SrcFactCMatchSelect = vic::BLEND_SRCFACTC_K1;
                b0->DstFactCMatchSelect = vic::BLEND_DSTFACTC_ZERO;
                b0->SrcFactAMatchSelect = vic::BLEND_SRCFACTA_K1;
                b0->DstFactAMatchSelect = vic::BLEND_DSTFACTA_ZERO;
            }
        }

        u32 OverlaySlotsFor(const OutDesc &out, BlendSlot *ov) {
            OverlayPlane p[MaxOverlaySlots];
            const u32 n = OverlayPlanes(p, MaxOverlaySlots);
            for (u32 i = 0; i < n; ++i) {
                BlendSlot &b = ov[i];
                b = {};
                b.addr = p[i].dev;
                b.src = SrcDesc{ p[i].w, p[i].h, p[i].w, vic::BLK_KIND_GENERIC_16Bx2, 4, p[i].fmt, vic::CACHE_WIDTH_64Bx4, p[i].w, p[i].h };
                const u32 r = (p[i].x + p[i].dw) * out.w / 1280, bt = (p[i].y + p[i].dh) * out.h / 720;
                b.dst_l = p[i].x * out.w / 1280;
                b.dst_t = p[i].y * out.h / 720;
                b.dst_r = (r > out.w ? out.w : r) - 1;
                b.dst_b = (bt > out.h ? out.h : bt) - 1;
                b.k1 = 1023;
            }
            return n;
        }

        constexpr u32 Host1xIncr0x10x2 = (UINT32_C(1) << 28) | (0x10u << 16) | 2u;

        constexpr u32 CfgAddrWord = 8, DstAddrWord = 11, SrcAddrWord = 14;

        u32 BuildCmdbuf(u32 *w, VicJob job, u32 cfg_addr, u32 dst_addr, u32 src_addr, bool set_class,
                        u32 chroma_addr = 0, const BlendSlot *slots = nullptr, u32 nslots = 0) {
            const bool has_src  = (job == VicJob::BlitSelf || job == VicJob::BlitGame || job == VicJob::BlitStrip);
            u32 n = 0;
            if (set_class) { w[n++] = vic::Host1xOpcodeSetClass(0, vic::HOST1X_CLASS_VIC, 0); }
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_APPLICATION_ID >> 2; w[n++] = 1;
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_CONTROL_PARAMS >> 2;
            w[n++] = (static_cast<u32>(sizeof(vic::VicConfigStruct) / 16)) << 16;
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_CONFIG_STRUCT_OFFSET >> 2;
            w[n++] = cfg_addr >> 8;
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_OUTPUT_SURFACE_LUMA_OFFSET >> 2;
            w[n++] = dst_addr >> 8;
            if (chroma_addr != 0) {
                w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_OUTPUT_SURFACE_CHROMA_U_OFFSET >> 2;
                w[n++] = chroma_addr >> 8;
            }
            if (has_src) {
                w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_SURFACE0_SLOT0_LUMA_OFFSET >> 2;
                w[n++] = src_addr >> 8;
            }
            for (u32 i = 0; has_src && i < nslots; ++i) {
                w[n++] = Host1xIncr0x10x2; w[n++] = vic::SlotLumaOffsetMethod(1 + i) >> 2;
                w[n++] = slots[i].addr >> 8;
            }
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::EXECUTE >> 2; w[n++] = 1u << 8;
            return n;
        }

        void TryIndirectCapture();
        void TryDebugCapture(u32 vfd, u32 nvmap_fd, u32 cmd_handle, u32 syncpt, u32 cfg_addr, u32 dst_addr);

        void TryNvencPhaseA(u32 nvmap_fd, u32 cmd_handle);
        void TryNvencProbe(u32 nvmap_fd, u32 cmd_handle, u32 dst_handle);
        void TryNvjpgProbe(u32 nvmap_fd, u32 cmd_handle, u32 dst_handle);
        void TryNvjpgDecodeControl(u32 nvmap_fd, u32 cmd_handle, u32 dst_handle);
        void TryNvencGrcReplay(u32 nvmap_fd, u32 cmd_handle);
        void TryGrcScan();
        void TryCscSweep(u32 vfd, u32 cmd_handle, u32 syncpt, u32 cfg_addr, u32 dst_addr, u32 self_addr);
        void TryNvencRealFrames(::ams::svc::Handle dbg, u64 slot_base, u32 vfd, u32 nvmap_fd,
                                u32 cmd_handle, u32 vsyncpt, u32 cfg_addr);
        enum class StreamEnd { Done, GameGone, ViewerGone, EngineStall, Failed, Reconfigure, Disabled, Regrow };
        bool ShotStandalone();
        StreamEnd TryUvcRawStream(::ams::svc::Handle dbg, u64 slot_base, u32 vfd, u32 nvmap_fd,
                                  u32 cmd_handle, u32 vsyncpt, u32 cfg_addr, const GameSurface *geo);
        StreamEnd TryNvencStream(::ams::svc::Handle dbg, u64 slot_base, u32 vfd, u32 nvmap_fd,
                                 u32 cmd_handle, u32 vsyncpt, u32 cfg_addr, bool live = false,
                                 const GameSurface *geo = nullptr);
        void TryNvencPFrames(::ams::svc::Handle dbg, u64 slot_base, u32 vfd, u32 nvmap_fd,
                             u32 cmd_handle, u32 vsyncpt, u32 cfg_addr);
        void TryNvenc1080(::ams::svc::Handle dbg, u64 slot_base, u32 vfd, u32 nvmap_fd,
                          u32 cmd_handle, u32 vsyncpt, u32 cfg_addr);
        bool WriteSdVerified(const char *path, const u8 *src, size_t len, u8 *verify, u32 *out_fnv);
        bool WriteEngineOutputToSd(const char *path, const u8 *uncached_src, size_t len, u32 *out_fnv);

        struct CscProbe {
            char name[16];
            u32  where;
            u32  shift;
            s32  c[3][4];
        };
        constinit const CscProbe *g_csc_override = nullptr;

        struct JobCtx {
            u32 vfd, cmd_handle, syncpt;
            u32 cfg_addr, dst_addr, src_addr, src_off;
            u32 cfg_handle, dst_handle, src_handle;
            SrcDesc game_src;
            bool out_is_dst_buf = true;
        };

        constinit u32  g_ctrl_fd       = 0;
        constinit u64  g_vic_ns_total  = 0;
        constinit u64  g_vic_ns_worst  = 0;
        constinit u32  g_vic_jobs      = 0;
        constinit u32  g_vic_timeouts  = 0;

        u32 CtrlFd() {
            if (g_ctrl_fd == 0) {
                u32 fd = 0, e = 0;
                if (R_SUCCEEDED(NvOpen("/dev/nvhost-ctrl", std::addressof(fd), std::addressof(e))) && e == 0) {
                    g_ctrl_fd = fd;
                }
            }
            return g_ctrl_fd;
        }

        u32 AppendIncrSyncpt(u32 *w, u32 n, u32 syncpt, bool after_engine_op) {
            const u32 cond = after_engine_op ? vic::INCR_SYNCPT_COND_OP_DONE
                                             : vic::INCR_SYNCPT_COND_IMMEDIATE;
            w[n++] = vic::Host1xOpcodeNonIncr(vic::UCLASS_INCR_SYNCPT, 1);
            w[n++] = (cond << vic::INCR_SYNCPT_COND_SHIFT) | syncpt;
            return n;
        }

        bool RunOneJob(const char *stage, VicJob job, bool set_class, const JobCtx &c) {
            VicStage(stage);

            const bool needs_src = (job == VicJob::BlitSelf || job == VicJob::BlitGame || job == VicJob::BlitStrip);
            const u32 nslots = (job == VicJob::BlitStrip && g_ovl_slots != nullptr) ? (g_ovl_nslots < MaxOverlaySlots ? g_ovl_nslots : MaxOverlaySlots) : 0;
            u32 zero_slot = 0;
            for (u32 i = 0; i < nslots; ++i) { if (g_ovl_slots[i].addr == 0) { zero_slot = 1 + i; } }
            if (c.cfg_addr == 0 || c.dst_addr == 0 || (needs_src && c.src_addr == 0) || zero_slot != 0) {
                LogLine("   [%s] REFUSING to submit: cfg=0x%x dst=0x%x src=0x%x zero slot %u - a zero "
                        "address hangs the VIC and freezes the console",
                        stage, c.cfg_addr, c.dst_addr, c.src_addr, zero_slot);
                return false;
            }

            auto *cfg = reinterpret_cast<vic::VicConfigStruct *>(g_vic_cfg_buf);
            switch (job) {
                case VicJob::Fill:     FillClearConfig(cfg);            break;
                case VicJob::BlitSelf:  FillBlitConfig(cfg, SelfSrc,   SelfOut);  break;
                case VicJob::BlitGame:  FillBlitConfig(cfg, c.game_src, SelfOut); break;
                case VicJob::BlitStrip:
                    FillBlitConfig(cfg, g_strip_src, g_strip_out);
                    cfg->outputConfig.OutputFlipX = (g_strip_flip & 1) ? 1 : 0;
                    cfg->outputConfig.OutputFlipY = (g_strip_flip & 2) ? 1 : 0;
                    break;
            }
            if (g_csc_override != nullptr && g_csc_override->where != 0) {
                const CscProbe &p = *g_csc_override;
                auto s20 = [](s32 v) -> u64 { return static_cast<u64>(static_cast<u32>(v) & 0xFFFFFu); };
                cfg->outColorMatrixStruct.matrix_enable = 0;
                auto &m = (p.where == 2) ? cfg->slotStruct[0].colorMatrixStruct : cfg->outColorMatrixStruct;
                m.matrix_coeff00 = s20(p.c[0][0]); m.matrix_coeff01 = s20(p.c[0][1]); m.matrix_coeff02 = s20(p.c[0][2]); m.matrix_coeff03 = s20(p.c[0][3]);
                m.matrix_coeff10 = s20(p.c[1][0]); m.matrix_coeff11 = s20(p.c[1][1]); m.matrix_coeff12 = s20(p.c[1][2]); m.matrix_coeff13 = s20(p.c[1][3]);
                m.matrix_coeff20 = s20(p.c[2][0]); m.matrix_coeff21 = s20(p.c[2][1]); m.matrix_coeff22 = s20(p.c[2][2]); m.matrix_coeff23 = s20(p.c[2][3]);
                m.matrix_r_shift = p.shift;
                m.matrix_enable  = 1;
            }
            for (u32 i = 0; i < nslots; ++i) { FillSlot(cfg, 1 + i, g_ovl_slots[i]); }

            const OutDesc &job_out = (job == VicJob::BlitStrip) ? g_strip_out : SelfOut;
            const u32 chroma_addr = vic::IsNv12(job_out.fmt) ? (c.dst_addr + Nv12LumaBytes(job_out)) : 0u;

            auto *w = reinterpret_cast<u32 *>(g_vic_cmd_buf);
            u32 words = BuildCmdbuf(w, job, c.cfg_addr, c.dst_addr, c.src_addr + c.src_off, set_class, chroma_addr,
                                    g_ovl_slots, nslots);
            const u32 rw = set_class ? 1u : 0u;
            words = AppendIncrSyncpt(w, words, c.syncpt, true);

            if (!g_vic_quiet && c.out_is_dst_buf) { std::memset(g_vic_dst_buf, 0xAB, DstSize); }
            armDCacheFlush(g_vic_dst_buf, DstSize);
            armDCacheFlush(g_vic_cfg_buf, VicCfgSize);
            armDCacheFlush(g_vic_cmd_buf, VicCmdSize);
            const u64 job_t0 = armTicksToNs(armGetSystemTick());

            const u32 nr = 0u;

            alignas(8) u8 sb[16 + 12 + 3 * (16 + 4) + 20 + 4] = {};
            u32 off = 0;
            auto put = [&](u32 v) { std::memcpy(sb + off, std::addressof(v), 4); off += 4; };

            put(1); put(nr); put(1); put(1);
            put(c.cmd_handle); put(0); put(words);
            put(c.syncpt); put(1); put(0); put(0); put(0);
            const u32 fence_off = off; put(0);
            const u32 sz = off;

            const u32 req = (UINT32_C(3) << 30) | (sz << 16) | (0x00u << 8) | 0x01u;
            u32 nverr = 0, fence_val = 0;
            ::Result rc = NvIoctl(c.vfd, req, sb, sz, std::addressof(nverr));
            std::memcpy(std::addressof(fence_val), sb + fence_off, 4);
            if (!g_vic_quiet) {
                LogLine("   [%s] setcl=%d words=%u cmd[0..3]=%08x %08x %08x %08x",
                        stage, static_cast<int>(set_class), words, w[0], w[1], w[2], w[3]);
                LogLine("   [%s] req=0x%08x sz=%u nr=%u rc=0x%x nverr=%u -> fence=%u",
                        stage, req, sz, nr, rc, nverr, fence_val);
            }
            if (R_FAILED(rc) || nverr != 0) { LogLine("   [%s] SUBMIT REJECTED", stage); return false; }

            armDCacheFlush(g_vic_cmd_buf, VicCmdSize);
            if (!g_vic_quiet) LogLine("   [%s] resolved addrs: cfg=0x%08x dst=0x%08x src=0x%08x (<<8: 0x%x 0x%x 0x%x)",
                    stage, w[CfgAddrWord + rw], w[DstAddrWord + rw],
                    (job == VicJob::Fill) ? 0u : w[SrcAddrWord + rw],
                    w[CfgAddrWord + rw] << 8, w[DstAddrWord + rw] << 8,
                    (job == VicJob::Fill) ? 0u : (w[SrcAddrWord + rw] << 8));

            bool completed = false;
            const u32 cfd = CtrlFd();
            if (cfd != 0) {
                struct { u32 id; u32 thresh; u32 timeout; } a = { c.syncpt, fence_val, 100 };
                nverr = 0;
                rc = NvIoctl(cfd, NvHostIocCtrlSyncptWait, std::addressof(a), sizeof(a), std::addressof(nverr));
                struct { u32 id; u32 value; } r = { c.syncpt, 0 };
                u32 e2 = 0;
                NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e2));
                completed = (r.value >= fence_val);

                if (!completed) {
                    const u32 spins = g_vic_quiet ? 5000 : 300;
                    for (u32 spin = 0; spin < spins && !completed; ++spin) {
                        os::SleepThread(TimeSpan::FromMicroSeconds(100));
                        r.value = 0;
                        NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e2));
                        completed = (r.value >= fence_val);
                    }
                    if (!completed) { ++g_vic_timeouts; }
                }
                if (!g_vic_quiet) {
                    LogLine("   [%s] WAIT nverr=%u  syncpt=%u (want >= %u)%s",
                            stage, nverr, r.value, fence_val,
                            completed ? "  OP_DONE fired" : "  ENGINE DID NOT COMPLETE");
                }

                if (!g_vic_quiet && r.value < fence_val) {
                    VicStage("vb:RESCUE_INCR");
                    u32 ireq = NvHostIocCtrlSyncptIncrW;
                    for (u32 k = 0, missing = fence_val - r.value; k < missing && k < 64; k++) {
                        struct { u32 id; } ai = { c.syncpt };
                        u32 ae = 0;
                        NvIoctl(cfd, ireq, std::addressof(ai), sizeof(ai), std::addressof(ae));
                        if (k == 0 && ae != 0) { ireq = NvHostIocCtrlSyncptIncrWR;
                            NvIoctl(cfd, ireq, std::addressof(ai), sizeof(ai), std::addressof(ae)); }
                    }
                    if (!g_vic_quiet) LogLine("   [%s] rescued syncpt %u up to fence %u", stage, c.syncpt, fence_val);
                }
            }
            {
                const u64 dt = armTicksToNs(armGetSystemTick()) - job_t0;
                g_vic_ns_total += dt; ++g_vic_jobs;
                if (dt > g_vic_ns_worst) { g_vic_ns_worst = dt; }
            }

            armDCacheFlush(g_vic_dst_buf, DstSize);
            if (g_vic_quiet) { return completed; }
            if (!c.out_is_dst_buf) {
                LogLine("   [%s] output went to a caller-owned buffer at %#x - the caller checks and saves it", stage, c.dst_addr);
                return completed;
            }

            u32 sum = 0, changed = 0;
            for (u32 i = 0; i < DstSize; i++) {
                sum += g_vic_dst_buf[i];
                if (g_vic_dst_buf[i] != 0xAB) { changed++; }
            }
            char hex[3 * 32 + 1];
            int k = 0;
            for (u32 i = 0; i < 32; i++) { k += std::snprintf(hex + k, sizeof(hex) - k, "%02x ", g_vic_dst_buf[i]); }
            LogLine("   [%s] dst sum32=%u changed=%u/%u  %s", stage, sum, changed, DstSize,
                    (changed > 0) ? "*** ENGINE WROTE OUR MEMORY ***"
                                  : "(untouched - poison 0xAB intact)");
            LogLine("   [%s] dst[0..32]: %s", stage, hex);
            const u8 *mid = g_vic_dst_buf + (DstH / 2) * DstPitch;
            LogLine("   [%s] row %u: %02x %02x %02x %02x %02x %02x %02x %02x",
                    stage, DstH / 2, mid[0], mid[1], mid[2], mid[3], mid[4], mid[5], mid[6], mid[7]);
            return completed;
        }

        constexpr u32 CscW = 64, CscH = 64, CscStridePx = 256;
        constexpr s32 K8 = 1 << 8, K11 = 1 << 11, K12 = 1 << 12, K16 = 1 << 16, K19 = (1 << 19) - 1;
        constexpr CscProbe CscProbes[] = {
            { "none",       0, 0, {} },
            { "out_k8",     1, 0, { { K8, 0, 0, 0 },  { 0, K8, 0, 0 },  { 0, 0, K8, 0 } } },
            { "out_k12",    1, 0, { { K12, 0, 0, 0 }, { 0, K12, 0, 0 }, { 0, 0, K12, 0 } } },
            { "out_k16",    1, 0, { { K16, 0, 0, 0 }, { 0, K16, 0, 0 }, { 0, 0, K16, 0 } } },
            { "out_k19",    1, 0, { { K19, 0, 0, 0 }, { 0, K19, 0, 0 }, { 0, 0, K19, 0 } } },
            { "out_k16_s8", 1, 8, { { K16, 0, 0, 0 }, { 0, K16, 0, 0 }, { 0, 0, K16, 0 } } },
            { "out_off",    1, 0, { { 0, 0, 0, 256 }, { 0, 0, 0, 512 }, { 0, 0, 0, 768 } } },
            { "out_neg",    1, 0, { { K16, 0, 0, 0 }, { 0, -K16, 0, 768 }, { 0, 0, K16, 0 } } },
            { "out_dense",  1, 0, { { 1 * K11, 2 * K11, 3 * K11, 0 }, { 4 * K11, 5 * K11, 6 * K11, 0 }, { 7 * K11, 8 * K11, 9 * K11, 0 } } },
            { "slot_k16",   2, 0, { { K16, 0, 0, 0 }, { 0, K16, 0, 0 }, { 0, 0, K16, 0 } } },
            { "m64_bt601",  1, 8, { { 66, 129, 25, 4096 }, { -38, -74, 112, 32768 }, { 112, -94, -18, 32768 } } },
            { "bt709",      1, vic_csc::Shift, {
                { vic_csc::Bt709[0][0], vic_csc::Bt709[0][1], vic_csc::Bt709[0][2], vic_csc::Bt709[0][3] },
                { vic_csc::Bt709[1][0], vic_csc::Bt709[1][1], vic_csc::Bt709[1][2], vic_csc::Bt709[1][3] },
                { vic_csc::Bt709[2][0], vic_csc::Bt709[2][1], vic_csc::Bt709[2][2], vic_csc::Bt709[2][3] } } },
        };
        constexpr u32 CscNumProbes = sizeof(CscProbes) / sizeof(CscProbes[0]);
        constexpr u8 CscPatch[16][3] = {
            {  40,  40,  40 }, { 200,  40,  40 }, {  40, 200,  40 }, {  40,  40, 200 },
            { 200, 200,  40 }, { 200,  40, 200 }, {  40, 200, 200 }, { 200, 200, 200 },
            { 120, 120, 120 }, { 200, 120,  40 }, {  40, 200, 120 }, { 120,  40, 200 },
            { 160,  80, 200 }, {  80, 160,  40 }, { 200, 200, 120 }, {  60, 100, 160 },
        };
        struct __attribute__((packed)) CscFileHdr { u32 magic, version, count, w, h, src_fmt, out_fmt, reserved; };
        struct __attribute__((packed)) CscFileRec { char name[16]; u32 where, shift; s32 c[3][4]; u32 completed; };
        static_assert(sizeof(CscFileHdr) == 32 && sizeof(CscFileRec) == 76, "must match tools/vic_csc.py");
        constexpr size_t CscRecBytes = sizeof(CscFileRec) + CscW * CscH * 3 / 2;
        constexpr size_t CscFileBytes = sizeof(CscFileHdr) + CscW * CscH * 4 + CscNumProbes * CscRecBytes;
        static_assert(CscStridePx * 4 * CscH <= VicSrcSize, "the card must fit our source buffer");
        static_assert(CscStridePx * CscH * 3 / 2 <= DstSize, "the output must fit what RunOneJob poisons and flushes");

        void TryCscSweep(u32 vfd, u32 cmd_handle, u32 syncpt, u32 cfg_addr, u32 dst_addr, u32 self_addr) {
            VicStage("csc:1");
            LogLine("   ---- VIC COLOUR-MATRIX PROBES (M82/M83): %u jobs on a 64x64 card we own ----", CscNumProbes);
            if (g_stage_buf == nullptr || 2 * ((CscFileBytes + 0xFFF) & ~static_cast<size_t>(0xFFF)) > StreamStageSize
                || g_ind_size < FbBlockRowStage + StreamStageSize) {
                LogLine("   csc: no stage buffer - not running");
                return;
            }
            for (u32 y = 0; y < CscH; ++y) {
                u8 *row = g_vic_src_buf + y * CscStridePx * 4;
                for (u32 x = 0; x < CscW; ++x) {
                    const u8 *c = CscPatch[(y / 16) * 4 + (x / 16)];
                    row[x * 4 + 0] = c[0]; row[x * 4 + 1] = c[1]; row[x * 4 + 2] = c[2]; row[x * 4 + 3] = 0xFF;
                }
            }
            armDCacheFlush(g_vic_src_buf, VicSrcSize);

            u8 *const f = g_stage_buf;
            const CscFileHdr hdr = { 0x43534356u , 1, CscNumProbes, CscW, CscH,
                                     vic::PIXFMT_A8R8G8B8, vic::PIXFMT_Y8_U8V8_N420, 0 };
            std::memcpy(f, std::addressof(hdr), sizeof(hdr));
            size_t off = sizeof(hdr);
            for (u32 y = 0; y < CscH; ++y) { std::memcpy(f + off, g_vic_src_buf + y * CscStridePx * 4, CscW * 4); off += CscW * 4; }

            const JobCtx ctx{ vfd, cmd_handle, syncpt, cfg_addr, dst_addr, self_addr, 0, 0, 0, 0, SelfSrc };
            u32 n_ok = 0;
            for (u32 i = 0; i < CscNumProbes; ++i) {
                const CscProbe &p = CscProbes[i];
                g_strip_src = SrcDesc{ CscW, CscH, CscStridePx, vic::BLK_KIND_PITCH, 0, vic::PIXFMT_A8R8G8B8,
                                       vic::CACHE_WIDTH_64Bx4, CscW, CscH };
                g_strip_out = OutDesc{ CscW, CscH, CscStridePx, vic::PIXFMT_Y8_U8V8_N420 };
                g_csc_override = std::addressof(p);
                char stage[32];
                std::snprintf(stage, sizeof(stage), "csc:%s", p.name);
                const bool ok = RunOneJob(stage, VicJob::BlitStrip, true, ctx);
                g_csc_override = nullptr;
                if (ok) { ++n_ok; }

                CscFileRec rec = {};
                std::memcpy(rec.name, p.name, sizeof(rec.name));
                rec.where = p.where; rec.shift = p.shift; rec.completed = ok ? 1 : 0;
                std::memcpy(rec.c, p.c, sizeof(rec.c));
                std::memcpy(f + off, std::addressof(rec), sizeof(rec)); off += sizeof(rec);
                const u8 *yp = g_vic_dst_buf, *uvp = g_vic_dst_buf + CscStridePx * CscH;
                for (u32 y = 0; y < CscH; ++y)     { std::memcpy(f + off, yp + y * CscStridePx, CscW); off += CscW; }
                for (u32 y = 0; y < CscH / 2; ++y) { std::memcpy(f + off, uvp + y * CscStridePx, CscW); off += CscW; }
                const u32 cy = 16 * 1 + 8, cx7 = 16 * 3 + 8, cx1 = 16 * 1 + 8;
                LogLine("   csc[%-10s] %s  patch(200,200,200): Y=%u U=%u V=%u  patch(200,40,40): Y=%u U=%u V=%u",
                        p.name, ok ? "done" : "DID NOT COMPLETE",
                        yp[cy * CscStridePx + cx7], uvp[(cy / 2) * CscStridePx + (cx7 & ~1u)], uvp[(cy / 2) * CscStridePx + (cx7 & ~1u) + 1],
                        yp[8 * CscStridePx + cx1], uvp[4 * CscStridePx + (cx1 & ~1u)], uvp[4 * CscStridePx + (cx1 & ~1u) + 1]);
                if (!ok) { LogLine("   csc: stopping at the first job that did not complete"); break; }
            }
            u32 fnv = 0;
            const size_t slot = (off + 0xFFF) & ~static_cast<size_t>(0xFFF);
            WriteSdVerified("sdmc:/vic-csc.bin", f, off, f + slot, std::addressof(fnv));
            LogLine("   csc: %u of %u probes completed; %zu B -> sdmc:/vic-csc.bin (decode: tools/vic_csc.py)", n_ok, CscNumProbes, off);
            VicStage("csc:done");
        }

        bool OverlayBlendCheck(u32 vfd, u32 cmd_handle, u32 syncpt, u32 cfg_addr, u32 dst_addr, u32 self_addr) {
            constexpr u32 W = 64, H = 32, Stride = 256;
            constexpr size_t Surf = Stride * 4 * H;
            static_assert(2 * Surf <= VicSrcSize && Surf <= DstSize);
            constexpr u8 Band[4][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 255 }, { 255, 255, 255, 255 }, { 255, 255, 255, 128 } };
            constexpr u8 Bg[3] = { 40, 120, 200 };
            u8 *const s0 = g_vic_src_buf, *const s1 = g_vic_src_buf + Surf;
            std::memset(g_vic_src_buf, 0, 2 * Surf);
            for (u32 y = 0; y < H; ++y) {
                for (u32 x = 0; x < W; ++x) {
                    u8 *p = s0 + y * Stride * 4 + x * 4;
                    p[0] = Bg[0]; p[1] = Bg[1]; p[2] = Bg[2]; p[3] = 255;
                    std::memcpy(s1 + y * Stride * 4 + x * 4, Band[y / 8], 4);
                }
            }
            armDCacheFlush(g_vic_src_buf, VicSrcSize);
            const SrcDesc card{ W, H, Stride, vic::BLK_KIND_PITCH, 0, vic::PIXFMT_A8R8G8B8, vic::CACHE_WIDTH_64Bx4, W, H };
            g_strip_src = card;
            g_strip_out = OutDesc{ W, H, Stride, vic::PIXFMT_A8R8G8B8 };
            g_strip_flip = 0;
            BlendSlot b = {};
            b.addr = self_addr + static_cast<u32>(Surf);
            b.src = card;
            b.dst_r = W - 1; b.dst_b = H - 1;
            b.k1 = 1023;
            g_ovl_slots = std::addressof(b);
            g_ovl_nslots = 1;
            std::memset(g_vic_dst_buf, 0xAB, DstSize);
            const JobCtx ctx{ vfd, cmd_handle, syncpt, cfg_addr, dst_addr, self_addr, 0, 0, 0, 0, SelfSrc };
            const bool done = RunOneJob("ovl:check", VicJob::BlitStrip, true, ctx);
            g_ovl_slots = nullptr;
            g_ovl_nslots = 0;
            const u8 *o[4];
            for (u32 band = 0; band < 4; ++band) { o[band] = g_vic_dst_buf + (band * 8 + 4) * Stride * 4 + 32 * 4; }
            auto near = [](u32 v, u32 want, u32 tol) { return (v > want ? v - want : want - v) <= tol; };
            bool ok = done;
            for (u32 c = 0; c < 3; ++c) {
                ok = ok && near(o[0][c], Bg[c], 3)
                        && near(o[1][c], 0, 3)
                        && near(o[2][c], 255, 3)
                        && near(o[3][c], (255 * 128 + Bg[c] * 127) / 255, 4);
            }
            LogLine("   overlays: blend check %s - transparent %02x%02x%02x (bg %02x%02x%02x), black %02x%02x%02x, white %02x%02x%02x, half %02x%02x%02x",
                    !done ? "DID NOT COMPLETE" : (ok ? "passed" : "FAILED"),
                    o[0][0], o[0][1], o[0][2], Bg[0], Bg[1], Bg[2], o[1][0], o[1][1], o[1][2], o[2][0], o[2][1], o[2][2], o[3][0], o[3][1], o[3][2]);
            return ok;
        }

#if SFT_OVL_PROBE
        constexpr u32 BcW = 64, BcH = 32, BcStridePx = 256;
        constexpr size_t BcSurfBytes = BcStridePx * 4 * BcH;
        static_assert(2 * BcSurfBytes <= VicSrcSize, "both cards fit our source buffer");
        static_assert(BcSurfBytes <= DstSize, "the output fits what RunOneJob flushes");
        constexpr u8 BcBand[8][4] = {
            {   0,   0,   0,   0 },
            { 255, 255, 255, 255 },
            { 128, 128, 128, 128 },
            {  64,   0,   0,  64 },
            {   0,   0,   0, 128 },
            { 255,   0,   0,   0 },
            {   0,   0,   0, 255 },
            { 192,  96,  48, 192 },
        };
        struct BcVariant { char name[16]; u32 k1; u8 src_c, dst_c, src_a, dst_a; bool enable, constant_alpha, slot0_factors, scaled, block_linear; };
        constexpr BcVariant BcVariants[] = {
            { "off",        0,    0, 0, 0, 0, false, false, false, false, false },
            { "zero",       0,    0, 0, 0, 0, true,  false, false, false, false },
            { "premul",     1023, vic::BLEND_SRCFACTC_K1, vic::BLEND_DSTFACTC_NEG_K1_TIMES_SRC,
                                  vic::BLEND_SRCFACTA_K1, vic::BLEND_DSTFACTA_NEG_K1_TIMES_SRC, true, false, false, false, false },
            { "straight",   1023, vic::BLEND_SRCFACTC_K1_TIMES_SRC, vic::BLEND_DSTFACTC_NEG_K1_TIMES_SRC,
                                  vic::BLEND_SRCFACTA_K1, vic::BLEND_DSTFACTA_NEG_K1_TIMES_SRC, true, false, false, false, false },
            { "premul_s0",  1023, vic::BLEND_SRCFACTC_K1, vic::BLEND_DSTFACTC_NEG_K1_TIMES_SRC,
                                  vic::BLEND_SRCFACTA_K1, vic::BLEND_DSTFACTA_NEG_K1_TIMES_SRC, true, false, true,  false, false },
            { "constant",   1023, vic::BLEND_SRCFACTC_K1, vic::BLEND_DSTFACTC_NEG_K1_TIMES_SRC,
                                  vic::BLEND_SRCFACTA_K1, vic::BLEND_DSTFACTA_NEG_K1_TIMES_SRC, true, true,  false, false, false },
            { "premul_x1.5", 1023, vic::BLEND_SRCFACTC_K1, vic::BLEND_DSTFACTC_NEG_K1_TIMES_SRC,
                                  vic::BLEND_SRCFACTA_K1, vic::BLEND_DSTFACTA_NEG_K1_TIMES_SRC, true, false, false, true,  false },
            { "premul_bl",  1023, vic::BLEND_SRCFACTC_K1, vic::BLEND_DSTFACTC_NEG_K1_TIMES_SRC,
                                  vic::BLEND_SRCFACTA_K1, vic::BLEND_DSTFACTA_NEG_K1_TIMES_SRC, true, false, false, false, true  },
        };
        constexpr u32 BcNumVariants = sizeof(BcVariants) / sizeof(BcVariants[0]);
        struct __attribute__((packed)) BcFileHdr { u32 magic, version, count, w, h, reserved[3]; };
        struct __attribute__((packed)) BcFileRec { char name[16]; u32 completed, enable, constant_alpha, slot0_factors, scaled, block_linear; u8 k1_lo, k1_hi, src_c, dst_c, src_a, dst_a, pad[2]; };
        static_assert(sizeof(BcFileHdr) == 32 && sizeof(BcFileRec) == 48, "must match tools/vic_blend.py");
        constexpr size_t BcFileBytes = sizeof(BcFileHdr) + 2 * BcW * BcH * 4 + BcNumVariants * (sizeof(BcFileRec) + BcW * BcH * 4);
        constexpr const char *BcTrigger = "sdmc:/config/nxstream/tap/probe-blend.txt";
        constexpr const char *BcMarker  = "sdmc:/config/nxstream/tap/probe/blend-running.txt";

        bool FileThere(const char *path) {
            fs::DirectoryEntryType t;
            return R_SUCCEEDED(fs::GetEntryType(std::addressof(t), path));
        }

        u32 BcBlOffset(u32 xbyte, u32 y, u32 blk_h_log2) {
            const u32 block_sz = 512u << blk_h_log2;
            const u32 gob_y = y / 8, yy = y % 8, xb = xbyte % 64;
            return (xbyte / 64) * block_sz + gob_y * 512 + (xb / 32) * 256 + (yy / 2) * 64 + ((xb % 32) / 16) * 32 + (yy % 2) * 16 + (xb % 16);
        }

        void TryBlendCard(u32 vfd, u32 cmd_handle, u32 syncpt, u32 cfg_addr, u32 dst_addr, u32 self_addr) {
            if (!FileThere(BcTrigger)) { return; }
            static_cast<void>(fs::CreateDirectory("sdmc:/config/nxstream/tap/probe"));
            if (FileThere(BcMarker)) {
                LogLine("   blend card: it did not finish on an earlier boot (%s) - not running it again", BcMarker);
                return;
            }
            u8 *const f = g_vic_heap != 0 ? reinterpret_cast<u8 *>(g_vic_heap + VicBounceOff) : nullptr;
            if (f == nullptr || 2 * ((BcFileBytes + 0xFFF) & ~static_cast<size_t>(0xFFF)) > VicBounceSize) {
                LogLine("   blend card: no room to assemble its file - not running");
                return;
            }
            static_cast<void>(fs::CreateFile(BcMarker, 0));
            VicStage("blend:1");
            LogLine("   ---- VIC BLEND CARD (ovl2): %u jobs, slot 1 over slot 0 on %ux%u cards we own ----", BcNumVariants, BcW, BcH);

            u8 *const s0 = g_vic_src_buf, *const s1 = g_vic_src_buf + BcSurfBytes;
            const u32 s1_addr = self_addr + static_cast<u32>(BcSurfBytes);
            std::memset(g_vic_src_buf, 0, 2 * BcSurfBytes);
            for (u32 y = 0; y < BcH; ++y) {
                for (u32 x = 0; x < BcW; ++x) {
                    u8 *p = s0 + y * BcStridePx * 4 + x * 4;
                    p[0] = static_cast<u8>(x * 4); p[1] = static_cast<u8>(64 + y * 4); p[2] = static_cast<u8>(255 - x * 4); p[3] = 255;
                }
            }
            auto paint_pitch = [&]() {
                std::memset(s1, 0, BcSurfBytes);
                for (u32 y = 0; y < BcH; ++y) {
                    for (u32 x = 0; x < BcW; ++x) { std::memcpy(s1 + y * BcStridePx * 4 + x * 4, BcBand[y / 4], 4); }
                }
            };
            paint_pitch();

            const BcFileHdr hdr = { 0x434C4256u , 1, BcNumVariants, BcW, BcH, {} };
            std::memcpy(f, std::addressof(hdr), sizeof(hdr));
            size_t off = sizeof(hdr);
            for (u32 y = 0; y < BcH; ++y) { std::memcpy(f + off, s0 + y * BcStridePx * 4, BcW * 4); off += BcW * 4; }
            for (u32 y = 0; y < BcH; ++y) { std::memcpy(f + off, s1 + y * BcStridePx * 4, BcW * 4); off += BcW * 4; }

            const JobCtx ctx{ vfd, cmd_handle, syncpt, cfg_addr, dst_addr, self_addr, 0, 0, 0, 0, SelfSrc };
            const SrcDesc pitch_src{ BcW, BcH, BcStridePx, vic::BLK_KIND_PITCH, 0, vic::PIXFMT_A8B8G8R8, vic::CACHE_WIDTH_64Bx4, BcW, BcH };
            u32 n_ok = 0;
            for (u32 i = 0; i < BcNumVariants; ++i) {
                const BcVariant &v = BcVariants[i];
                if (v.block_linear) {
                    std::memset(s1, 0, BcSurfBytes);
                    for (u32 y = 0; y < BcH; ++y) {
                        for (u32 x = 0; x < BcW; ++x) {
                            for (u32 b = 0; b < 4; ++b) { s1[BcBlOffset(x * 4 + b, y, 4)] = BcBand[y / 4][b]; }
                        }
                    }
                }
                armDCacheFlush(g_vic_src_buf, VicSrcSize);
                g_strip_src = pitch_src;
                g_strip_out = OutDesc{ BcW, BcH, BcStridePx, vic::PIXFMT_A8B8G8R8 };
                g_strip_flip = 0;
                BlendSlot b = {};
                b.addr = s1_addr;
                b.src = v.block_linear ? SrcDesc{ BcW, BcH, BcW, vic::BLK_KIND_GENERIC_16Bx2, 4, vic::PIXFMT_A8B8G8R8, vic::CACHE_WIDTH_64Bx4, BcW, BcH }
                                       : pitch_src;
                if (v.scaled) { b.src.rect_w = 32; b.src.rect_h = 16; b.dst_l = 0; b.dst_t = 0; b.dst_r = 47; b.dst_b = 23; }
                else          { b.dst_l = 0; b.dst_t = 0; b.dst_r = BcW - 1; b.dst_b = BcH - 1; }
                b.k1 = v.k1; b.src_c = v.src_c; b.dst_c = v.dst_c; b.src_a = v.src_a; b.dst_a = v.dst_a;
                b.constant_alpha = v.constant_alpha; b.slot0_factors = v.slot0_factors;
                g_ovl_slots = std::addressof(b);
                g_ovl_nslots = v.enable ? 1 : 0;
                std::memset(g_vic_dst_buf, 0xAB, DstSize);
                char stage[32];
                std::snprintf(stage, sizeof(stage), "blend:%s", v.name);
                const bool ok = RunOneJob(stage, VicJob::BlitStrip, true, ctx);
                g_ovl_slots = nullptr;
                g_ovl_nslots = 0;
                if (ok) { ++n_ok; }

                BcFileRec rec = {};
                std::memcpy(rec.name, v.name, sizeof(rec.name));
                rec.completed = ok ? 1 : 0; rec.enable = v.enable; rec.constant_alpha = v.constant_alpha;
                rec.slot0_factors = v.slot0_factors; rec.scaled = v.scaled; rec.block_linear = v.block_linear;
                rec.k1_lo = static_cast<u8>(v.k1 & 0xFF); rec.k1_hi = static_cast<u8>(v.k1 >> 8);
                rec.src_c = v.src_c; rec.dst_c = v.dst_c; rec.src_a = v.src_a; rec.dst_a = v.dst_a;
                std::memcpy(f + off, std::addressof(rec), sizeof(rec)); off += sizeof(rec);
                for (u32 y = 0; y < BcH; ++y) { std::memcpy(f + off, g_vic_dst_buf + y * BcStridePx * 4, BcW * 4); off += BcW * 4; }
                char line[8 * 16 + 1];
                int k = 0;
                for (u32 band = 0; band < 8; ++band) {
                    const u8 *p = g_vic_dst_buf + (band * 4 + 1) * BcStridePx * 4 + 32 * 4;
                    k += std::snprintf(line + k, sizeof(line) - k, " %02x%02x%02x%02x", p[0], p[1], p[2], p[3]);
                }
                LogLine("   blend[%-11s] %s:%s", v.name, ok ? "done" : "DID NOT COMPLETE", line);
                if (!ok) { LogLine("   blend card: stopping at the first job that did not complete"); break; }
            }
            u32 fnv = 0;
            const size_t slot = (off + 0xFFF) & ~static_cast<size_t>(0xFFF);
            WriteSdVerified("sdmc:/config/nxstream/tap/probe/vic-blend.bin", f, off, f + slot, std::addressof(fnv));
            LogLine("   blend card: %u of %u jobs completed; %zu B -> probe/vic-blend.bin (tools/vic_blend.py)", n_ok, BcNumVariants, off);
            static_cast<void>(fs::DeleteFile(BcMarker));
            VicStage("blend:done");
        }
#endif

    }

    u8 *VicBounceBuffer() {
        return g_vic_heap != 0 ? reinterpret_cast<u8 *>(g_vic_heap + VicBounceOff) : nullptr;
    }
    size_t VicBounceBufferSize() { return VicBounceOverlayOff; }

    constinit bool g_dump_armed    = false;
    constinit u32  g_probe_delay_s = 120;

    constinit std::atomic<u32> g_queue_count{0};
    constinit std::atomic<s32> g_queue_slot{-1};
    constinit std::atomic<bool> g_album_seed_no_binder{false};
    constinit std::atomic<u64> g_queue_tick{0};
    constinit std::atomic<u32> g_queue_fence_n{0};
    constinit bool g_nvab_armed = false;
    constinit std::atomic<u32> g_early_collect{1};
    constinit std::atomic<u32> g_mid_collect{MidCollectDefault};
    constinit std::atomic<u32> g_usb_gop{keyframe::UsbGopDefault};
    constinit std::atomic<u32> g_max_fps{0};
    constinit std::atomic<u32> g_live_frames{0};
    constinit std::atomic<u64> g_live_read_ns{0}, g_live_vic_ns{0};
    namespace {
        inline void LiveFrameSent(u64 read_ns, u64 vic_ns) {
            g_live_frames.fetch_add(1, std::memory_order_relaxed);
            g_live_read_ns.fetch_add(read_ns, std::memory_order_relaxed);
            g_live_vic_ns.fetch_add(vic_ns, std::memory_order_relaxed);
        }
    }
    constinit std::atomic<u32> g_queue_transform{0};
    constinit std::atomic<u32> g_queue_crop_wh{0};
    constinit std::atomic<bool> g_queue_crop_origin0{true};
    constinit std::atomic<u32> g_queue_crop_c{0};
    constinit std::atomic<u32> g_queue_crop_o{0};
    constinit std::atomic<u32> g_queue_crop_ow{0};
    constinit std::atomic<bool> g_viewer_key_request{false};
    constinit std::atomic<bool> g_viewer_can_ask{false};
    constinit std::atomic<bool> g_viewer_crop{false};
    constinit std::atomic<bool> g_donor_regrow{false};
    constinit std::atomic<u32> g_size_lock{0};
    void ViewerLockSize(bool lock) {
        if (!lock) { g_size_lock.store(0, std::memory_order_relaxed); return; }
        if (g_size_lock.load(std::memory_order_relaxed) != 0) { return; }
        const u32 w = g_live_w.load(std::memory_order_relaxed), h = g_live_h.load(std::memory_order_relaxed);
        g_size_lock.store(g_streaming_now.load(std::memory_order_relaxed) && w != 0 ? (w << 16 | h) : ~0u, std::memory_order_relaxed);
    }
    constinit std::atomic<u64> g_queue_fence[4] = {};
    constinit std::atomic<u32> g_queue_seq{0};
    constinit PresentRec g_present_ring[PresentRingSize] = {};
    constinit std::atomic<bool> g_stream_enabled{true};
    constinit std::atomic<bool> g_reconfig_request{false};
    constinit std::atomic<u64>  g_app_tid{0};
    constinit std::atomic<u64>  g_vim_program{0};
    constinit std::atomic<bool> g_app_excluded{false};
    constinit std::atomic<u32>  g_live_state{LiveState_Starting};
    constinit std::atomic<bool> g_streaming_now{false};
    constinit std::atomic<u32>  g_live_w{0}, g_live_h{0}, g_live_fps_x10{0}, g_live_game_fps_x10{0}, g_live_sessions{0};

    namespace {
    void CaptureInto(GameSurface &surf, const char *who, const NvGraphicBufferRaw *gb, s32 slot_arg) {
        if (gb == nullptr || gb->num_planes == 0) { return; }
        const NvSurfaceRaw *p0 = std::addressof(gb->planes[0]);

        if (slot_arg == 0 || (slot_arg < 0 && static_cast<u32>(gb->nvmap_id) != surf.nvmap_id)) {
            for (auto &o : surf.slot_offset) { o = 0; }
            for (auto &o : surf.slot_nvmap) { o = 0; }
            surf.num_slots = 0;
            ++surf.generation;
        }
        const u32 slot = (slot_arg >= 0 && slot_arg < 8) ? static_cast<u32>(slot_arg) : surf.num_slots;
        if (slot < 8) {
            surf.slot_offset[slot] = p0->offset;
            surf.slot_nvmap[slot]  = static_cast<u32>(gb->nvmap_id);
            if (slot + 1 > surf.num_slots) { surf.num_slots = slot + 1; }
        }
        surf.buf_size     = gb->total_size;
        surf.color_format = p0->color_format;
        surf.layout       = p0->layout;
        surf.nvmap_id     = static_cast<u32>(gb->nvmap_id);
        surf.width        = p0->width;
        surf.height       = p0->height;
        const bool rgb565 = p0->color_format == 0x010A881210ULL;
        surf.stride_px    = p0->pitch / (rgb565 ? 2 : 4);
        surf.block_h_log2 = p0->block_height_log2;
        surf.pix_format   = rgb565 ? vic::PIXFMT_B5G6R5
                          : (p0->color_format == 0x0100D12120ULL) ? vic::PIXFMT_A8R8G8B8 : vic::PIXFMT_A8B8G8R8;
        surf.armed        = true;

        LogLine("   captured %s slot %u: nvmap=%u %ux%u stride_px=%u blk_h_log2=%u off=0x%x pixfmt=%u (slots=%u)",
                who, slot, surf.nvmap_id, surf.width, surf.height,
                surf.stride_px, surf.block_h_log2, p0->offset,
                surf.pix_format, surf.num_slots);
    }
    }

    void CaptureGameSurface(const NvGraphicBufferRaw *gb, s32 slot_arg) { CaptureInto(g_game_surface, "game", gb, slot_arg); }
    void CaptureAppletSurface(const NvGraphicBufferRaw *gb, s32 slot_arg) {
        CaptureInto(g_applet_surface, "applet", gb, slot_arg);
        g_album_seed_no_binder.store(false, std::memory_order_relaxed);
    }

    namespace {

        constinit std::atomic<s32> g_vic_pending{-1};
        constinit std::atomic<bool> g_rearm{false};
        constinit bool g_setup_failed = false;
        bool FaultInjectNvTmemOnce() {
            static bool done = false;
            if (done) { return false; }
            done = true;
            fs::DirectoryEntryType t;
            if (R_FAILED(fs::GetEntryType(std::addressof(t), "sdmc:/config/nxstream/tap/fault-nv.txt"))) { return false; }
            LogLine("   test: fault-nv.txt - failing this engine setup once");
            return true;
        }
        constinit s32 g_last_slot = 0;
        alignas(os::ThreadStackAlignment) constinit u8 g_vic_stack[24_KB];
        constinit os::ThreadType g_vic_thread;

    void RunVicBlit(s32 slot) {
        if (!g_vic_armed || (!g_game_surface.armed && !g_ui_stream_armed)) { return; }
        bool expected = false;
        if (!g_blit_done.compare_exchange_strong(expected, true)) { return; }

        const u32 sidx = (slot >= 0 && static_cast<u32>(slot) < g_game_surface.num_slots)
                       ? static_cast<u32>(slot) : 0;
        const u32 src_off = g_game_surface.slot_offset[sidx];

        u32 cfg_addr = 0, dst_addr = 0, src_addr = 0, self_addr = 0;

        for (u32 heap_fails = 0;; ++heap_fails) {
        if (g_live_armed && g_release_mode) {
            if (heap_fails == 0) { LogLine("   engines: set up when a viewer connects (until then: no heap, no channels)"); }
            VicStage("vb:wait_viewer");
            for (bool said_busy = false;;) {
                for (;;) {
                    UsbFollowStreamSwitch();
                    if (ShotRequestPending()) { break; }
                    if (UsbViewerPresent(300)) {
                        if (WebMenuUp()) {
                            SetViewerReason(ViewerReason_AppletOpen);
                            static const char *const WebMenuStage = "vb:web_menu";
                            if (g_vic_stage.load(std::memory_order_relaxed) != WebMenuStage) { VicStage(WebMenuStage); }
                            os::SleepThread(TimeSpan::FromMilliSeconds(100));
                            continue;
                        }
                        if (g_ui_stream_armed) { break; }
                        const u32 q0 = g_queue_count.load(std::memory_order_relaxed);
                        os::SleepThread(TimeSpan::FromMilliSeconds(250));
                        if (g_queue_count.load(std::memory_order_relaxed) != q0) { break; }
                        ::ams::os::ProcessId app{};
                        const bool have_app = g_pmdmnt_rc == 0 && R_SUCCEEDED(::ams::pm::dmnt::GetApplicationProcessId(std::addressof(app)));
                        SetViewerReason(have_app ? ViewerReason_GameHidden : ViewerReason_NoGame);
                        VicStage("vb:wait_game");
                    }
                    os::SleepThread(TimeSpan::FromMilliSeconds(500));
                }
                u64 mb = 0;
                const bool hold = AppletHoldActive();
                if (!hold && !AppletBusy(std::addressof(mb))) { break; }
                if (!hold && g_ui_stream_armed && SwitchUOpen()) {
                    static u64 s_headroom_ask_ns = 0;
                    const u64 now_ask = armTicksToNs(armGetSystemTick());
                    if (s_headroom_ask_ns == 0 || now_ask - s_headroom_ask_ns >= UINT64_C(30000000000)) {
                        s_headroom_ask_ns = now_ask;
                        if (!said_busy) {
                            LogLine("   engines: Album/SwitchU holds the Applet pool (%llu MB) - asking NeedHeadroom for the donor",
                                    static_cast<unsigned long long>(mb));
                            said_busy = true;
                        } else {
                            LogLine("   engines: retry NeedHeadroom (Applet pool still %llu MB used)",
                                    static_cast<unsigned long long>(mb));
                        }
                        HeadroomAskAndWait(42, 2500);
                        if (!AppletBusy(std::addressof(mb))) { break; }
                        HeadroomClear();
                        LogLine("   engines: SwitchU ACK but Applet pool still tight (%llu MB used, need ~42 MB free) - not holding NeedHeadroom (avoids icon thrash)",
                                static_cast<unsigned long long>(mb));
                    }
                    VicStage("vb:applet_busy");
                    SetViewerReason(ViewerReason_NoMemory);
                    os::SleepThread(TimeSpan::FromMilliSeconds(500));
                    continue;
                }
                if (TakeShotRequest()) {
                    LogLine("shot: an applet holds the Applet pool (%llu MB) - no screenshot now", static_cast<unsigned long long>(mb));
                    NotifyShot(KeyboardHoldsStream() ? "No screenshot while the keyboard is open" : "No screenshot while a manual, the eShop or another applet is open");
                }
                if (!said_busy) { LogLine("   engines: %s - waiting for it to close", hold ? "an applet is starting" : "an applet holds the Applet pool"); said_busy = true; }
                VicStage("vb:applet_busy");
                SetViewerReason(KeyboardHoldsStream() ? ViewerReason_Keyboard : ViewerReason_AppletOpen);
                os::SleepThread(TimeSpan::FromMilliSeconds(250));
            }
            g_applet_needs_memory.store(false, std::memory_order_relaxed);
            SetViewerReason(ViewerReason_Starting);
            LogLine("   engines: %s - setting up", ShotRequestPending() ? "a screenshot was asked for" : "a viewer is reading");
        }
        VicStage("vb:0_heap");
        if (AllocVicHeap()) { break; }
        VicStage("vb:0_FAILED");
        if (!(g_live_armed && g_release_mode)) { return; }
        if (TakeShotRequest()) { NotifyShot("Screenshot failed: not enough memory free"); }
        if (heap_fails % 12 == 0) { LogLine("   engines: no streaming memory - trying again in 5 s"); }
        SetViewerReason(ViewerReason_NoMemory);
        os::SleepThread(TimeSpan::FromSeconds(5));
        }
        if ((!g_live_armed || g_csc_armed) && !CaptureAcquire("tests")) { VicStage("vb:0_FAILED"); return; }

        VicStage("vb:1_smGetService");
        ::Result rc = 0;
        const char *nv_svc = "nvdrv:t";
        u64 &nv_tmem_size = g_nv_tmem_size;
        {
            SmUse sm_nv;
            rc = smGetService(std::addressof(g_nv_srv), "nvdrv:t");
            if (R_FAILED(rc)) {
                LogLine("   nvdrv:t unavailable rc=0x%x - falling back", rc);
                rc = smGetService(std::addressof(g_nv_srv), "nvdrv:s");
                nv_svc = "nvdrv:s";
            }
        }
        LogLine("   using %s rc=0x%x", nv_svc, rc);
        if (R_FAILED(rc)) { VicStage("vb:1_FAILED"); g_setup_failed = true; goto close_sess; }

        VicStage("vb:2_tmem");
        if (FaultInjectNvTmemOnce() || !DonorTakeNvTmem(std::addressof(g_nv_tmem_handle), std::addressof(nv_tmem_size))) {
            LogLine("   nvdrv transfer memory: none came with the donor's memory - no engines");
            VicStage("vb:2_FAILED"); g_setup_failed = true; goto close_sess;
        }

        VicStage("vb:3_Initialize");
        {
            const u32 tmem_size = static_cast<u32>(nv_tmem_size);
            rc = serviceDispatchIn(std::addressof(g_nv_srv), 3, tmem_size,
                .in_num_handles = 2,
                .in_handles     = { CUR_PROCESS_HANDLE, g_nv_tmem_handle });
            LogLine("   Initialize rc=0x%x", rc);
            if (R_FAILED(rc)) { VicStage("vb:3_FAILED"); goto close_sess; }
        }

        u32 nvmap_fd, nverr;

        if (g_live_armed) {
            LogLine("   live mode: our own nvdrv identity (no game aruid, no FROM_ID)");
        } else {
            VicStage("vb:4a_discover_aruid");
            u32 tfd = 0, terr = 0;
            if (R_SUCCEEDED(NvOpen("/dev/nvmap", std::addressof(tfd), std::addressof(terr))) && terr == 0) {
                struct { u32 id; u32 handle; } a = { g_game_surface.nvmap_id, 0 };
                u32 e = 0;
                if (R_SUCCEEDED(NvIoctl(tfd, NvmapIocFromId, std::addressof(a), sizeof(a), std::addressof(e))) && e == 0) {
                    for (u64 v = 1; v <= 256 && g_game_aruid == 0; v++) {
                        if (NvIsOwnedByAruid(tfd, v, a.handle)) { g_game_aruid = v; }
                    }
                }
                NvClose(tfd);
            }
            LogLine("   discovered game aruid = %llu (on a throwaway fd, now closed)",
                    static_cast<unsigned long long>(g_game_aruid));

            if (g_game_aruid != 0) {
                u32 aerr = 0;
                const ::Result ar = NvSetAruidWithoutCheck(g_game_aruid, std::addressof(aerr));
                LogLine("   SetAruidWithoutCheck(%llu) BEFORE any real Open: rc=0x%x err=%u",
                        static_cast<unsigned long long>(g_game_aruid), ar, aerr);
            }
        }

        VicStage("vb:4_open_nvmap");
        rc = NvOpen("/dev/nvmap", std::addressof(nvmap_fd), std::addressof(nverr));
        LogLine("   /dev/nvmap rc=0x%x fd=%u nverr=%u", rc, nvmap_fd, nverr);
        if (R_FAILED(rc) || nverr != 0) { VicStage("vb:4_FAILED"); goto close_sess; }

        {
            u32 gfd = 0, gerr = 0;
            const ::Result gr = NvOpen("/dev/nvhost-gpu", std::addressof(gfd), std::addressof(gerr));
            LogLine("   perm probe: /dev/nvhost-gpu rc=0x%x nverr=%u -> %s",
                    gr, gerr, (R_SUCCEEDED(gr) && gerr == 0) ? "OPEN (bit0 set: full mask)"
                                                             : "denied (restricted mask)");
            if (R_SUCCEEDED(gr) && gerr == 0) { NvClose(gfd); }
        }

        u32 src_handle;
        src_handle = 0;
        {
            VicStage("vb:4b_node_survey");
            static const char *const nodes[] = {
                "/dev/nvhost-display", "/dev/nvdisp-ctrl", "/dev/nvdisp-disp0",
                "/dev/nvdisp-disp1",   "/dev/nvdcutil-disp0", "/dev/nvcec-ctrl",
                "/dev/nvhost-as-gpu",  "/dev/nvhost-ctrl-gpu", "/dev/nvhost-msenc",
                "/dev/nvhost-nvdec",   "/dev/nvhost-tsec",     "/dev/nvhost-nvjpg",
            };
            for (const char *n : nodes) {
                u32 fd = 0, e = 0;
                const ::Result r = NvOpen(n, std::addressof(fd), std::addressof(e));
                const bool ok = R_SUCCEEDED(r) && e == 0;
                LogLine("   node %-22s %s (rc=0x%x nverr=%u)", n, ok ? "OPEN" : "denied", r, e);
                if (ok) { NvClose(fd); }
            }
        }

        VicStage("vb:5_FROM_ID");
        if (!g_live_armed) {
            struct { u32 id; u32 handle; } a = { g_game_surface.nvmap_id, 0 };
            rc = NvIoctl(nvmap_fd, NvmapIocFromId, std::addressof(a), sizeof(a), std::addressof(nverr));
            LogLine("   FROM_ID(%u) under aruid %llu rc=0x%x nverr=%u -> handle=%u",
                    g_game_surface.nvmap_id, static_cast<unsigned long long>(g_game_aruid),
                    rc, nverr, a.handle);
            if (R_FAILED(rc) || nverr != 0) { VicStage("vb:5_FAILED"); goto close_sess; }
            src_handle = a.handle;
        }

        u32 cfg_handle, cfg_id, cmd_handle, cmd_id, dst_handle, dst_id, self_handle, self_id;
        VicStage("vb:6_alloc_bufs");
        rc = NvmapOwn(nvmap_fd, g_vic_cfg_buf, VicCfgSize, 0, std::addressof(cfg_handle), std::addressof(cfg_id));
        if (R_FAILED(rc)) { VicStage("vb:6_cfg_FAILED"); goto close_sess; }
        rc = NvmapOwn(nvmap_fd, g_vic_cmd_buf, VicCmdSize, 0, std::addressof(cmd_handle), std::addressof(cmd_id));
        if (R_FAILED(rc)) { VicStage("vb:6_cmd_FAILED"); goto close_sess; }
        rc = NvmapOwn(nvmap_fd, g_vic_dst_buf, VicDstSize, 0, std::addressof(dst_handle), std::addressof(dst_id));
        if (R_FAILED(rc)) { VicStage("vb:6_dst_FAILED"); goto close_sess; }
        rc = NvmapOwn(nvmap_fd, g_vic_src_buf, VicSrcSize, 0, std::addressof(self_handle), std::addressof(self_id));
        if (R_FAILED(rc)) { VicStage("vb:6_self_FAILED"); goto close_sess; }
        AMS_UNUSED(cfg_id, cmd_id, dst_id, self_id);
        AMS_UNUSED(cfg_handle, dst_handle, src_handle, src_off);

        u32 vfd, syncpt;
        VicStage("vb:7_open_vic");
        rc = NvOpen("/dev/nvhost-vic", std::addressof(vfd), std::addressof(nverr));
        LogLine("   /dev/nvhost-vic rc=0x%x fd=%u nverr=%u", rc, vfd, nverr);
        if (R_FAILED(rc) || nverr != 0) { VicStage("vb:7_FAILED"); goto close_sess; }
        {
            struct { u32 module_id; u32 syncpt; } gs = { 0, 0 };
            rc = NvIoctl(vfd, NvHostIocChannelGetSyncpoint, std::addressof(gs), sizeof(gs), std::addressof(nverr));
            LogLine("   GET_SYNCPOINT rc=0x%x nverr=%u -> syncpt=%u", rc, nverr, gs.syncpt);
            if (R_FAILED(rc) || nverr != 0) { VicStage("vb:7_syncpt_FAILED"); goto close_vic; }
            syncpt = gs.syncpt;
        }

        VicStage("vb:8_set_nvmap_fd");
        {
            struct { u32 fd; } a = { nvmap_fd };
            rc = NvIoctl(vfd, NvHostIocChannelSetNvmapFd, std::addressof(a), sizeof(a), std::addressof(nverr));
            LogLine("   SET_NVMAP_FD(%u) rc=0x%x nverr=%u", nvmap_fd, rc, nverr);
        }
        {
            struct { u32 timeout; } a = { 1000 };
            rc = NvIoctl(vfd, NvHostIocChannelSetSubmitTo, std::addressof(a), sizeof(a), std::addressof(nverr));
            LogLine("   SET_SUBMIT_TIMEOUT rc=0x%x nverr=%u", rc, nverr);
        }

        if (g_vic_execute) {
            {
                VicStage("vb:8_guard");
                g_guard_nvmap_fd = nvmap_fd;
                u32 gh = 0, gid = 0, gaddr = 0;
                if (R_SUCCEEDED(NvmapOwn(nvmap_fd, reinterpret_cast<u8 *>(g_vic_heap + 0x8000), 0x1000, 0, std::addressof(gh), std::addressof(gid)))) {
                    MapCmdBuffer(vfd, gh, std::addressof(gaddr), "guard (holds device address 0)");
                }
            }
            VicStage("vb:8a_map_cfg");
            if (!MapCmdBuffer(vfd, cfg_handle, std::addressof(cfg_addr), "cfg")) { VicStage("vb:8a_FAILED"); goto close_vic; }
            VicStage("vb:8b_map_dst");
            if (!MapCmdBuffer(vfd, dst_handle, std::addressof(dst_addr), "dst")) { VicStage("vb:8b_FAILED"); goto close_vic; }
            VicStage("vb:8c_map_src");
            if (src_handle != 0) {
                MapCmdBuffer(vfd, src_handle, std::addressof(src_addr), "src(game)", 0);
                if (src_addr == 0) { MapCmdBuffer(vfd, src_handle, std::addressof(src_addr), "src(game)", 1); }
                if (src_addr == 0) { MapCmdBuffer(vfd, src_handle, std::addressof(src_addr), "src(game)", 0, NvHostIocChannelMapCmdBufEx); }
            }
            VicStage("vb:8d_map_self");
            MapCmdBuffer(vfd, self_handle, std::addressof(self_addr), "src(ours)", 0);
            LogLine("   pinned: cfg=0x%x dst=0x%x self=0x%x game_src=0x%x (+slot off 0x%x)",
                    cfg_addr, dst_addr, self_addr, src_addr, src_off);
            if (cfg_addr == 0 || dst_addr == 0 || self_addr == 0) {
                LogLine("   engines: a buffer of ours got device address 0 (cfg=0x%x dst=0x%x self=0x%x) - setting up again", cfg_addr, dst_addr, self_addr);
                VicStage("vb:8_zero_retry");
                goto close_vic;
            }

            for (u32 y = 0; y < DstH; y++) {
                u8 *row = g_vic_src_buf + y * DstPitch;
                for (u32 x = 0; x < DstW; x++) {
                    row[x * 4 + 0] = 0xFF;
                    row[x * 4 + 1] = static_cast<u8>(x * 4);
                    row[x * 4 + 2] = static_cast<u8>(y * 4);
                    row[x * 4 + 3] = 0x11;
                }
            }
            armDCacheFlush(g_vic_src_buf, VicSrcSize);
            LogLine("   self-src pattern: row0[0..8]=%02x %02x %02x %02x %02x %02x %02x %02x",
                    g_vic_src_buf[0], g_vic_src_buf[1], g_vic_src_buf[2], g_vic_src_buf[3],
                    g_vic_src_buf[4], g_vic_src_buf[5], g_vic_src_buf[6], g_vic_src_buf[7]);
        }

        {
            const SrcDesc game_src{ g_game_surface.width, g_game_surface.height,
                                    g_game_surface.stride_px, vic::BLK_KIND_GENERIC_16Bx2,
                                    g_game_surface.block_h_log2, g_game_surface.pix_format,
                                    vic::CACHE_WIDTH_64Bx4,
                                    g_game_surface.width, g_game_surface.height };
            const JobCtx ctx{ vfd, cmd_handle, syncpt, cfg_addr, dst_addr,
                              (self_addr != 0) ? self_addr : src_addr, 0,
                              cfg_handle, dst_handle, src_handle, game_src };

            if (!RunOneJob("vb:job_fill", VicJob::Fill, true, ctx)) {
                LogLine("   fill did not complete - engine wedged, skipping the rest");
                VicStage("vb:ABORTED_after_fill");
            } else if (self_addr == 0) {
                LogLine("   no address for our own source buffer; nothing further to try");
                VicStage("vb:no_self_addr");
            } else {
                RunOneJob("vb:job_blit_self", VicJob::BlitSelf, true, ctx);
            }

            if (src_addr == 0) {
                LogLine("   game blit still not possible: handle pins to phys=0");
                VicStage("vb:game_blit_SKIPPED");
            }
        }
        VicStage("vb:ALL_JOBS_DONE");

#if SFT_OVL_PROBE
        {
            static bool s_blend_tried = false;
            if (!s_blend_tried && self_addr != 0 && cfg_addr != 0 && dst_addr != 0 && !g_engine_wedged) {
                s_blend_tried = true;
                TryBlendCard(vfd, cmd_handle, syncpt, cfg_addr, dst_addr, self_addr);
            }
        }
#endif

        if (OverlaysArmed()) {
            static bool s_checked = false;
            if (!s_checked && self_addr != 0 && cfg_addr != 0 && dst_addr != 0 && !g_engine_wedged) {
                s_checked = true;
                OverlaySetCalibrated(OverlayBlendCheck(vfd, cmd_handle, syncpt, cfg_addr, dst_addr, self_addr));
            }
        }

        if (g_csc_armed && self_addr != 0 && cfg_addr != 0 && dst_addr != 0 && !g_engine_wedged) {
            TryCscSweep(vfd, cmd_handle, syncpt, cfg_addr, dst_addr, self_addr);
        }

        if (!g_live_armed) { TryIndirectCapture(); }
        if (dst_addr != 0) {
            OverlayAreaUp(g_vic_dst_buf + DstSize, dst_addr + static_cast<u32>(DstSize), VicDstSize - DstSize,
                          reinterpret_cast<u8 *>(g_vic_heap + VicBounceOff + VicBounceOverlayOff), VicBounceSize - VicBounceOverlayOff);
        }
        TryDebugCapture(vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr, dst_addr);
        if (g_grcscan_armed) { TryGrcScan(); }
        if (g_jpgdec_armed) { TryNvjpgDecodeControl(nvmap_fd, cmd_handle, dst_handle); }
        if (g_nvgrc_armed)  { TryNvencGrcReplay(nvmap_fd, cmd_handle); }
        if (g_nvenc_armed) { TryNvencPhaseA(nvmap_fd, cmd_handle); }
        if (g_nvenc_armed) { TryNvencProbe(nvmap_fd, cmd_handle, dst_handle); }
        if (g_nvjpg_armed) { TryNvjpgProbe(nvmap_fd, cmd_handle, dst_handle); }

    close_vic:
        OverlayAreaDown();
        if (cfg_addr != 0) { UnmapCmdBuffer(vfd, cfg_handle); }
        if (dst_addr != 0) { UnmapCmdBuffer(vfd, dst_handle); }
        if (src_addr != 0)  { UnmapCmdBuffer(vfd, src_handle); }
        if (self_addr != 0) { UnmapCmdBuffer(vfd, self_handle); }
        NvClose(vfd);
    close_sess:
        serviceClose(std::addressof(g_nv_srv));
        if (g_nv_tmem_handle != os::InvalidNativeHandle) { DonorAcceptNv(g_nv_tmem_size, g_nv_tmem_handle); g_nv_tmem_handle = os::InvalidNativeHandle; }
        VicStage("vb:released");
        if (g_donor_mode) {
            DonorRelease();
            g_donor_mode = false;
            g_vic_heap = 0;
            g_vic_heap_size = 0;
            g_vic_cfg_buf = g_vic_cmd_buf = g_vic_dst_buf = g_vic_src_buf = nullptr;
            g_ind_buf = nullptr;
            SetCaptureTail(nullptr);
            g_ind_size = 0;
            g_stage_buf = nullptr;
            g_stream_stage[0] = g_stream_stage[1] = nullptr;
            g_ctrl_fd = 0;
            g_guard_nvmap_fd = 0;
            g_guard_next = 0x9000;
            g_blit_done.store(false);
            g_rearm.store(true);
            LogLine("   engines closed, the donor's memory given back - set up again when a viewer connects");
            static u32 retry_s = 5;
            if (g_setup_failed) {
                LogLine("   engines: the setup failed - trying again in %u s", retry_s);
                os::SleepThread(TimeSpan::FromSeconds(retry_s));
                retry_s = retry_s >= 30 ? 60 : retry_s * 2;
            } else {
                retry_s = 5;
            }
        }
        g_setup_failed = false;
    }

        void TryIndirectCapture() {
            VicStage("ind:1_open_vi_root");
            ::Service vi_root = {};
            ::Service disp    = {};
            bool got_disp = false;
            for (const auto &v : { std::tuple<const char *, u32, u32>{ "vi:m", 2, 1 },
                                   std::tuple<const char *, u32, u32>{ "vi:s", 1, 1 } }) {
                const char *name = std::get<0>(v);
                const u32 cmd = std::get<1>(v), mode = std::get<2>(v);

                SmUse sm_vi;
                ::Result r = smGetService(std::addressof(vi_root), name);
                if (R_FAILED(r)) { LogLine("   smGetService(%s) rc=0x%x", name, r); continue; }

                r = serviceDispatchIn(std::addressof(vi_root), cmd, mode,
                    .out_num_objects = 1, .out_objects = std::addressof(disp));
                LogLine("   %s GetDisplayService cmd=%u mode=%u rc=0x%x", name, cmd, mode, r);
                if (R_SUCCEEDED(r)) { got_disp = true; break; }
                serviceClose(std::addressof(vi_root));
            }
            if (!got_disp) { VicStage("ind:2_FAILED"); return; }
            ::Result rc = 0;
            AMS_UNUSED(rc);

            ::Service mgr_keep = {};
            for (const auto &p : { std::pair<u32, const char *>{ 101, "GetSystemDisplayService" },
                                   std::pair<u32, const char *>{ 102, "GetManagerDisplayService" } }) {
                ::Service sub = {};
                const ::Result r = serviceDispatch(std::addressof(disp), p.first,
                    .out_num_objects = 1, .out_objects = std::addressof(sub));
                LogLine("   %u %-26s rc=0x%x %s", p.first, p.second, r,
                        R_SUCCEEDED(r) ? "GOT IT" : "denied");
                if (R_SUCCEEDED(r)) { serviceClose(std::addressof(sub)); }
            }

            VicStage("ind:3_required_memory");
            for (const auto &d : { std::pair<s32, s32>{ 1280, 720 }, std::pair<s32, s32>{ 1920, 1080 } }) {
                const struct { s64 w; s64 h; } in = { d.first, d.second };
                struct { s64 size; s64 align; } out = {};
                const ::Result r = serviceDispatchInOut(std::addressof(disp), 2460, in, out);
                LogLine("   2460(%dx%d) rc=0x%x -> size=%lld align=%lld",
                        d.first, d.second, r,
                        static_cast<long long>(out.size), static_cast<long long>(out.align));
            }

            u64 display_id = 0;
            {
                struct { char data[0x40]; } name = {};
                std::strncpy(name.data, "Default", sizeof(name.data) - 1);
                const ::Result r = serviceDispatchInOut(std::addressof(disp), 1010, name, display_id);
                LogLine("   1010 OpenDisplay(\"Default\") rc=0x%x -> display_id=%llu",
                        r, static_cast<unsigned long long>(display_id));
            }

            u64 consumer = 0;
            if (R_SUCCEEDED(serviceDispatch(std::addressof(disp), 102,
                                            .out_num_objects = 1, .out_objects = std::addressof(mgr_keep)))) {
                VicStage("ind:3b_create_indirect_layer");
                {
                    const struct { u64 display_id; u64 aruid; } in = { display_id, g_game_aruid };
                    u64 out = 0;
                    const ::Result r = serviceDispatchInOut(std::addressof(mgr_keep), 2050, in, out);
                    LogLine("   2050 shapeA{disp,aruid} rc=0x%x -> handle=%llu", r,
                            static_cast<unsigned long long>(out));
                    if (R_SUCCEEDED(r)) { consumer = out; }
                }
                if (consumer == 0) {
                    const struct { u32 flags; u32 pad; u64 display_id; u64 aruid; } in =
                        { 0, 0, display_id, g_game_aruid };
                    u64 out = 0;
                    const ::Result r = serviceDispatchInOut(std::addressof(mgr_keep), 2050, in, out);
                    LogLine("   2050 shapeB{flags,disp,aruid} rc=0x%x -> handle=%llu", r,
                            static_cast<unsigned long long>(out));
                    if (R_SUCCEEDED(r)) { consumer = out; }
                }
                if (consumer == 0) {
                    const u64 in = g_game_aruid;
                    u64 out = 0;
                    const ::Result r = serviceDispatchInOut(std::addressof(mgr_keep), 2050, in, out);
                    LogLine("   2050 shapeC{aruid} rc=0x%x -> handle=%llu", r,
                            static_cast<unsigned long long>(out));
                    if (R_SUCCEEDED(r)) { consumer = out; }
                }
                LogLine("   consumer handle = %llu", static_cast<unsigned long long>(consumer));

                if (consumer != 0) {
                    VicStage("ind:3c_endpoints");
                    for (const auto &e : { std::pair<u32, const char *>{ 2052, "CreateIndirectProducerEndPoint" },
                                           std::pair<u32, const char *>{ 2054, "CreateIndirectConsumerEndPoint" } }) {
                        {
                            const struct { u64 h; u64 aruid; } in = { consumer, g_game_aruid };
                            u64 out = 0;
                            const ::Result r = serviceDispatchInOut(std::addressof(mgr_keep), e.first, in, out);
                            LogLine("   %u %-32s {h,aruid} rc=0x%x -> %llu", e.first, e.second, r,
                                    static_cast<unsigned long long>(out));
                            if (R_SUCCEEDED(r)) { continue; }
                        }
                        {
                            const u64 in = consumer;
                            u64 out = 0;
                            const ::Result r = serviceDispatchInOut(std::addressof(mgr_keep), e.first, in, out);
                            LogLine("   %u %-32s {h}       rc=0x%x -> %llu", e.first, e.second, r,
                                    static_cast<unsigned long long>(out));
                        }
                    }
                }
            }

            VicStage("ind:4_image_map");
            s32 cw = 0, ch = 0;
            for (const auto &d : { std::pair<s32, s32>{ 1920, 1080 }, std::pair<s32, s32>{ 1280, 720 },
                                   std::pair<s32, s32>{ 960, 540 },  std::pair<s32, s32>{ 640, 360 } }) {
                const struct { s64 w; s64 h; } q = { d.first, d.second };
                struct { s64 size; s64 align; } a = {};
                if (R_SUCCEEDED(serviceDispatchInOut(std::addressof(disp), 2460, q, a)) &&
                    a.size > 0 && static_cast<size_t>(a.size) <= g_ind_size) {
                    cw = d.first; ch = d.second;
                    LogLine("   capture size %dx%d needs %lld B, buffer is %zu B - OK",
                            cw, ch, static_cast<long long>(a.size), g_ind_size);
                    break;
                }
            }
            if (cw == 0) { LogLine("   no resolution fits the %zu B buffer", g_ind_size); }

            std::memset(g_ind_buf, 0xAB, 0x1000);
            armDCacheFlush(g_ind_buf, g_ind_size);
            const u64 handles[] = { consumer, 0, 1, 2, g_game_aruid };
            for (u64 h : handles) {
                if (cw == 0) { break; }
                const struct { s64 w; s64 h; u64 handle; u64 aruid; } in =
                    { cw, ch, h, g_game_aruid };
                struct { s64 size; s64 stride; } out = {};
                const ::Result r = serviceDispatchInOut(std::addressof(disp), 2450, in, out,
                    .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias |
                                      SfBufferAttr_HipcMapTransferAllowsNonSecure },
                    .buffers      = { { g_ind_buf, g_ind_size } },
                    .in_send_pid  = true);
                LogLine("   2450(%dx%d, handle=%llu, aruid=%llu) rc=0x%x -> size=%lld stride=%lld", cw, ch,
                        static_cast<unsigned long long>(h),
                        static_cast<unsigned long long>(g_game_aruid), r,
                        static_cast<long long>(out.size), static_cast<long long>(out.stride));
                if (R_SUCCEEDED(r)) {
                    armDCacheFlush(g_ind_buf, g_ind_size);
                    u32 changed = 0;
                    for (u32 i = 0; i < 0x1000; i++) { if (g_ind_buf[i] != 0xAB) { changed++; } }
                    LogLine("   *** 2450 SUCCEEDED *** changed=%u/4096  first16: "
                            "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                            changed, g_ind_buf[0], g_ind_buf[1], g_ind_buf[2], g_ind_buf[3],
                            g_ind_buf[4], g_ind_buf[5], g_ind_buf[6], g_ind_buf[7],
                            g_ind_buf[8], g_ind_buf[9], g_ind_buf[10], g_ind_buf[11],
                            g_ind_buf[12], g_ind_buf[13], g_ind_buf[14], g_ind_buf[15]);
                    VicStage("ind:CAPTURED");
                    break;
                }
            }

            if (serviceIsActive(std::addressof(mgr_keep))) { serviceClose(std::addressof(mgr_keep)); }
            serviceClose(std::addressof(disp));
            serviceClose(std::addressof(vi_root));
            VicStage("ind:done");
        }

        struct RegionHit { u64 base; u64 size; u32 state; u32 perm; u32 attr; u32 devices; };

        constexpr u64 FbSlotSize   = 8847360ull;
        constexpr u64 FbSwapSize   = 3ull * FbSlotSize;
        constexpr u64 FbSlotOff[3] = { 0ull, 0x870000ull, 0x10E0000ull };

        struct FbCandidate { u64 addr; u64 region_base; u32 lane; u32 varied; u8 head[3][16]; };

        constexpr u64 FbBlockRow = 983040ull;

        s32 FbPixelLane(const u8 *p, u32 *out_varied) {
            for (u32 lane = 0; lane < 4; ++lane) {
                bool opaque = true;
                for (u32 i = 0; i < 16; ++i) {
                    if (p[i * 4 + lane] != 0xFF) { opaque = false; break; }
                }
                if (!opaque) { continue; }

                u32 varied = 0;
                for (u32 o = 0; o < 4; ++o) {
                    if (o == lane) { continue; }
                    for (u32 i = 1; i < 16; ++i) {
                        if (p[i * 4 + o] != p[o]) { ++varied; break; }
                    }
                }
                if (varied == 0) { continue; }
                if (out_varied != nullptr) { *out_varied = varied; }
                return static_cast<s32>(lane);
            }
            return -1;
        }

        constexpr const char *FrameBinPath = "sdmc:/applet-mitm-frame.bin";
        constexpr const char *FrameTxtPath = "sdmc:/applet-mitm-frame.txt";

        bool WriteBufToSd(const char *path, const void *buf, size_t len) {
            fs::DeleteFile(path);
            if (R_FAILED(fs::CreateFile(path, static_cast<s64>(len)))) { return false; }
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), path, fs::OpenMode_Write))) { return false; }
            const auto wrc = fs::WriteFile(f, 0, buf, len, fs::WriteOption::Flush);
            fs::CloseFile(f);
            if (R_FAILED(wrc)) { LogLine("   WriteBufToSd(%s) rc=0x%x", path, wrc.GetValue()); }
            return R_SUCCEEDED(wrc);
        }

        constexpr u32 Fnv1a32(const u8 *p, size_t n) {
            u32 h = 0x811C9DC5u;
            for (size_t i = 0; i < n; ++i) { h = (h ^ p[i]) * 0x01000193u; }
            return h;
        }

        bool WriteSdVerified(const char *path, const u8 *src, size_t len, u8 *verify, u32 *out_fnv) {
            const u32 fnv = Fnv1a32(src, len);
            if (out_fnv != nullptr) { *out_fnv = fnv; }
            for (u32 attempt = 1; attempt <= 2; ++attempt) {
                static_cast<void>(fs::DeleteFile(path));
                Result rc = fs::CreateFile(path, static_cast<s64>(len));
                if (R_FAILED(rc)) { LogLine("   sd(%s) #%u CreateFile rc=0x%x", path, attempt, rc.GetValue()); continue; }
                fs::FileHandle f;
                rc = fs::OpenFile(std::addressof(f), path, fs::OpenMode_Write);
                if (R_FAILED(rc)) { LogLine("   sd(%s) #%u OpenFile(write) rc=0x%x", path, attempt, rc.GetValue()); continue; }
                rc = fs::WriteFile(f, 0, src, len, fs::WriteOption::Flush);
                fs::CloseFile(f);
                if (R_FAILED(rc)) { LogLine("   sd(%s) #%u WriteFile(%zu B) rc=0x%x", path, attempt, len, rc.GetValue()); continue; }

                rc = fs::OpenFile(std::addressof(f), path, fs::OpenMode_Read);
                if (R_FAILED(rc)) { LogLine("   sd(%s) #%u OpenFile(read-back) rc=0x%x", path, attempt, rc.GetValue()); continue; }
                size_t got = 0;
                rc = fs::ReadFile(std::addressof(got), f, 0, verify, len);
                fs::CloseFile(f);
                if (R_FAILED(rc) || got != len) {
                    LogLine("   sd(%s) #%u read-back rc=0x%x got %zu of %zu B", path, attempt, rc.GetValue(), got, len);
                    continue;
                }
                size_t bad = 0;
                while (bad < len && verify[bad] == src[bad]) { ++bad; }
                if (bad != len) {
                    LogLine("   sd(%s) #%u read-back DIFFERS at byte %zu (wrote %02x, read %02x)", path, attempt, bad, src[bad], verify[bad]);
                    continue;
                }
                LogLine("   sd(%s): %zu B written, read back identical, fnv1a32=%08x", path, len, fnv);
                return true;
            }
            LogLine("   sd(%s): FAILED after 2 attempts - the file on the card is NOT this data (fnv1a32 would be %08x)", path, fnv);
            return false;
        }

        bool WriteEngineOutputToSd(const char *path, const u8 *uncached_src, size_t len, u32 *out_fnv) {
            const size_t slot = (len + 0xFFF) & ~static_cast<size_t>(0xFFF);
            if (g_stage_buf == nullptr || 2 * slot > StreamStageSize || g_ind_size < FbBlockRowStage + 2 * slot) {
                LogLine("   sd(%s): no cached stage buffer for %zu B - not writing", path, len);
                return false;
            }
            std::memcpy(g_stage_buf, uncached_src, len);
            return WriteSdVerified(path, g_stage_buf, len, g_stage_buf + slot, out_fnv);
        }

        constexpr const char *StripBinPath = "sdmc:/applet-mitm-strip.bin";
        constexpr const char *VicBinPath   = "sdmc:/applet-mitm-vic.bin";

        bool DumpSlotToSd(::ams::svc::Handle dbg, u64 src, u64 *out_ns, u32 *out_chunks) {
            fs::DeleteFile(FrameBinPath);
            if (R_FAILED(fs::CreateFile(FrameBinPath, static_cast<s64>(FbSlotSize)))) { return false; }
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), FrameBinPath, fs::OpenMode_Write))) { return false; }

            const u64 t0 = armTicksToNs(armGetSystemTick());
            u32 chunks = 0;
            bool ok = true;
            for (u64 done = 0; done < FbSlotSize; done += 0x10000) {
                const u64 n = (FbSlotSize - done < 0x10000) ? (FbSlotSize - done) : 0x10000;
                if (!DbgRead(dbg, g_ind_buf, src + done, n)) { ok = false; break; }
                const auto opt = (done + n >= FbSlotSize) ? fs::WriteOption::Flush : fs::WriteOption::None;
                if (R_FAILED(fs::WriteFile(f, static_cast<s64>(done), g_ind_buf, n, opt))) { ok = false; break; }
                ++chunks;
            }
            *out_ns     = armTicksToNs(armGetSystemTick()) - t0;
            *out_chunks = chunks;
            fs::CloseFile(f);
            return ok;
        }

        struct __attribute__((packed)) SftHdrWire {
            u32 magic; u16 version; u16 flags;
            u32 width, height, stride, length, block_h_log2, kind;
        };
        static_assert(sizeof(SftHdrWire) == 32, "must match sft_hdr_t in tools/raw-recv/raw-recv.c");

        inline u32 BlockLinearOffset(u32 byte_x, u32 y) {
            const u32 block_y  = y / 128;
            const u32 y_in_blk = y % 128;
            const u32 gob_y    = y_in_blk / 8;
            const u32 yy       = y_in_blk % 8;
            const u32 block_x  = byte_x / 64;
            const u32 xg       = byte_x % 64;
            const u32 base     = (block_y * 120u + block_x) * 8192u;
            const u32 in_gob   = ((xg / 32u) * 256u) + ((yy / 2u) * 64u)
                               + (((xg % 32u) / 16u) * 32u) + ((yy % 2u) * 16u);
            return base + gob_y * 512u + in_gob;
        }

        [[maybe_unused]] void DownscalePoint(const u8 *src, u8 *dst, u32 out_w, u32 out_h, u32 step) {
            for (u32 oy = 0; oy < out_h; ++oy) {
                const u32 sy = oy * step;
                u8 *drow = dst + static_cast<size_t>(oy) * out_w * 4;
                for (u32 ox = 0; ox < out_w; ++ox) {
                    std::memcpy(drow + ox * 4, src + BlockLinearOffset(ox * step * 4u, sy), 4);
                }
            }
        }

        void StreamFrames(::ams::svc::Handle dbg, u64 slot_base,
                          u32 vfd, u32 cmd_handle, u32 syncpt,
                          u32 cfg_addr, u32 dst_addr, u32 src_pin,
                          u32 wantW = 0, u32 wantH = 0, u32 wantFrames = 0) {
            const u32 W = (wantW != 0) ? wantW : g_stream_w;
            const u32 H = (wantH != 0) ? wantH : g_stream_h;
            const u32 nframes = (wantFrames != 0) ? wantFrames : g_stream_frames;
            const size_t out_bytes  = static_cast<size_t>(W) * static_cast<size_t>(H) * 3 / 2;
            const size_t wire_bytes = sizeof(SftHdrWire) + out_bytes;

            if (out_bytes > StreamStageSize) {
                LogLine("   stream: %ux%u needs %zu B but a stage buffer is %zu - refusing",
                        W, H, out_bytes, StreamStageSize);
                return;
            }
            if (g_stream_stage[0] == nullptr || g_stream_stage[1] == nullptr) {
                LogLine("   stream: stage buffers absent - heap too small"); return;
            }
            if (W == 0 || H == 0 || (W & 1) != 0 || (H & 1) != 0) {
                LogLine("   stream: %ux%u must have even dimensions (4:2:0 chroma)", W, H);
                return;
            }
            if (src_pin == 0 || cfg_addr == 0 || dst_addr == 0) {
                LogLine("   stream: engine resources missing (pin=%x cfg=%x dst=%x)", src_pin, cfg_addr, dst_addr);
                return;
            }
            if (!UsbReady()) {
                LogLine("   stream: USB not Configured - nothing to stream to"); return;
            }

            g_vic_quiet = true;
            LogLine("   ---- STREAMING %ux%u packed-420 via VIC, up to %u frames (%zu B/frame) ----",
                    W, H, nframes, wire_bytes);

            SftHdrWire h = {};
            h.magic  = 0x52544653u;
            h.version = 1;
            h.width  = W;  h.height = H;  h.stride = W * 4;
            h.length = static_cast<u32>(out_bytes);
            h.block_h_log2 = 0;
            h.kind         = 0;
            h.flags        = 1;
            h.stride       = W;
            std::memcpy(g_usb_hdr, std::addressof(h), sizeof(h));

            const SrcDesc stream_src{ 1920, 1080, 1920,
                                      vic::BLK_KIND_GENERIC_16Bx2, 4,
                                      vic::PIXFMT_A8R8G8B8, vic::CACHE_WIDTH_64Bx4,
                                      1920, 1080 };

            u32 seen   = g_queue_count.load(std::memory_order_relaxed);
            const u32 q_at_start = seen;
            u32 parity = 0, urb = 0, sent = 0, dropped = 0;
            u32 stale_run = 0, stale_total = 0;
            bool pending = false;
            u64 t_read = 0, t_vic = 0, t_copy = 0, t_wait = 0;
            u64 worst = 0, best = ~UINT64_C(0);
            u32 timed = 0;
            const u64 loop_t0 = armTicksToNs(armGetSystemTick());

            for (u32 i = 0; i < nframes; ++i) {
                u32 spins = 0;
                while (g_queue_count.load(std::memory_order_relaxed) == seen && spins < 20) {
                    os::SleepThread(TimeSpan::FromMilliSeconds(1));
                    ++spins;
                }
                const u32 now = g_queue_count.load(std::memory_order_relaxed);
                if (now != seen) {
                    dropped += (now - seen) - 1;
                    stale_run = 0;
                } else {
                    ++stale_run; ++stale_total;
                    if (stale_run >= 1200) {
                        LogLine("   stream: no new frame for %u iterations - stopping at frame %u", stale_run, i);
                        break;
                    }
                }
                seen = now;

                const s32 slot = g_queue_slot.load(std::memory_order_relaxed);
                const u64 off  = (slot >= 0 && slot < 3) ? FbSlotOff[slot] : 0;

                const u64 f0 = armTicksToNs(armGetSystemTick());
                if (!DbgRead(dbg, g_ind_buf, slot_base + off, FbSlotSize)) {
                    LogLine("   stream: slot read failed at frame %u", i); break;
                }
                const u64 f1 = armTicksToNs(armGetSystemTick());

                armDCacheFlush(g_ind_buf, FbSlotSize);

                g_strip_src = stream_src;
                g_strip_out = OutDesc{ W, H, W, vic::PIXFMT_Y8_U8V8_N420 };
                const JobCtx vc{ vfd, cmd_handle, syncpt, cfg_addr, dst_addr, src_pin, 0, 0, 0, 0, SelfSrc };
                if (!RunOneJob("st:vic", VicJob::BlitStrip, true, vc)) {
                    LogLine("   stream: VIC job failed at frame %u", i); break;
                }
                const u64 f2 = armTicksToNs(armGetSystemTick());

                std::memcpy(g_stream_stage[parity], g_vic_dst_buf, out_bytes);
                const u64 f3 = armTicksToNs(armGetSystemTick());

                if (pending) {
                    size_t got = 0;
                    if (!UsbWaitAsync(urb, std::addressof(got))) {
                        LogLine("   stream: transfer stalled at frame %u", i); pending = false; break;
                    }
                    ++sent;
                }
                const u64 f4 = armTicksToNs(armGetSystemTick());

                size_t hsent = 0;
                if (!UsbSendBuffer(g_usb_hdr, sizeof(SftHdrWire), std::addressof(hsent))) {
                    LogLine("   stream: header send failed at frame %u", i); pending = false; break;
                }
                if (!UsbPostAsync(g_stream_stage[parity], out_bytes, std::addressof(urb))) {
                    LogLine("   stream: post failed at frame %u", i); pending = false; break;
                }
                pending = true;
                parity ^= 1;

                os::SleepThread(TimeSpan::FromMilliSeconds(2));

                t_read += f1 - f0; t_vic += f2 - f1; t_copy += f3 - f2; t_wait += f4 - f3;
                const u64 dt = f4 - f0;
                if (dt > worst) { worst = dt; }
                if (dt < best)  { best  = dt; }
                ++timed;
            }

            if (pending) {
                size_t got = 0;
                if (UsbWaitAsync(urb, std::addressof(got))) { ++sent; }
            }
            g_vic_quiet = false;

            const u64 loop_ns = armTicksToNs(armGetSystemTick()) - loop_t0;
            const u32 n = (timed > 0) ? timed : 1;
            const u64 fps_x10 = (loop_ns > 0) ? (UINT64_C(10000000000) * sent / loop_ns) : 0;
            LogLine("   stream: %u frames sent in %llu ms -> %llu.%llu fps  (%u presented frames dropped)",
                    sent, static_cast<unsigned long long>(loop_ns / 1000000),
                    static_cast<unsigned long long>(fps_x10 / 10),
                    static_cast<unsigned long long>(fps_x10 % 10), dropped);
            LogLine("   stream: avg per frame - read %llu us, vic %llu us, copy %llu us, usb wait %llu us",
                    static_cast<unsigned long long>(t_read / 1000 / n),
                    static_cast<unsigned long long>(t_vic  / 1000 / n),
                    static_cast<unsigned long long>(t_copy / 1000 / n),
                    static_cast<unsigned long long>(t_wait / 1000 / n));
            LogLine("   stream: queue counter %u -> %u over the run; %u stale iterations",
                    q_at_start, g_queue_count.load(std::memory_order_relaxed), stale_total);
            LogLine("   stream: frame time best %llu us, worst %llu us  (60 fps budget = 16667 us)",
                    static_cast<unsigned long long>((best == ~UINT64_C(0)) ? 0 : best / 1000),
                    static_cast<unsigned long long>(worst / 1000));
        }

        bool FindSwapchainBySize(::ams::svc::Handle dbg, u64 want, u64 *out) {
            u64 addr = 0, best = 0, best_size = ~UINT64_C(0);
            u64 exact = 0;
            u32 exact_devc = 0, exact_n = 0;
            u64 run_base = 0, run_size = 0, run_exact = 0, run_best = 0, run_best_size = ~UINT64_C(0);
            u32 run_parts = 0, exact_parts = 0;
            auto end_run = [&]() {
                if (run_parts > 1 && run_size == want && run_exact == 0) { run_exact = run_base; exact_parts = run_parts; }
                if (run_parts > 1 && run_size > want && run_size < run_best_size) { run_best = run_base; run_best_size = run_size; }
                run_size = 0; run_parts = 0;
            };
            for (u32 steps = 0; steps < 4000; ++steps) {
                ::ams::svc::MemoryInfo mi = {};
                ::ams::svc::PageInfo   pi = {};
                if (!DbgQuery(dbg, std::addressof(mi), std::addressof(pi), addr) || mi.size == 0) { break; }
                const u32  attr = static_cast<u32>(mi.attribute);
                const bool dev  = (attr & ::ams::svc::MemoryAttribute_DeviceShared) != 0 || mi.device_count > 0;
                if (dev && mi.size == want) {
                    ++exact_n;
                    if (exact == 0 || mi.device_count > exact_devc) {
                        exact = mi.base_address;
                        exact_devc = mi.device_count;
                    }
                }
                if (dev && mi.size > want && mi.size < best_size) { best = mi.base_address; best_size = mi.size; }
                if (dev) {
                    if (run_parts == 0 || run_base + run_size != mi.base_address) { end_run(); run_base = mi.base_address; }
                    run_size += mi.size; ++run_parts;
                } else {
                    end_run();
                }
                const u64 next = mi.base_address + mi.size;
                if (next <= addr) { break; }
                addr = next;
            }
            end_run();
            if (exact != 0) {
                if (exact_n > 1) {
                    LogLine("   swapchain: %u regions of exactly %llu B - using 0x%010llx (device mappings %u)",
                            exact_n, static_cast<unsigned long long>(want),
                            static_cast<unsigned long long>(exact), exact_devc);
                }
                *out = exact;
                return true;
            }
            if (run_exact != 0) {
                LogLine("   swapchain: %llu B in %u adjacent device-mapped regions at 0x%010llx", static_cast<unsigned long long>(want), exact_parts,
                        static_cast<unsigned long long>(run_exact));
                *out = run_exact;
                return true;
            }
            if (best_size != ~UINT64_C(0)) {
                LogLine("   swapchain: no region of exactly %llu B; using the smallest device-mapped one that holds it (%llu B)",
                        static_cast<unsigned long long>(want), static_cast<unsigned long long>(best_size));
                *out = best;
                return true;
            }
            if (run_best_size != ~UINT64_C(0)) {
                LogLine("   swapchain: no region holds %llu B; using the smallest run of adjacent device-mapped ones that does (%llu B at 0x%010llx)",
                        static_cast<unsigned long long>(want), static_cast<unsigned long long>(run_best_size), static_cast<unsigned long long>(run_best));
                *out = run_best;
                return true;
            }
            return false;
        }

        u32 FindSlotCandidates(::ams::svc::Handle dbg, u64 buf, const u32 *slot_off, u32 nslots, bool separate, u64 *out, u32 max, u32 *devc = nullptr) {
            if (buf == 0 || nslots == 0) { return 0; }
            static constexpr u64 Aligns[] = { 0x1000, 0x10000, 0x20000, 0x40000, 0x100000, 0x200000, 0x400000 };
            u64 span = buf;
            for (u32 k = 0; k < nslots; ++k) { if (slot_off[k] + buf > span) { span = slot_off[k] + buf; } }
            u64 addr = 0;
            u32 n = 0;
            for (u32 steps = 0; steps < 4000 && n < max; ++steps) {
                ::ams::svc::MemoryInfo mi = {};
                ::ams::svc::PageInfo   pi = {};
                if (!DbgQuery(dbg, std::addressof(mi), std::addressof(pi), addr) || mi.size == 0) { break; }
                const u32  attr = static_cast<u32>(mi.attribute);
                const bool dev  = (attr & ::ams::svc::MemoryAttribute_DeviceShared) != 0 || mi.device_count > 0;
                if (dev && separate) {
                    for (const u64 a : Aligns) {
                        const u64 step = (buf + a - 1) & ~(a - 1);
                        if (mi.size % step == 0 && mi.size / step <= 8) {
                            for (u64 o = 0; o < mi.size && n < max; o += step) { if (devc) { devc[n] = mi.device_count; } out[n++] = mi.base_address + o; }
                            break;
                        }
                    }
                } else if (dev && mi.size >= span) {
                    LogLine("     region 0x%010llx, %llu MB: state 0x%x attr 0x%x perm 0x%x, device mappings %u, ipc %u",
                            static_cast<unsigned long long>(mi.base_address), static_cast<unsigned long long>(mi.size >> 20),
                            static_cast<u32>(mi.state), attr, static_cast<u32>(mi.permission), mi.device_count, mi.ipc_count);
                    for (u32 k = 0; k < nslots && n < max; ++k) { if (devc) { devc[n] = mi.device_count; } out[n++] = mi.base_address + slot_off[k]; }
                }
                const u64 next = mi.base_address + mi.size;
                if (next <= addr) { break; }
                addr = next;
            }
            return n;
        }

        bool LocateSwapchain(::ams::svc::Handle dbg, GameSurface &geo, u64 *base, const char *who) {
            AMS_UNUSED(who);
            u64 want = FbSwapSize;
            if (geo.num_slots != 0 && geo.buf_size != 0) {
                u32 top = 0;
                for (u32 k = 0; k < geo.num_slots && k < 8; ++k) { if (geo.slot_offset[k] > top) { top = geo.slot_offset[k]; } }
                want = static_cast<u64>(top) + geo.buf_size;
            }
            static u64 s_base = 0, s_want = 0;
            static u32 s_gen = 0;
            if (FindSwapchainBySize(dbg, want, base)) { s_base = *base; s_want = want; s_gen = geo.generation; return true; }
            if (s_base != 0 && s_want == want && s_gen == geo.generation) {
                ::ams::svc::MemoryInfo mi = {};
                ::ams::svc::PageInfo   pi = {};
                if (DbgQuery(dbg, std::addressof(mi), std::addressof(pi), s_base) &&
                    ((static_cast<u32>(mi.attribute) & ::ams::svc::MemoryAttribute_DeviceShared) != 0 || mi.device_count > 0)) {
                    LogLine("   swapchain: not found by size - using the last session's address 0x%010llx (the same buffers)", static_cast<unsigned long long>(s_base));
                    *base = s_base;
                    return true;
                }
            }
            return false;
        }

        bool DiscoverAlbumSurface(::ams::svc::Handle dbg, GameSurface *out) {
            static constexpr u64 Wants[] = {
                7864320ull,
                2ull * 8847360ull,
                3ull * 8847360ull,
                3ull * 3932160ull,
            };
            u64 base = 0, want = 0;
            for (const u64 w : Wants) {
                if (FindSwapchainBySize(dbg, w, std::addressof(base))) { want = w; break; }
            }
            if (want == 0) { return false; }
            GameSurface g = {};
            if (want == 7864320ull || want == 3ull * 3932160ull) {
                g.width = 1280; g.height = 720; g.stride_px = 1280;
                g.buf_size = 3932160u;
                g.num_slots = (want == 7864320ull) ? 2u : 3u;
            } else {
                g.width = 1920; g.height = 1080; g.stride_px = 1920;
                g.buf_size = 8847360u;
                g.num_slots = static_cast<u32>(want / 8847360ull);
            }
            g.block_h_log2 = 4;
            g.color_format = 0x100532120ull;
            g.layout = 3;
            g.pix_format = vic::PIXFMT_A8B8G8R8;
            for (u32 k = 0; k < g.num_slots && k < 8; ++k) {
                g.slot_offset[k] = k * g.buf_size;
                g.slot_nvmap[k] = 1;
            }
            g.generation = g_applet_surface.generation + 1;
            g.armed = true;
            *out = g;
            LogLine("   live-ui: discovered Album/SwitchU swapchain %llu B (%ux%u, %u slot(s)) at 0x%010llx",
                    static_cast<unsigned long long>(want), g.width, g.height, g.num_slots,
                    static_cast<unsigned long long>(base));
            return true;
        }

        bool SeedRunningAlbumSurface() {
            static bool s_tried = false;
            static bool s_ok = false;
            if (s_ok && g_applet_surface.armed && g_applet_target_pid.load(std::memory_order_relaxed) != 0) { return true; }
            if (s_tried && !s_ok) { return false; }
            os::ProcessId ap{};
            if (R_FAILED(::ams::pm::dmnt::GetProcessId(std::addressof(ap), ncm::ProgramId{UINT64_C(0x010000000000100D)})) || ap.value == 0) {
                return false;
            }
            if (g_applet_surface.armed) {
                g_applet_target_pid.store(ap.value, std::memory_order_relaxed);
                s_ok = true;
                return true;
            }
            s_tried = true;
            ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
            if (!DmAttach(ap.value, std::addressof(dbg))) {
                LogLine("   live-ui: 100D pid %llu up but attach failed - cannot seed Album surface",
                        static_cast<unsigned long long>(ap.value));
                return false;
            }
            u32 nev = 0;
            static_cast<void>(DbgResume(dbg, std::addressof(nev)));
            GameSurface geo = {};
            const bool found = DiscoverAlbumSurface(dbg, std::addressof(geo));
            DbgDetach(dbg);
            if (!found) {
                LogLine("   live-ui: 100D pid %llu up but no Album/SwitchU swapchain found yet",
                        static_cast<unsigned long long>(ap.value));
                s_tried = false;
                return false;
            }
            g_applet_surface = geo;
            g_applet_target_pid.store(ap.value, std::memory_order_relaxed);
            g_present_pid.store(ap.value, std::memory_order_relaxed);
            g_album_seed_no_binder.store(true, std::memory_order_relaxed);
            s_ok = true;
            LogLine("   live-ui: seeded Album/SwitchU from already-running 100D (pid %llu) - boot capture without binder history (content-sync slots)",
                    static_cast<unsigned long long>(ap.value));
            return true;
        }

        bool RunUiSession(u32 vfd, u32 nvmap_fd, u32 cmd_handle, u32 syncpt, u32 cfg_addr, u32 &sessions) {
            if (WebAppletOpen()) {
                static bool said = false;
                if (!said) { LogLine("   live-ui: a web applet is open - the HOME menu is not streamed meanwhile"); said = true; }
                return false;
            }
            u64 vpid = 0, base = 0;
            GameSurface ug = {};
            if (!UiSourceFind(std::addressof(vpid), std::addressof(base), std::addressof(ug))) {
                const bool swu = SwitchUOpen();
                if (swu && !g_applet_surface.armed) {
                    static bool said_wait = false;
                    if (!said_wait) {
                        LogLine("   live-ui: waiting up to 3.5s for 100D binders (SwitchU remount)");
                        said_wait = true;
                    }
                    constexpr const char NeedRemount[] = "sdmc:/config/nxstream/tap/need_capture_remount";
                    static_cast<void>(fs::CreateDirectory("sdmc:/config/nxstream"));
                    static_cast<void>(fs::CreateDirectory("sdmc:/config/nxstream/tap"));
                    static_cast<void>(fs::DeleteFile(NeedRemount));
                    static_cast<void>(fs::CreateFile(NeedRemount, 0));
                    for (u32 w = 0; w < 70 && !g_applet_surface.armed; ++w) {
                        os::SleepThread(TimeSpan::FromMilliSeconds(50));
                    }
                    static_cast<void>(fs::DeleteFile(NeedRemount));
                }
                if (swu && g_applet_surface.armed) {
                    static bool said_binder = false;
                    if (!said_binder) {
                        LogLine("   live-ui: vi shared buffer absent (SwitchU/Album) - using binder-taught 100D surface");
                        said_binder = true;
                    }
                    return false;
                }
                if (swu && SeedRunningAlbumSurface()) {
                    static bool said_seed = false;
                    if (!said_seed) {
                        LogLine("   live-ui: vi shared buffer absent (SwitchU/Album) - using seeded 100D surface");
                        said_seed = true;
                    }
                    return false;
                }
                static bool said = false;
                if (!said) { LogLine("   live-ui: vi's shared buffer not found - the HOME menu is not streamed"); said = true; }
                return false;
            }
            if (!CaptureAcquire("live-ui")) {
                VicStage("live-ui:no_memory");
                SetViewerReason(ViewerReason_NoMemory);
                os::SleepThread(TimeSpan::FromSeconds(2));
                return false;
            }
            SetViewerReason(ViewerReason_Starting);
            ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
            const u64 f0 = armTicksToNs(armGetSystemTick());
            if (const Result ra = ::ams::svc::DebugActiveProcess(std::addressof(dbg), vpid); R_FAILED(ra)) {
                LogLine("   live-ui: attach to vi rc=0x%x", ra.GetValue());
                os::SleepThread(TimeSpan::FromSeconds(5));
                return false;
            }
            u32 nev = 0;
            const Result rr = DbgResume(dbg, std::addressof(nev));
            const u64 held_us = (armTicksToNs(armGetSystemTick()) - f0) / 1000;
            if (R_FAILED(rr)) { DbgDetach(dbg); os::SleepThread(TimeSpan::FromSeconds(2)); return false; }
            DebugPumpStart(dbg);
            ++sessions;
            g_live_sessions.store(sessions, std::memory_order_relaxed);
            LogLine("   live[%u]: the HOME menu / applets - attached to vi (pid %llu), held %llu us, %u attach events", sessions,
                    static_cast<unsigned long long>(vpid), static_cast<unsigned long long>(held_us), nev);
            g_game_presented_in_ui.store(false, std::memory_order_relaxed);
            g_session_surface = std::addressof(g_game_surface);
            g_ui_session_active.store(true, std::memory_order_relaxed);
            StreamEnd end = StreamEnd::Failed;
            const bool ui_started = UiSourceStart(dbg, vpid, base);
            if (!ui_started && UiSourceIdle()) {
                g_ui_session_active.store(false, std::memory_order_relaxed);
                DebugPumpStop();
                DbgDetach(dbg);
                return false;
            }
            if (ui_started) {
                LogLine("home: streaming HOME 1280x720");
                g_reconfig_request.store(false, std::memory_order_relaxed);
                end = g_uvc_raw ? TryUvcRawStream(dbg, base, vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr, std::addressof(ug))
                                : TryNvencStream(dbg, base, vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr, true, std::addressof(ug));
            }
            UiSourceStop();
            g_ui_session_active.store(false, std::memory_order_relaxed);
            DebugPumpStop();
            DbgDetach(dbg);
            UiSourceLogStats("live-ui");
            g_live_fps_x10.store(0, std::memory_order_relaxed);
            g_live_game_fps_x10.store(0, std::memory_order_relaxed);
            static const char *const names[] = { "done", "a game is on screen", "the viewer went away", "NVENC stalled", "failed",
                                                 "restarting (new settings or a game's new buffers)", "turned off, or an applet needs the memory",
                                                 "a 1080p picture - setting up again on the full memory loan" };
            LogLine("   live[%u]: HOME-menu session ended - %s; detached from vi", sessions, names[static_cast<int>(end)]);
            if (end == StreamEnd::GameGone) { LogLine("home: back to the game"); }
            if (end == StreamEnd::Failed) { os::SleepThread(TimeSpan::FromSeconds(5)); }
            return true;
        }

        void SettleAfterRebuild() {
            static constinit u64 s_rebuild_ns[3] = {};
            static constinit u32 s_rebuild_i = 0;
            const u64 now = armTicksToNs(armGetSystemTick());
            s_rebuild_ns[s_rebuild_i++ % 3] = now;
            const u64 oldest = s_rebuild_ns[s_rebuild_i % 3];
            const bool churning = s_rebuild_i >= 3 && now - oldest < UINT64_C(20000000000);
            const u64 quiet_ns = churning ? UINT64_C(3000000000) : UINT64_C(1000000000);
            const u64 pid = g_app_pid.load(std::memory_order_relaxed);
            u32 gen = g_game_surface.generation;
            u64 t_gen = now;
            for (;;) {
                os::SleepThread(TimeSpan::FromMilliSeconds(100));
                ViewerKeepAlive();
                const u64 t = armTicksToNs(armGetSystemTick());
                if (g_app_pid.load(std::memory_order_relaxed) != pid) { return; }
                if (g_game_surface.generation != gen) { gen = g_game_surface.generation; t_gen = t; }
                const u64 last_present = armTicksToNs(g_app_present_tick.load(std::memory_order_relaxed));
                const bool presenting = last_present != 0 && t > last_present && t - last_present < UINT64_C(200000000);
                if (presenting && t - t_gen >= quiet_ns) { break; }
                if (t - now >= UINT64_C(8000000000)) { break; }
            }
            const u64 waited = armTicksToNs(armGetSystemTick()) - now;
            if (churning || waited > UINT64_C(1500000000)) {
                LogLine("   live: the game %s its frame buffers - waited %llu ms for it to settle before attaching again",
                        churning ? "keeps rebuilding" : "rebuilt", static_cast<unsigned long long>(waited / 1000000));
            }
        }

        constexpr u64 AutoSkipNs = UINT64_C(6000000000);
        struct AttachRecord { u64 tid; u64 ns; bool candidate; };

        AttachRecord NoteAttach(u64 pid, bool applet_target) {
            AttachRecord r = { g_app_tid.load(std::memory_order_relaxed), armTicksToNs(armGetSystemTick()), false };
            if (applet_target || r.tid == 0 || g_pmdmnt_rc != 0) { return r; }
            os::NativeHandle h = os::InvalidNativeHandle;
            ncm::ProgramLocation loc = {};
            cfg::OverrideStatus st = {};
            if (R_SUCCEEDED(::ams::pm::dmnt::AtmosphereGetProcessInfo(std::addressof(h), std::addressof(loc), std::addressof(st), os::ProcessId{pid}))) {
                os::CloseNativeHandle(h);
                r.candidate = !st.IsHbl() && loc.program_id.value == r.tid;
            }
            return r;
        }

        void AutoSkipIfClosedAtOnce(const AttachRecord &a) {
            const u64 ms = (armTicksToNs(armGetSystemTick()) - a.ns) / UINT64_C(1000000);
            if (!a.candidate || ms * UINT64_C(1000000) > AutoSkipNs) { return; }
            SetExcluded(a.tid, true);
            LogLine("   live: %016llx closed itself %llu ms after the stream attached (anti-debug, as TiCo) - skipped from now on; \"Stream this app\" in the overlay brings it back",
                    static_cast<unsigned long long>(a.tid), static_cast<unsigned long long>(ms));
            NotifyShot("This app closed when streamed - it is skipped from now on");
        }

        void RunLive(u32 vfd, u32 nvmap_fd, u32 cmd_handle, u32 syncpt, u32 cfg_addr) {
            LogLine("   ==== LIVE MODE (M86): streaming whenever a viewer is reading and a game is running ====");
            {
                const u32 v = hosversionGet();
                if (g_release_mode && !g_anyfw_armed && (HOSVER_MAJOR(v) < 21 || HOSVER_MAJOR(v) > 23)) {
                    g_live_state.store(LiveState_Unsupported, std::memory_order_relaxed);
                    LogLine("   live: firmware %u.%u.%u is untested (this build: 21.x-23.x) - not streaming. allow_untested_firmware = 1 in config.ini overrides.",
                            HOSVER_MAJOR(v), HOSVER_MINOR(v), HOSVER_MICRO(v));
                    VicStage("live:untested_fw");
                    SetViewerReason(ViewerReason_Unsupported);
                    for (;;) { static_cast<void>(UsbViewerPresent(300)); os::SleepThread(TimeSpan::FromSeconds(2)); }
                }
            }
            u64 skip_pid = 0;
            bool skip_big = false;
            u32 skip_gen = 0;
            bool said_wait_viewer = false, said_wait_game = false;
            u64 hidden_since_ns = 0;
            bool said_hidden_wait = false;
            u32 sessions = 0;
            bool said_off = false;
            constexpr u64 IdleTeardownNs = 15'000'000'000ull;
            u64 idle_since = 0;
            auto idle_long = [&]() -> bool {
                const u64 now = armTicksToNs(armGetSystemTick());
                if (idle_since == 0) { idle_since = now; }
                return g_donor_mode && now - idle_since >= IdleTeardownNs && g_ind_buf == nullptr && g_capture_handles == 0;
            };
            for (;;) {
                if (g_engine_wedged) {
                    g_live_state.store(LiveState_Wedged, std::memory_order_relaxed);
                    VicStage("live:engine_wedged");
                    LogLine("   live: the engine is wedged - no more streaming this boot");
                    SetViewerReason(ViewerReason_Wedged);
                    for (;;) { static_cast<void>(UsbViewerPresent(300)); os::SleepThread(TimeSpan::FromSeconds(2)); }
                }
                if (g_applet_needs_memory.load(std::memory_order_relaxed) && g_donor_mode) {
                    CaptureRelease("live");
                    if (g_ind_buf == nullptr && g_capture_handles == 0) { LogLine("   live: an applet needs the memory - closing the engines now"); return; }
                }
                if (g_donor_regrow.load(std::memory_order_relaxed) && g_donor_mode) {
                    CaptureRelease("live");
                    if (g_ind_buf == nullptr && g_capture_handles == 0) {
                        g_donor_regrow.store(false, std::memory_order_relaxed);
                        LogLine("   live: setting up again on the full memory loan - closing the engines now");
                        return;
                    }
                }
                UsbFollowStreamSwitch();
                if (TakeShotRequest()) { static_cast<void>(ShotStandalone()); }
                if (!g_stream_enabled.load(std::memory_order_relaxed)) {
                    g_live_state.store(LiveState_Off, std::memory_order_relaxed);
                    if (!said_off) { LogLine("   live: turned off (overlay)"); VicStage("live:off"); said_off = true; }
                    SetViewerReason(ViewerReason_Off);
                    CaptureRelease("live");
                    if (idle_long()) { LogLine("   live: off for 15 s - closing the engines, memory back to the system"); return; }
                    os::SleepThread(TimeSpan::FromMilliSeconds(250));
                    continue;
                }
                if (said_off) { LogLine("   live: turned on (overlay)"); said_off = false; said_wait_viewer = said_wait_game = false; }
                if (!UsbViewerPresent(300)) {
                    g_live_state.store(LiveState_WaitViewer, std::memory_order_relaxed);
                    if (!said_wait_viewer) { LogLine("   live: waiting for a viewer (raw-view) to read the stream"); VicStage("live:no_viewer"); said_wait_viewer = true; }
                    CaptureRelease("live");
                    if (idle_long()) { LogLine("   live: no viewer for 15 s - closing the engines, memory back to the system"); return; }
                    os::SleepThread(TimeSpan::FromMilliSeconds(700));
                    continue;
                }
                said_wait_viewer = false;
                idle_since = 0;

                ::ams::os::ProcessId pid{};
                const u32 q_before = g_queue_count.load(std::memory_order_relaxed);
                const bool have_app = g_pmdmnt_rc == 0 && R_SUCCEEDED(::ams::pm::dmnt::GetApplicationProcessId(std::addressof(pid)));
                if (g_ui_stream_armed && SwitchUOpen() && (g_applet_target_pid.load(std::memory_order_relaxed) == 0 || !g_applet_surface.armed)) {
                    if (!g_applet_surface.armed) {
                        constexpr const char NeedRemount[] = "sdmc:/config/nxstream/tap/need_capture_remount";
                        static_cast<void>(fs::CreateDirectory("sdmc:/config/nxstream"));
                        static_cast<void>(fs::CreateDirectory("sdmc:/config/nxstream/tap"));
                        static_cast<void>(fs::DeleteFile(NeedRemount));
                        static_cast<void>(fs::CreateFile(NeedRemount, 0));
                        LogLine("   live: waiting up to 3.5s for 100D binders (SwitchU remount)");
                        for (u32 w = 0; w < 70 && !g_applet_surface.armed; ++w) {
                            os::SleepThread(TimeSpan::FromMilliSeconds(50));
                        }
                        static_cast<void>(fs::DeleteFile(NeedRemount));
                    }
                    if (!g_applet_surface.armed) {
                        static_cast<void>(SeedRunningAlbumSurface());
                    } else if (g_applet_target_pid.load(std::memory_order_relaxed) == 0) {
                        static_cast<void>(SeedRunningAlbumSurface());
                    }
                }
                const u64 applet_pid = g_ui_stream_armed ? g_applet_target_pid.load(std::memory_order_relaxed) : 0;
                if (skip_pid != 0 && have_app && pid.value == skip_pid && g_game_surface.generation != skip_gen) {
                    LogLine("   live: the game made new frame buffers - looking for its swapchain again");
                    skip_pid = 0;
                    said_wait_game = false;
                }
                if ((have_app && pid.value != skip_pid) || applet_pid != 0) {
                    os::SleepThread(TimeSpan::FromMilliSeconds(200));
                }
                bool presenting = g_queue_count.load(std::memory_order_relaxed) != q_before;
                bool applet_target = false;
                if (presenting && applet_pid != 0) {
                    const u64 presenter = g_present_pid.load(std::memory_order_relaxed);
                    if (presenter == applet_pid && (!have_app || presenter != pid.value)) {
                        pid.value = applet_pid;
                        applet_target = true;
                    } else if (!have_app || presenter != pid.value) {
                        presenting = false;
                    }
                } else if (!presenting && applet_pid != 0 && g_applet_surface.armed && !have_app && g_album_seed_no_binder.load(std::memory_order_relaxed)) {
                    presenting = true;
                    applet_target = true;
                    pid.value = applet_pid;
                }
                const bool have_target = have_app || applet_target;
                if (have_app && !applet_target && g_app_excluded.load(std::memory_order_relaxed)) {
                    g_live_state.store(LiveState_Excluded, std::memory_order_relaxed);
                    SetViewerReason(ViewerReason_Excluded);
                    if (!said_wait_game) { LogLine("   live: %016llx is excluded - not attaching", static_cast<unsigned long long>(g_app_tid.load())); VicStage("live:excluded"); said_wait_game = true; }
                    CaptureRelease("live");
                    os::SleepThread(TimeSpan::FromMilliSeconds(500));
                    continue;
                }
                if (!have_target || pid.value == skip_pid || !presenting) {
                    static u64 s_ui_look_ns = 0;
                    const u64 look_now = armTicksToNs(armGetSystemTick());
                    const bool look = !have_app || !UiSourceIdle() || look_now - s_ui_look_ns >= UINT64_C(1000000000);
                    if (g_ui_stream_armed && look) {
                        s_ui_look_ns = look_now;
                        if (RunUiSession(vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr, sessions)) {
                            said_wait_game = false;
                            continue;
                        }
                        if (g_donor_mode && g_detach_on_home) {
                            const char *ui_why = nullptr;
                            if (HomeDetachShouldFire(50, g_home_detach_ms, g_home_fallback_ms, std::addressof(ui_why))) {
                                PrepareForHomeMenu();
                                CaptureRelease("live");
                                if (g_ind_buf == nullptr && g_capture_handles == 0) {
                                    LogLine("   live: home_menu UI unavailable (%s) - closing the engines, memory back for HOME/Album",
                                            ui_why != nullptr ? ui_why : "?");
                                    return;
                                }
                            }
                        }
                    }
                    if (!g_ui_stream_armed && !presenting) {
                        static bool said_home_off = false;
                        if (!said_home_off) { said_home_off = true; LogLine("home: not streamed - home_menu is off (config.ini is read at boot: restart the console after changing it)"); }
                    }
                    g_live_state.store(LiveState_WaitGame, std::memory_order_relaxed);
                    SetViewerReason(!have_app ? ViewerReason_NoGame : (pid.value == skip_pid ? (skip_big ? ViewerReason_TooBig : ViewerReason_NotFound) : ViewerReason_GameHidden));
                    const bool web_menu = have_app && WebMenuUp();
                    const bool loading = have_app && g_ui_stream_armed && (UiSourceIdle() || SwitchUMenuClosed());
                    const bool app_hidden = have_app && pid.value != skip_pid && !presenting;
                    if (g_donor_mode && g_detach_on_home && app_hidden && !loading) {
                        const u64 now_ns = armTicksToNs(armGetSystemTick());
                        if (hidden_since_ns == 0) { hidden_since_ns = now_ns; said_hidden_wait = false; }
                        const u64 age_ms = (now_ns - hidden_since_ns) / UINT64_C(1000000);
                        const char *home_why = nullptr;
                        if (HomeDetachShouldFire(age_ms, g_home_detach_ms, g_home_fallback_ms, std::addressof(home_why))) {
                            PrepareForHomeMenu();
                            CaptureRelease("live");
                            if (g_ind_buf == nullptr && g_capture_handles == 0) {
                                LogLine("   live: home confirmed (%s) after %llu ms not presenting - closing the engines, memory back to the system",
                                        home_why != nullptr ? home_why : "?", static_cast<unsigned long long>(age_ms));
                                return;
                            }
                        } else {
                            if (!said_hidden_wait) {
                                LogLine("   live: game not presenting (%llu ms) - holding capture through load gaps (detach needs HOME button / Album / applet or %u ms fallback)",
                                        static_cast<unsigned long long>(age_ms), g_home_fallback_ms);
                                VicStage("live:no_game");
                                said_hidden_wait = true;
                                said_wait_game = true;
                            }
                            os::SleepThread(TimeSpan::FromMilliSeconds(200));
                            continue;
                        }
                    }
                    if (g_donor_mode && g_detach_on_home && pid.value != skip_pid && (!have_app || web_menu)) {
                        CaptureRelease("live");
                        if (g_ind_buf == nullptr && g_capture_handles == 0) {
                            LogLine("   live: %s - closing the engines, memory back to the system",
                                    !have_app ? "no game" : "a web applet's menu is over the game");
                            return;
                        }
                    }
                    hidden_since_ns = 0;
                    said_hidden_wait = false;
                    if (!said_wait_game) {
                        LogLine("   live: viewer present; waiting for a game (%s)",
                                !have_app ? "no application" : (pid.value == skip_pid ? (skip_big ? "this game draws above 1080p" : "this game's swapchain was not found") : "not presenting"));
                        VicStage("live:no_game");
                        said_wait_game = true;
                    }
                    CaptureRelease("live");
                    os::SleepThread(TimeSpan::FromMilliSeconds(have_app ? 100 : 800));
                    continue;
                }
                said_wait_game = false;
                hidden_since_ns = 0;
                said_hidden_wait = false;

                if (!CaptureAcquire("live")) {
                    static u32 short_n = 0;
                    if ((short_n++ % 15) == 0) { LogLine("   live: not enough memory free to stream right now - trying again"); }
                    VicStage("live:no_memory");
                    SetViewerReason(ViewerReason_NoMemory);
                    os::SleepThread(TimeSpan::FromSeconds(2));
                    continue;
                }

                SetViewerReason(ViewerReason_Starting);
                ++sessions;
                g_live_sessions.store(sessions, std::memory_order_relaxed);
                ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
                if (!DmAttach(pid.value, std::addressof(dbg))) {
                    LogLine("   live[%u]: no attach to pid %llu (direct failed, dmnt not holding it) - retrying in 5 s", sessions,
                            static_cast<unsigned long long>(pid.value));
                    os::SleepThread(TimeSpan::FromSeconds(5));
                    continue;
                }
                GameSurface geo;
                const GameSurface &src_surface = applet_target ? g_applet_surface : g_game_surface;
                g_session_surface = std::addressof(src_surface);
                for (u32 tries = 0; tries < 10; ++tries) {
                    const u32 gen = src_surface.generation;
                    geo = src_surface;
                    if (gen == src_surface.generation) { break; }
                }
                if (applet_target) { LogLine("   live[%u]: the Album / hbmenu is in front (pid %llu)", sessions, static_cast<unsigned long long>(pid.value)); }
                u64 slot_base = 0;
                const u64 f0 = armTicksToNs(armGetSystemTick());
                const bool found = LocateSwapchain(dbg, geo, std::addressof(slot_base), "live");
                u64 want = FbSwapSize;
                if (geo.num_slots != 0 && geo.buf_size != 0) {
                    u32 top = 0;
                    for (u32 k = 0; k < geo.num_slots && k < 8; ++k) { if (geo.slot_offset[k] > top) { top = geo.slot_offset[k]; } }
                    want = static_cast<u64>(top) + geo.buf_size;
                }
                u32 nev = 0;
                const Result rc = DbgResume(dbg, std::addressof(nev));
                const u64 frozen_us = (armTicksToNs(armGetSystemTick()) - f0) / 1000;
                LogLine("   live[%u]: attached to pid %llu; %ux%u, %u slot(s); swapchain (%llu B) %s%010llx; game held %llu us; %u attach events, continue rc=0x%x",
                        sessions, static_cast<unsigned long long>(pid.value), geo.width, geo.height, geo.num_slots,
                        static_cast<unsigned long long>(want), found ? "at 0x" : "NOT FOUND ",
                        static_cast<unsigned long long>(slot_base), static_cast<unsigned long long>(frozen_us), nev, rc.GetValue());
                if (R_FAILED(rc)) {
                    DbgDetach(dbg);
                    os::SleepThread(TimeSpan::FromSeconds(2));
                    continue;
                }
                if (!found) {
                    DbgDetach(dbg);
                    skip_pid = pid.value;
                    skip_gen = geo.generation;
                    skip_big = false;
                    continue;
                }
                if (geo.width > 1920 || geo.height > 1088) {
                    DbgDetach(dbg);
                    LogLine("   live: this game draws at %ux%u - above 1080p, not streamed (until it makes new frame buffers)", geo.width, geo.height);
                    skip_pid = pid.value;
                    skip_gen = geo.generation;
                    skip_big = true;
                    SetViewerReason(ViewerReason_TooBig);
                    continue;
                }
                if (!DbgIsDmnt(dbg)) { DebugPumpStart(dbg); }
                g_reconfig_request.store(false, std::memory_order_relaxed);
                const GameSurface *gp = (geo.num_slots != 0 && geo.buf_size != 0) ? std::addressof(geo) : nullptr;
                const AttachRecord attached = NoteAttach(pid.value, applet_target);
                g_live_pid.store(pid.value, std::memory_order_relaxed);
                const StreamEnd end = g_uvc_raw ? TryUvcRawStream(dbg, slot_base, vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr, gp)
                                                : TryNvencStream(dbg, slot_base, vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr, true, gp);
                g_live_pid.store(0, std::memory_order_relaxed);
                const bool target_exited = !DbgIsDmnt(dbg) && DebugPumpTargetGone();
                if (!DbgIsDmnt(dbg)) { DebugPumpStop(); }
                DbgDetach(dbg);
                g_live_fps_x10.store(0, std::memory_order_relaxed);
                g_live_game_fps_x10.store(0, std::memory_order_relaxed);
                static const char *const names[] = { "done", "the game went away", "the viewer went away", "NVENC stalled", "failed",
                                                     "the picture size changed - restarting at the new size", "turned off from the overlay",
                                                     "a 1080p picture - setting up again on the full memory loan" };
                LogLine("   live[%u]: session ended - %s; detached", sessions, names[static_cast<int>(end)]);
                if (end == StreamEnd::GameGone && target_exited) { AutoSkipIfClosedAtOnce(attached); }
                if (end == StreamEnd::ViewerGone && g_donor_mode && !g_ui_stream_armed) {
                    PrepareForHomeMenu();
                    CaptureRelease("live");
                    if (g_ind_buf == nullptr && g_capture_handles == 0) {
                        LogLine("   live: viewer closed - closing the engines, memory back for HOME/Album");
                        return;
                    }
                }
                if (end == StreamEnd::Failed) { os::SleepThread(TimeSpan::FromSeconds(5)); }
                else if (end == StreamEnd::GameGone && g_ui_stream_armed) { os::SleepThread(TimeSpan::FromMilliSeconds(300)); }
                else if (end != StreamEnd::Reconfigure && end != StreamEnd::Disabled && end != StreamEnd::Regrow) { os::SleepThread(TimeSpan::FromSeconds(1)); }
                else if (end == StreamEnd::Reconfigure) {
                    os::SleepThread(TimeSpan::FromMilliSeconds(300));
                    if (!applet_target) { SettleAfterRebuild(); }
                }
            }
        }

        void TryDebugCapture(u32 vfd, u32 nvmap_fd, u32 cmd_handle, u32 syncpt, u32 cfg_addr, u32 dst_addr) {
            if (!g_dbg_armed) { return; }
            if (g_live_armed) { RunLive(vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr); return; }

            VicStage("dbg:1_find_pid");
            if (g_pmdmnt_rc != 0) {
                LogLine("   pmdmntInitialize() failed at boot (rc=0x%x) - skipping", g_pmdmnt_rc);
                VicStage("dbg:1_no_pmdmnt");
                return;
            }
            ::ams::os::ProcessId pid{};
            {
                const Result r = ::ams::pm::dmnt::GetApplicationProcessId(std::addressof(pid));
                LogLine("   pm:dmnt GetApplicationProcessId rc=0x%x -> pid=%llu",
                        r.GetValue(), static_cast<unsigned long long>(pid.value));
                if (R_FAILED(r)) { VicStage("dbg:1_FAILED"); return; }
            }

            RegionHit   big[8]  = {};   u32 nbig  = 0;
            FbCandidate cand    = {};   bool found = false, by_size = false;
            u32  steps = 0, reads = 0, budget = 24000, nexact = 0;
            bool walk_complete = false;

            u32  nev = 0;
            ::ams::Result r_attach{}, r_cont = ::ams::svc::ResultInvalidHandle();
            bool attached = false, resumed = false;

            u64  strip_ns = 0, dump_ns = 0, full_ns = 0;
            u32  strip_chunks_ok = 0, dump_chunks = 0, full_strips = 0;
            u32  cap_done = 0, cap_missed = 0, cap_distinct = 0;
            u64  cap_min = ~UINT64_C(0), cap_max = 0, cap_sum = 0, cap_ns = 0;
            u32  fps_before = 0, fps_during = 0, fps_after = 0;
            u32  cap_over = 0, slot_hits[4] = {};
            u32  strip_own_rc = 0, strip_pin = 0;
            u64  frozen_ns = 0;
            bool strip_read_ok = false, strip_blit_ok = false, strip_dumped = false, vic_dumped = false;
            u32  strip_ok_count = 0, strip_dump_count = 0, variant_sum[8] = {};
            bool dumped = false;
            u32  nonzero = 0, distinct = 0, lane_ff[4] = {}, total_px = 0;
            u8   live_a[16] = {}, live_b[16] = {};
            bool live_ok = false, live_changed = false;

            ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
            attached = DmAttach(pid.value, std::addressof(dbg));
            if (attached) { r_attach = ResultSuccess(); } else { r_attach = ::ams::svc::ResultInvalidHandle(); }
            if (attached) {

                u64 addr = 0;
                for (; steps < 4000; ++steps) {
                    ::ams::svc::MemoryInfo mi = {};
                    ::ams::svc::PageInfo   pi = {};
                    if (!DbgQuery(dbg, std::addressof(mi), std::addressof(pi), addr)) { walk_complete = true; break; }
                    if (mi.size == 0) { walk_complete = true; break; }

                    const u32  attr = static_cast<u32>(mi.attribute);
                    const bool dev  = (attr & ::ams::svc::MemoryAttribute_DeviceShared) != 0 || mi.device_count > 0;

                    if (dev && mi.size >= FbSwapSize && nbig < 8) {
                        const RegionHit r = { mi.base_address, mi.size, static_cast<u32>(mi.state),
                                              static_cast<u32>(mi.permission), attr, mi.device_count };
                        if (mi.size == FbSwapSize) {
                            for (u32 k = nbig; k > nexact; --k) { big[k] = big[k - 1]; }
                            big[nexact++] = r;
                        } else {
                            big[nbig] = r;
                        }
                        ++nbig;
                    }
                    const u64 next = mi.base_address + mi.size;
                    if (next <= addr) { walk_complete = true; break; }
                    addr = next;
                }

                u8 s[3][64];
                if (nexact > 0) {
                    cand.addr        = big[0].base;
                    cand.region_base = big[0].base;
                    cand.lane        = 3;
                    found            = true;
                    by_size          = true;
                    for (u32 k = 0; k < 3; ++k) {
                        ++reads;
                        if (DbgRead(dbg, s[k], cand.addr + FbSlotOff[k], 64)) {
                            std::memcpy(cand.head[k], s[k], 16);
                        }
                    }
                }
                for (u32 r = 0; r < nbig && !found && budget > 0; ++r) {
                    const u64 last = big[r].size - FbSwapSize;
                    for (u64 off = 0; off <= last && !found && budget > 0; off += 0x1000) {
                        --budget; ++reads;
                        if (!DbgRead(dbg, s[0], big[r].base + off, 64)) { continue; }
                        u32 varied = 0;
                        const s32 lane = FbPixelLane(s[0], std::addressof(varied));
                        if (lane < 0) { continue; }

                        bool all_slots = true;
                        for (u32 k = 1; k < 3; ++k) {
                            --budget; ++reads;
                            if (!DbgRead(dbg, s[k], big[r].base + off + FbSlotOff[k], 64)
                                || FbPixelLane(s[k], nullptr) != lane) { all_slots = false; break; }
                        }
                        if (!all_slots) { continue; }

                        cand.addr        = big[r].base + off;
                        cand.region_base = big[r].base;
                        cand.lane        = static_cast<u32>(lane);
                        cand.varied      = varied;
                        for (u32 k = 0; k < 3; ++k) { std::memcpy(cand.head[k], s[k], 16); }
                        found = true;
                    }
                }

                const u64 t_frozen0 = armTicksToNs(armGetSystemTick());
                auto resume_game = [&]() {
                    if (resumed) { return; }
                    r_cont = DbgResume(dbg, std::addressof(nev));
                    resumed = R_SUCCEEDED(r_cont);
                    frozen_ns = armTicksToNs(armGetSystemTick()) - t_frozen0;
                    if (resumed && !DbgIsDmnt(dbg)) { DebugPumpStart(dbg); }
                };

                if (found && !g_stream_armed && !g_nvframe_armed && !g_nvstream_armed && !g_nvp_armed && !g_nv1080_armed && g_ind_buf != nullptr && g_ind_size >= FbBlockRow && cfg_addr != 0 && dst_addr != 0) {
                    VicStage("vs:1_own_capture_buf");
                    u32 cap_handle = 0, cap_id = 0, cap_addr = 0;
                    const auto orc = NvmapOwn(nvmap_fd, g_ind_buf, static_cast<u32>(FbBlockRow), 0,
                                              std::addressof(cap_handle), std::addressof(cap_id), true);
                    if (R_SUCCEEDED(orc)) {
                        VicStage("vs:2_pin");
                        MapCmdBuffer(vfd, cap_handle, std::addressof(cap_addr), "capture", 0);
                        if (cap_addr != 0) {
                            VicStage("vs:3_read_block_row");
                            strip_read_ok = DbgRead(dbg, g_ind_buf, cand.addr, FbBlockRow);
                            if (strip_read_ok) {
                                armDCacheFlush(g_ind_buf, FbBlockRow);

                                resume_game();

                                VicStage("vs:4_dump_strip");
                                strip_dumped = WriteBufToSd(StripBinPath, g_ind_buf, FbBlockRow);

                                const JobCtx sctx{ vfd, cmd_handle, syncpt, cfg_addr, dst_addr,
                                                   cap_addr, 0, 0, 0, 0, SelfSrc };
                                for (u32 v = 0; v < StripVariantCount; ++v) {
                                    const StripVariant &sv = StripVariants[v];
                                    g_strip_src = SrcDesc{ StripW, StripH, StripW,
                                                           vic::BLK_KIND_GENERIC_16Bx2, 4,
                                                           sv.src_fmt, vic::CACHE_WIDTH_64Bx4,
                                                           sv.rect_w, sv.rect_h };
                                    g_strip_out = OutDesc{ StripOut.w, StripOut.h,
                                                           StripOut.stride_px, sv.out_fmt };
                                    char stage[48];
                                    std::snprintf(stage, sizeof(stage), "vb:strip_%s", sv.name);
                                    VicStage("vs:5_sweep");
                                    if (RunOneJob(stage, VicJob::BlitStrip, true, sctx)) { ++strip_ok_count; }

                                    std::memcpy(g_stage_buf, g_vic_dst_buf, StripOutSize);
                                    variant_sum[v] = 0;
                                    for (u32 k = 0; k < StripOutSize; ++k) { variant_sum[v] += g_stage_buf[k]; }
                                    char path[64];
                                    std::snprintf(path, sizeof(path), "sdmc:/applet-mitm-vic-%s.bin", sv.name);
                                    if (WriteBufToSd(path, g_stage_buf, StripOutSize)) { ++strip_dump_count; }
                                }
                                strip_blit_ok = strip_ok_count > 0;
                                vic_dumped    = strip_dump_count > 0;
                            }
                            UnmapCmdBuffer(vfd, cap_handle);
                        }
                    }
                    strip_own_rc = orc;
                    strip_pin    = cap_addr;
                }

                if (found && g_dump_armed && g_ind_buf != nullptr && g_ind_size >= 0x10000) {
                    dumped = DumpSlotToSd(dbg, cand.addr, std::addressof(dump_ns), std::addressof(dump_chunks));
                }

                resume_game();

                if (found && g_bench_armed && g_ind_buf != nullptr
                    && g_ind_size >= FbSlotSize && cfg_addr != 0 && dst_addr != 0) {
                    VicStage("bn:1_pin");
                    u32 bh = 0, bi = 0, ba = 0;
                    const auto brc = NvmapOwn(nvmap_fd, g_ind_buf, static_cast<u32>(FbSlotSize), 0,
                                              std::addressof(bh), std::addressof(bi), true);
                    if (R_FAILED(brc)) {
                        LogLine("   bench: NvmapOwn rc=0x%x", brc);
                    } else {
                        MapCmdBuffer(vfd, bh, std::addressof(ba), "bench-src", 0);
                        if (ba == 0) {
                            LogLine("   bench: pin returned 0 - refusing to submit");
                        } else {
                            VicStage("bn:2_read");
                            if (!DbgRead(dbg, g_ind_buf, cand.addr, FbSlotSize)) {
                                LogLine("   bench: slot read failed");
                            } else {
                                armDCacheFlush(g_ind_buf, FbSlotSize);

                                const SrcDesc full_src{ 1920, 1080, 1920,
                                                        vic::BLK_KIND_GENERIC_16Bx2, 4,
                                                        vic::PIXFMT_A8R8G8B8, vic::CACHE_WIDTH_64Bx4,
                                                        1920, 1080 };
                                const JobCtx bc{ vfd, cmd_handle, syncpt, cfg_addr, dst_addr, ba, 0, 0, 0, 0, SelfSrc };
                                constexpr u32 BenchW = 640, BenchH = 360, BenchN = 60;

                                struct BenchCase { const char *name; u32 fmt; };
                                const BenchCase cases[2] = {
                                    { "RGBA", vic::PIXFMT_A8B8G8R8 },
                                    { "NV12", vic::PIXFMT_Y8_U8V8_N420 },
                                };

                                for (u32 ci = 0; ci < 2; ++ci) {
                                    g_strip_src = full_src;
                                    g_strip_out = OutDesc{ BenchW, BenchH, BenchW, cases[ci].fmt };

                                    g_vic_quiet = false;
                                    const bool warm = RunOneJob("bn:warmup", VicJob::BlitStrip, true, bc);
                                    LogLine("   bench %s: warm-up %s", cases[ci].name, warm ? "OK" : "FAILED");
                                    if (!warm) { continue; }

                                    g_vic_quiet    = true;
                                    g_vic_ns_total = 0; g_vic_ns_worst = 0; g_vic_jobs = 0; g_vic_timeouts = 0;
                                    u32 ok = 0;
                                    const u64 t0 = armTicksToNs(armGetSystemTick());
                                    for (u32 i = 0; i < BenchN; ++i) {
                                        if (RunOneJob("bn:job", VicJob::BlitStrip, true, bc)) { ++ok; }
                                    }
                                    const u64 wall = armTicksToNs(armGetSystemTick()) - t0;
                                    g_vic_quiet = false;

                                    const u32 n = (g_vic_jobs > 0) ? g_vic_jobs : 1;
                                    LogLine("   bench %s: %u timeouts (syncpt never reached the fence)",
                                            cases[ci].name, g_vic_timeouts);
                                    LogLine("   bench %s %ux%u: %u/%u jobs, submit+wait avg %llu us, worst %llu us",
                                            cases[ci].name, BenchW, BenchH, ok, BenchN,
                                            static_cast<unsigned long long>(g_vic_ns_total / 1000 / n),
                                            static_cast<unsigned long long>(g_vic_ns_worst / 1000));
                                    LogLine("   bench %s: wall %llu us for %u jobs = %llu us/frame  (60 fps budget 16667)",
                                            cases[ci].name,
                                            static_cast<unsigned long long>(wall / 1000), BenchN,
                                            static_cast<unsigned long long>(wall / 1000 / BenchN));
                                }

                                {
                                    const size_t nv12_sz = static_cast<size_t>(BenchW) * BenchH * 3 / 2;
                                    if (g_stage_buf != nullptr && nv12_sz <= StreamStageSize) {
                                        std::memcpy(g_stage_buf, g_vic_dst_buf, nv12_sz);
                                        const size_t ysz = static_cast<size_t>(BenchW) * BenchH;
                                        u8 ymin = 255, ymax = 0, cmin = 255, cmax = 0;
                                        for (size_t i = 0; i < ysz; ++i) {
                                            const u8 v = g_stage_buf[i];
                                            if (v < ymin) { ymin = v; }
                                            if (v > ymax) { ymax = v; }
                                        }
                                        for (size_t i = ysz; i < nv12_sz; ++i) {
                                            const u8 v = g_stage_buf[i];
                                            if (v < cmin) { cmin = v; }
                                            if (v > cmax) { cmax = v; }
                                        }
                                        LogLine("   bench: luma %u..%u   chroma %u..%u   %s",
                                                ymin, ymax, cmin, cmax,
                                                (cmin == cmax) ? "*** CHROMA PLANE NOT WRITTEN ***" : "chroma varies");
                                        const bool w = WriteBufToSd("sdmc:/applet-mitm-nv12.bin", g_stage_buf, nv12_sz);
                                        LogLine("   bench: nv12 dump %s (%zu B, %ux%u)",
                                                w ? "OK -> sdmc:/applet-mitm-nv12.bin" : "FAILED", nv12_sz, BenchW, BenchH);
                                    }
                                }
                            }
                            UnmapCmdBuffer(vfd, bh);
                        }
                    }
                    VicStage("bn:done");
                }

                if (found && g_nvframe_armed && !g_stream_armed && cfg_addr != 0) {
                    TryNvencRealFrames(dbg, cand.addr, vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr);
                }
                if (found && g_nvstream_armed && !g_stream_armed && cfg_addr != 0) {
                    TryNvencStream(dbg, cand.addr, vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr);
                }
                if (found && g_nvp_armed && !g_stream_armed && cfg_addr != 0) {
                    TryNvencPFrames(dbg, cand.addr, vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr);
                }
                if (found && g_nv1080_armed && !g_stream_armed && cfg_addr != 0) {
                    TryNvenc1080(dbg, cand.addr, vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr);
                }

                if (found && g_stream_armed && g_ind_buf != nullptr
                    && g_ind_size >= FbSlotSize && cfg_addr != 0 && dst_addr != 0) {
                    VicStage("st:1_pin");
                    u32 sh = 0, si = 0, sa = 0;
                    const auto srrc = NvmapOwn(nvmap_fd, g_ind_buf, static_cast<u32>(FbSlotSize), 0,
                                               std::addressof(sh), std::addressof(si), true);
                    if (R_FAILED(srrc)) {
                        LogLine("   stream: NvmapOwn(slot) rc=0x%x", srrc);
                    } else {
                        MapCmdBuffer(vfd, sh, std::addressof(sa), "stream-src", 0);
                        if (sa == 0) {
                            LogLine("   stream: pin returned 0 - refusing to submit");
                        } else {
                            VicStage("st:2_loop");
                            if (g_sweep_armed) {
                                struct Step { u32 w, h; };
                                const Step steps[] = {
                                    {  768, 432 }, {  896, 504 }, {  960, 540 },
                                    { 1152, 648 }, { 1280, 720 },
                                };
                                for (u32 si = 0; si < sizeof(steps)/sizeof(steps[0]); ++si) {
                                    StreamFrames(dbg, cand.addr, vfd, cmd_handle, syncpt,
                                                 cfg_addr, dst_addr, sa,
                                                 steps[si].w, steps[si].h, 300);
                                }
                            } else {
                                const bool ss = UsbIsSuperSpeed();
                                u32 sW = g_stream_w, sH = g_stream_h;
                                if (g_stream_auto) {
                                    if (ss) { sW = 1600; sH = 900; }
                                    else    { sW =  800; sH = 450; }
                                }
                                LogLine("   stream: link=%s -> %ux%u (%u B/frame, %u MB/s at 60 fps)",
                                        ss ? "SUPERSPEED" : "high-speed", sW, sH,
                                        sW * sH * 3 / 2, (sW * sH * 3 / 2) * 60 / 1000000);
                                StreamFrames(dbg, cand.addr, vfd, cmd_handle, syncpt,
                                             cfg_addr, dst_addr, sa, sW, sH);
                            }
                            VicStage("st:3_done");
                        }
                        UnmapCmdBuffer(vfd, sh);
                    }
                }

                if (found) {
                    if (DbgRead(dbg, live_a, cand.addr, 16)) {
                        os::SleepThread(TimeSpan::FromMilliSeconds(120));
                        if (DbgRead(dbg, live_b, cand.addr, 16)) {
                            live_ok = true;
                            live_changed = std::memcmp(live_a, live_b, 16) != 0;
                        }
                    }

                    if (g_ind_buf != nullptr && g_ind_size >= FbBlockRow) {
                        const u64 t0 = armTicksToNs(armGetSystemTick());
                        for (u64 done = 0; done < FbBlockRow; done += 0x10000) {
                            const u64 n = (FbBlockRow - done < 0x10000) ? (FbBlockRow - done) : 0x10000;
                            if (!DbgRead(dbg, g_ind_buf + done, cand.addr + done, n)) { break; }
                            ++strip_chunks_ok;
                        }
                        strip_ns = armTicksToNs(armGetSystemTick()) - t0;

                        u32 hist[256] = {};
                        const u64 got = static_cast<u64>(strip_chunks_ok) * 0x10000;
                        const u64 lim = (got > FbBlockRow) ? FbBlockRow : got;
                        for (u64 i = 0; i < lim; ++i) {
                            const u8 v = g_ind_buf[i];
                            if (v != 0) { ++nonzero; }
                            ++hist[v];
                        }
                        for (u32 i = 0; i < 256; ++i) { if (hist[i] != 0) { ++distinct; } }
                        for (u64 i = 0; i + 3 < lim; i += 4) {
                            ++total_px;
                            for (u32 l = 0; l < 4; ++l) { if (g_ind_buf[i + l] == 0xFF) { ++lane_ff[l]; } }
                        }

                        {
                            const u64 t1 = armTicksToNs(armGetSystemTick());
                            for (u64 done = 0; done < FbSlotSize; done += FbBlockRow) {
                                const u64 n = (FbSlotSize - done < FbBlockRow) ? (FbSlotSize - done) : FbBlockRow;
                                if (!DbgRead(dbg, g_ind_buf, cand.addr + done, n)) { break; }
                                ++full_strips;
                            }
                            full_ns = armTicksToNs(armGetSystemTick()) - t1;
                        }

                        if (!g_stream_armed && !g_nvstream_armed && g_ind_size >= FbSlotSize) {
                            if (!UsbReady()) {
                                LogLine("   usb: not Configured (no host, or \"usb\" absent from the arm file) - skipping transport");
                            } else {
                                const u64 u0 = armTicksToNs(armGetSystemTick());
                                bool read_ok = true;
                                for (u64 done = 0; done < FbSlotSize; done += 0x10000) {
                                    const u64 n = (FbSlotSize - done < 0x10000) ? (FbSlotSize - done) : 0x10000;
                                    if (!DbgRead(dbg, g_ind_buf + done, cand.addr + done, n)) { read_ok = false; break; }
                                }
                                const u64 read_ns = armTicksToNs(armGetSystemTick()) - u0;
                                LogLine("   usb: resident slot read %s in %llu ms (%llu B)",
                                        read_ok ? "OK" : "FAILED",
                                        static_cast<unsigned long long>(read_ns / 1000000),
                                        static_cast<unsigned long long>(FbSlotSize));

                                if (read_ok) {
                                    struct __attribute__((packed)) SftHdr {
                                        u32 magic; u16 version; u16 flags;
                                        u32 width, height, stride, length, block_h_log2, kind;
                                    };
                                    static_assert(sizeof(SftHdr) == 32, "header must match the PC receiver");

                                    SftHdr h = {};
                                    h.magic        = 0x52544653u;
                                    h.version      = 1;
                                    h.flags        = 0;
                                    h.width        = 1920;
                                    h.height       = 1080;
                                    h.stride       = 7680;
                                    h.length       = static_cast<u32>(FbSlotSize);
                                    h.block_h_log2 = 4;
                                    h.kind         = 0xFE;
                                    std::memcpy(g_usb_hdr, std::addressof(h), sizeof(h));

                                    const u64 s0 = armTicksToNs(armGetSystemTick());
                                    size_t hdr_sent = 0, body_sent = 0;
                                    const bool hdr_ok = UsbSendBuffer(g_usb_hdr, sizeof(SftHdr), std::addressof(hdr_sent));
                                    const bool body_ok = hdr_ok && UsbSendBuffer(g_ind_buf, FbSlotSize, std::addressof(body_sent));
                                    const u64 send_ns = armTicksToNs(armGetSystemTick()) - s0;

                                    const u64 mbps = (send_ns > 0)
                                        ? (static_cast<u64>(body_sent) * UINT64_C(1000)) / (send_ns / UINT64_C(1000) + 1) / UINT64_C(1024)
                                        : 0;
                                    LogLine("   usb: header %s (%zu B), body %s (%zu / %llu B) in %llu ms ~ %llu KB/s",
                                            hdr_ok ? "sent" : "FAILED", hdr_sent,
                                            body_ok ? "sent" : "FAILED", body_sent,
                                            static_cast<unsigned long long>(FbSlotSize),
                                            static_cast<unsigned long long>(send_ns / 1000000),
                                            static_cast<unsigned long long>(mbps));
                                    LogLine("   usb: %s", (hdr_ok && body_ok)
                                            ? "*** A 1080p FRAME LEFT THE CONSOLE OVER USB ***"
                                            : "transport incomplete - see the rc above");
                                }
                            }
                        }

                        if (!g_stream_armed && !g_nvframe_armed && !g_nvstream_armed && !g_nvp_armed && !g_nv1080_armed) {
                            constexpr u32 CapFrames = 120;

                            const u32 c0 = g_queue_count.load(std::memory_order_relaxed);
                            os::SleepThread(TimeSpan::FromMilliSeconds(1000));
                            fps_before = g_queue_count.load(std::memory_order_relaxed) - c0;

                            const u32 during0 = g_queue_count.load(std::memory_order_relaxed);
                            u32 seen = during0, last_sig = 0;
                            const u64 cap_t0 = armTicksToNs(armGetSystemTick());

                            for (u32 i = 0; i < CapFrames; ++i) {
                                u32 spins = 0;
                                while (g_queue_count.load(std::memory_order_relaxed) == seen && spins < 200) {
                                    os::SleepThread(TimeSpan::FromMilliSeconds(1));
                                    ++spins;
                                }
                                if (g_queue_count.load(std::memory_order_relaxed) == seen) { ++cap_missed; break; }
                                seen = g_queue_count.load(std::memory_order_relaxed);

                                const s32 slot = g_queue_slot.load(std::memory_order_relaxed);
                                const u64 off  = (slot >= 0 && static_cast<u32>(slot) < 3) ? FbSlotOff[slot] : 0;

                                if (slot >= 0 && slot < 4) { ++slot_hits[slot]; }

                                const u64 t2 = armTicksToNs(armGetSystemTick());
                                bool ok = true;
                                u32  sig = 0;
                                for (u64 done = 0; done < FbSlotSize; done += FbBlockRow) {
                                    const u64 n = (FbSlotSize - done < FbBlockRow) ? (FbSlotSize - done) : FbBlockRow;
                                    if (!DbgRead(dbg, g_ind_buf, cand.addr + off + done, n)) { ok = false; break; }
                                    if (done == 0) {
                                        for (u32 k = 0; k < 8192; k += 8) { sig = sig * 31u + g_ind_buf[k]; }
                                    }
                                }
                                const u64 dt = armTicksToNs(armGetSystemTick()) - t2;
                                if (!ok) { ++cap_missed; continue; }

                                ++cap_done;
                                cap_sum += dt;
                                if (dt < cap_min) { cap_min = dt; }
                                if (dt > cap_max) { cap_max = dt; }
                                if (dt > UINT64_C(16667000)) { ++cap_over; }
                                if (sig != last_sig) { ++cap_distinct; last_sig = sig; }
                            }
                            cap_ns = armTicksToNs(armGetSystemTick()) - cap_t0;
                            if (cap_ns > 0) {
                                fps_during = static_cast<u32>((static_cast<u64>(seen - during0) * UINT64_C(1000000000)) / cap_ns);
                            }

                            const u32 c1 = g_queue_count.load(std::memory_order_relaxed);
                            os::SleepThread(TimeSpan::FromMilliSeconds(1000));
                            fps_after = g_queue_count.load(std::memory_order_relaxed) - c1;
                        }
                    }
                }

                if (!DbgIsDmnt(dbg)) { DebugPumpStop(); }
                DbgDetach(dbg);
            }

            VicStage("dbg:2_attached");
            LogLine("   attach(pid=%llu) rc=0x%x via %s: %s",
                    static_cast<unsigned long long>(pid.value), r_attach.GetValue(),
                    attached ? (DbgIsDmnt(dbg) ? "dmnt:cht" : "direct") : "-",
                    attached ? "ATTACHED" : "failed");
            if (!attached) { VicStage("dbg:2_FAILED"); return; }

            LogLine("   walked %u regions (%s); %u big enough (%u exactly %llu B); %u probe reads",
                    steps, walk_complete ? "complete" : "HIT THE STEP CAP", nbig, nexact,
                    static_cast<unsigned long long>(FbSwapSize), reads);
            for (u32 i = 0; i < nbig; ++i) {
                LogLine("     region %u: base=0x%010llx size=%llu%s", i,
                        static_cast<unsigned long long>(big[i].base),
                        static_cast<unsigned long long>(big[i].size),
                        (big[i].size == FbSwapSize) ? "   <== EXACT SWAPCHAIN SIZE" : "");
            }

            if (!found) {
                LogLine("   no exactly-sized region and no signature match");
                VicStage("dbg:no_signature");
                return;
            }

            LogLine("   *** SWAPCHAIN AT 0x%010llx (offset %llu into its region) - matched by %s ***",
                    static_cast<unsigned long long>(cand.addr),
                    static_cast<unsigned long long>(cand.addr - cand.region_base),
                    by_size ? "EXACT SIZE" : "pixel signature");
            for (u32 k = 0; k < 3; ++k) {
                const u8 *h = cand.head[k];
                LogLine("       slot%u +0x%07llx: %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x",
                        k, static_cast<unsigned long long>(FbSlotOff[k]),
                        h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7],
                        h[8], h[9], h[10], h[11], h[12], h[13], h[14], h[15]);
            }

            LogLine("   GAME FROZEN FOR %llu ms (M45 was 4780 ms - only the strip read needs a halt)",
                    static_cast<unsigned long long>(frozen_ns / 1000000));
            LogLine("   drained %u debug events; ContinueDebugEvent rc=0x%x -> %s",
                    nev, r_cont.GetValue(),
                    resumed ? "GAME RUNNING WHILE WE STAY ATTACHED" : "still frozen");
            if (resumed) { DebugPumpLogStats("attached, in total"); }

            if (live_ok) {
                LogLine("   live sample over 120 ms: %s", live_changed
                        ? "*** PIXELS CHANGED - the game is presenting while attached ***"
                        : "identical (game may be paused, or the corner is static)");
                LogLine("       t0: %02x %02x %02x %02x %02x %02x %02x %02x",
                        live_a[0], live_a[1], live_a[2], live_a[3], live_a[4], live_a[5], live_a[6], live_a[7]);
                LogLine("       t1: %02x %02x %02x %02x %02x %02x %02x %02x",
                        live_b[0], live_b[1], live_b[2], live_b[3], live_b[4], live_b[5], live_b[6], live_b[7]);
            }

            if (strip_chunks_ok > 0 && strip_ns > 0) {
                const u64 got     = static_cast<u64>(strip_chunks_ok) * 0x10000;
                const u64 kbps    = (got * UINT64_C(1000000)) / strip_ns;
                const u64 slot_us = (strip_ns * (FbSlotSize / 1024)) / (got / 1024) / 1000;
                LogLine("   read %llu B of block-row in %llu us -> %llu MB/s",
                        static_cast<unsigned long long>(got),
                        static_cast<unsigned long long>(strip_ns / 1000),
                        static_cast<unsigned long long>(kbps / 1000));
                LogLine("   => one full 8,847,360 B slot ~%llu us; 60 fps needs <= 16667 us  [%s]",
                        static_cast<unsigned long long>(slot_us),
                        (slot_us <= 16667) ? "FEASIBLE" : "too slow for 60 fps at full res");
                LogLine("   block-row: nonzero=%u/%llu  distinct=%u  pixels=%u", nonzero,
                        static_cast<unsigned long long>(got), distinct, total_px);
                LogLine("   0xFF per lane: [0]=%u [1]=%u [2]=%u [3]=%u  (the alpha lane is the big one)",
                        lane_ff[0], lane_ff[1], lane_ff[2], lane_ff[3]);
                LogLine("   first 16 B: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                        g_ind_buf[0], g_ind_buf[1], g_ind_buf[2], g_ind_buf[3],
                        g_ind_buf[4], g_ind_buf[5], g_ind_buf[6], g_ind_buf[7],
                        g_ind_buf[8], g_ind_buf[9], g_ind_buf[10], g_ind_buf[11],
                        g_ind_buf[12], g_ind_buf[13], g_ind_buf[14], g_ind_buf[15]);
            }

            if (full_strips > 0 && full_ns > 0) {
                const u64 mbps = (FbSlotSize * UINT64_C(1000000)) / full_ns / 1000;
                LogLine("   FULL SLOT into RAM: %u/9 strips, %llu us total, %llu MB/s -> %s",
                        full_strips,
                        static_cast<unsigned long long>(full_ns / 1000),
                        static_cast<unsigned long long>(mbps),
                        (full_ns / 1000 <= 16667) ? "*** FITS IN A 60 fps FRAME BUDGET ***"
                                                  : "over budget for 60 fps");
            }

            LogLine("   ---- VIC on real game pixels ----");
            LogLine("   nvmapOwn(capture, cacheable) rc=0x%x -> pinned at 0x%08x", strip_own_rc, strip_pin);
            LogLine("   block-row read %s; strip blit %s", strip_read_ok ? "OK" : "FAILED",
                    strip_blit_ok ? "*** COMPLETED ***" : "did not complete");
            LogLine("   swept %u variants: %u completed, %u dumped", StripVariantCount, strip_ok_count, strip_dump_count);
            for (u32 v = 0; v < StripVariantCount && v < 8; ++v) {
                LogLine("     %-12s bytesum=%u", StripVariants[v].name, variant_sum[v]);
            }
            LogLine("   dumps: %s %s   (compare with tools/compare_vic.py)",
                    strip_dumped ? "strip.bin OK" : "strip.bin FAILED",
                    vic_dumped   ? "vic.bin OK"   : "vic.bin FAILED");

            if (cap_done > 0) {
                const u64 avg     = cap_sum / cap_done;
                const u32 cap_fps = (cap_ns > 0) ? static_cast<u32>((static_cast<u64>(cap_done) * UINT64_C(1000000000)) / cap_ns) : 0;
                LogLine("   ---- sustained per-frame capture ----");
                LogLine("   captured %u full frames in %llu ms -> %u fps  (%u distinct, %u missed)",
                        cap_done, static_cast<unsigned long long>(cap_ns / 1000000),
                        cap_fps, cap_distinct, cap_missed);
                LogLine("   per-frame read: min %llu us  avg %llu us  max %llu us  (%u of %u over the 16667 us budget)",
                        static_cast<unsigned long long>(cap_min / 1000),
                        static_cast<unsigned long long>(avg / 1000),
                        static_cast<unsigned long long>(cap_max / 1000),
                        cap_over, cap_done);
                LogLine("   slots read: [0]=%u [1]=%u [2]=%u  (a rotating swapchain should hit all three)",
                        slot_hits[0], slot_hits[1], slot_hits[2]);
                const u32 base = (fps_before + fps_after) / 2;
                LogLine("   game presented: %u fps before, %u during, %u after -> %d fps delta  [%s]",
                        fps_before, fps_during, fps_after,
                        static_cast<int>(fps_during) - static_cast<int>(base),
                        (fps_during + 3 >= base) ? "*** GAME UNAFFECTED ***" : "GAME SLOWED while capturing");
            } else if (cap_missed > 0) {
                LogLine("   capture loop never got a frame (%u misses) - was the game presenting?", cap_missed);
            }

            if (dumped && dump_ns > 0) {
                const u64 mbps = (FbSlotSize * UINT64_C(1000000)) / dump_ns / 1000;
                LogLine("   *** WROTE A WHOLE FRAME (game stopped - no tearing): %s (%llu B, %u chunks, %llu ms, %llu MB/s incl. SD) ***",
                        FrameBinPath, static_cast<unsigned long long>(FbSlotSize), dump_chunks,
                        static_cast<unsigned long long>(dump_ns / 1000000),
                        static_cast<unsigned long long>(mbps));
                char meta[256];
                const int n = std::snprintf(meta, sizeof(meta),
                        "addr=0x%llx\nslot_bytes=%llu\nwidth=1920\nheight=1080\npadded_height=1152\n"
                        "pitch=7680\nbpp=4\nblock_height_log2=4\nkind=0xfe\nformat=A8B8G8R8 (bytes R,G,B,A)\n",
                        static_cast<unsigned long long>(cand.addr),
                        static_cast<unsigned long long>(FbSlotSize));
                fs::DeleteFile(FrameTxtPath);
                if (n > 0 && R_SUCCEEDED(fs::CreateFile(FrameTxtPath, n))) {
                    fs::FileHandle mf;
                    if (R_SUCCEEDED(fs::OpenFile(std::addressof(mf), FrameTxtPath, fs::OpenMode_Write))) {
                        static_cast<void>(fs::WriteFile(mf, 0, meta, n, fs::WriteOption::Flush));
                        fs::CloseFile(mf);
                    }
                }
                VicStage("dbg:FRAME_ON_SD");
            } else {
                LogLine("   frame dump did not complete (%u chunks)", dump_chunks);
                VicStage("dbg:dump_failed");
            }
        }

        constexpr u32 NvEncW = 256;
        constexpr u32 NvEncH = 128;

        constexpr u32 EncSetupOff  = 0x000000;
        constexpr u32 EncSliceOff  = 0x000200;
        constexpr u32 EncMeOff     = 0x000280;
        constexpr u32 EncMdOff     = 0x000340;
        constexpr u32 EncQOff      = 0x0003C0;
        constexpr u32 EncStatOff   = 0x001000;
        constexpr u32 EncBitsOff   = 0x002000;
        constexpr u32 EncBitsSize  = 0x040000;
        constexpr u32 EncHistOff   = 0x042000;
        constexpr u32 EncHistSize  = 0x010000;
        constexpr u32 EncInYOff    = 0x060000;
        constexpr u32 EncInUVOff   = EncInYOff  + NvEncW * NvEncH;
        constexpr u32 EncRefYOff   = EncInUVOff + NvEncW * NvEncH / 2;
        constexpr u32 EncRefUVOff  = EncRefYOff + NvEncW * NvEncH;
        constexpr u32 EncEnd       = EncRefUVOff + NvEncW * NvEncH / 2;
        static_assert(EncEnd <= VicDstSize, "NVENC probe surfaces do not fit the VIC dst buffer");

        void FillEncSurface(nvenc_h264_surface_cfg_s *c, u32 w, u32 h, bool tiled) {
            std::memset(c, 0, sizeof(*c));
            c->frame_width_minus1  = static_cast<unsigned short>(w - 1);
            c->frame_height_minus1 = static_cast<unsigned short>(h - 1);
            c->sfc_pitch           = static_cast<unsigned short>(w);
            c->sfc_pitch_chroma    = static_cast<unsigned short>(w);
            c->sfc_trans_mode      = 0;
            c->luma_top_frm_offset   = 0;
            c->luma_bot_offset       = 0;
            c->chroma_top_frm_offset = 0;
            c->chroma_bot_offset     = 0;
            c->block_height   = 0;
            c->tiled_16x16    = tiled ? 1 : 0;
            c->memory_mode    = 0;
            c->nv21_enable    = 0;
            c->input_bl_mode  = 0;
        }

        void FillEncSetup(u8 *base) {
            auto *st = reinterpret_cast<nvenc_h264_drv_pic_setup_s *>(base + EncSetupOff);
            std::memset(base + EncSetupOff, 0, EncQOff + 192 - EncSetupOff);

            st->magic = NV_NVENC_DRV_MAGIC_VALUE;
            FillEncSurface(std::addressof(st->input_cfg),     NvEncW, NvEncH, false);
            FillEncSurface(std::addressof(st->refpic_cfg),    NvEncW, NvEncH, true);
            FillEncSurface(std::addressof(st->outputpic_cfg), NvEncW, NvEncH, true);
            FillEncSurface(std::addressof(st->half_scaled_outputpic_cfg), NvEncW / 2, NvEncH / 2, true);

            st->sps_data.profile_idc               = 66;
            st->sps_data.level_idc                 = 42;
            st->sps_data.chroma_format_idc         = 1;
            st->sps_data.pic_order_cnt_type        = 2;
            st->sps_data.log2_max_frame_num_minus4 = 0;
            st->sps_data.frame_mbs_only            = 1;

            st->pps_data.pic_param_set_id                        = 0;
            st->pps_data.entropy_coding_mode_flag                = 0;
            st->pps_data.num_ref_idx_l0_active_minus1            = 0;
            st->pps_data.num_ref_idx_l1_active_minus1            = 0;
            st->pps_data.pic_init_qp_minus26                     = 0;
            st->pps_data.deblocking_filter_control_present_flag  = 1;

            auto *rc = std::addressof(st->rate_control);
            rc->hrd_type  = 2;
            rc->QP[0] = rc->QP[1] = rc->QP[2] = 26;
            rc->minQP[0] = rc->minQP[1] = rc->minQP[2] = 10;
            rc->maxQP[0] = rc->maxQP[1] = rc->maxQP[2] = 51;
            rc->framerate  = 60;
            rc->gop_length = 0xFFFFFFFFu;
            rc->rhopbi[0] = rc->rhopbi[1] = rc->rhopbi[2] = 256;

            auto *pc = std::addressof(st->pic_control);
            pc->pic_struct            = 0;
            pc->pic_type              = 3;
            pc->ref_pic_flag          = 1;
            pc->slice_mode            = 1;
            pc->codec                 = nvenc::CONTROL_CODEC_H264;
            pc->frame_num             = 0;
            pc->pic_order_cnt_lsb     = 0;
            pc->idr_pic_id            = 0;
            pc->num_forced_slices_minus1 = 0;
            pc->num_me_controls_minus1   = 0;
            pc->num_md_controls_minus1   = 0;
            pc->num_q_controls_minus1    = 0;
            pc->slice_control_offset  = EncSliceOff - EncSetupOff;
            pc->me_control_offset     = EncMeOff    - EncSetupOff;
            pc->md_control_offset     = EncMdOff    - EncSetupOff;
            pc->q_control_offset      = EncQOff     - EncSetupOff;
            pc->hist_buf_size         = EncHistSize;
            pc->bitstream_buf_size    = EncBitsSize;
            pc->bitstream_start_pos   = 0;
            pc->max_slice_size        = EncBitsSize;

            auto *sl = reinterpret_cast<nvenc_h264_slice_control_s *>(base + EncSliceOff);
            std::memset(sl, 0, sizeof(*sl));
            sl->num_mb       = (NvEncW / 16) * (NvEncH / 16);
            sl->qp_avr       = 26;
            sl->qp_slice_min = 10;
            sl->qp_slice_max = 51;
            sl->force_intra  = 1;

            auto *me = reinterpret_cast<nvenc_h264_me_control_s *>(base + EncMeOff);
            std::memset(me, 0, sizeof(*me));
            me->refinement_mode = 1;

            auto *md = reinterpret_cast<nvenc_h264_md_control_s *>(base + EncMdOff);
            std::memset(md, 0, sizeof(*md));
            md->intra_luma4x4_mode_enable   = 0x1FF;
            md->intra_luma8x8_mode_enable   = 0x1FF;
            md->intra_luma16x16_mode_enable = 0xF;
            md->intra_chroma_mode_enable    = 0xF;

            auto *q = reinterpret_cast<nvenc_h264_quant_control_s *>(base + EncQOff);
            std::memset(q, 0, sizeof(*q));
            q->qpp_mode = 0;
        }

        void TryNvencProbe(u32 nvmap_fd, u32 cmd_handle, u32 dst_handle) {
            if (g_engine_wedged) { LogLine("   nvenc: skipped - an engine job is stuck this boot"); return; }
            if (g_clk_armed) { ClocksHoldForEngines("nvenc"); }
            VicStage("nvp:1_open");
            u32 efd = 0, nverr = 0;
            if (R_FAILED(NvOpen("/dev/nvhost-msenc", std::addressof(efd), std::addressof(nverr))) || nverr != 0) {
                LogLine("   nvenc-probe: /dev/nvhost-msenc open failed nverr=%u", nverr);
                return;
            }
            LogLine("   ---- NVENC LADDER (magic 0x%08x, %ux%u) ----",
                    static_cast<unsigned>(NV_NVENC_DRV_MAGIC_VALUE), NvEncW, NvEncH);

            u32 esyncpt = 0;
            {
                struct { u32 module_id; u32 syncpt; } gs = { 0, 0 };
                nverr = 0;
                NvIoctl(efd, NvHostIocChannelGetSyncpoint, std::addressof(gs), sizeof(gs), std::addressof(nverr));
                esyncpt = gs.syncpt;
            }
            { struct { u32 fd; } sn = { nvmap_fd }; nverr = 0;
              NvIoctl(efd, NvHostIocChannelSetNvmapFd, std::addressof(sn), sizeof(sn), std::addressof(nverr)); }
            { struct { u32 ms; } to = { 1000 }; nverr = 0;
              NvIoctl(efd, NvHostIocChannelSetSubmitTo, std::addressof(to), sizeof(to), std::addressof(nverr)); }

            VicStage("nvp:2_pin");
            u32 buf_addr = 0, cmd_addr = 0;
            MapCmdBuffer(efd, dst_handle, std::addressof(buf_addr), "nvenc-buf", 0);
            MapCmdBuffer(efd, cmd_handle, std::addressof(cmd_addr), "nvenc-cmd", 0);
            LogLine("   syncpt=%u (the VIC uses 12) buf=0x%08x cmd=0x%08x", esyncpt, buf_addr, cmd_addr);
            if (buf_addr == 0) { LogLine("   buffer pin failed - refusing to submit"); NvClose(efd); return; }

            std::memset(g_vic_dst_buf + EncInYOff,  0x80, NvEncW * NvEncH);
            std::memset(g_vic_dst_buf + EncInUVOff, 0x80, NvEncW * NvEncH / 2);
            std::memset(g_vic_dst_buf + EncRefYOff, 0x00, EncEnd - EncRefYOff);
            std::memset(g_vic_dst_buf + EncHistOff, 0x00, EncHistSize);

            auto *stat = reinterpret_cast<nvenc_pic_stat_s *>(g_vic_dst_buf + EncStatOff);

            struct Level { const char *name; u32 depth; bool op_done; };
            const Level levels[] = {
                { "L0  bare INCR_SYNCPT (no class, no engine)", 0, false },
                { "L1  + SETCL class 0x21",                     1, false },
                { "L2  + SET_APPLICATION_ID",                   2, false },
                { "L3  + full surfaces + EXECUTE",              3, false },
                { "L4  same, INCR on OP_DONE",                  3, true  },
            };

            for (u32 li = 0; li < sizeof(levels) / sizeof(levels[0]); ++li) {
                const Level &L = levels[li];

                std::memset(g_vic_dst_buf + EncStatOff, 0, sizeof(nvenc_pic_stat_s));
                if (L.depth >= 3) {
                    FillEncSetup(g_vic_dst_buf);
                    std::memset(g_vic_dst_buf + EncBitsOff, 0, 0x1000);
                }
                armDCacheFlush(g_vic_dst_buf, EncEnd);

                VicStage("nvp:3_submit");
                auto *w = reinterpret_cast<u32 *>(g_vic_cmd_buf);
                u32 n = 0;
                auto m = [&](u32 method, u32 value) {
                    w[n++] = Host1xIncr0x10x2; w[n++] = method >> 2; w[n++] = value;
                };
                if (L.depth >= 1) {
                    w[n++] = vic::Host1xOpcodeSetClass(0, nvenc::HOST1X_CLASS_NVENC, 0);
                }
                if (L.depth >= 2) {
                    m(nvenc::SET_APPLICATION_ID, nvenc::APPLICATION_ID_H264);
                }
                if (L.depth >= 3) {
                    m(nvenc::SET_CONTROL_PARAMS,      nvenc::CONTROL_CODEC_H264);
                    m(nvenc::SET_PICTURE_INDEX,       0);
                    m(nvenc::SET_IN_DRV_PIC_SETUP,    (buf_addr + EncSetupOff) >> 8);
                    m(nvenc::SET_IN_CUR_PIC,          (buf_addr + EncInYOff)   >> 8);
                    m(nvenc::SET_IN_CUR_PIC_CHROMA_U, (buf_addr + EncInUVOff)  >> 8);
                    m(nvenc::SET_OUT_REF_PIC_LUMA,    (buf_addr + EncRefYOff)  >> 8);
                    m(nvenc::SET_OUT_REF_PIC_CHROMA,  (buf_addr + EncRefUVOff) >> 8);
                    m(nvenc::SET_IOHISTORY,           (buf_addr + EncHistOff)  >> 8);
                    m(nvenc::SET_OUT_ENC_STATUS,      (buf_addr + EncStatOff)  >> 8);
                    m(nvenc::SET_OUT_BITSTREAM,       (buf_addr + EncBitsOff)  >> 8);
                    m(nvenc::EXECUTE,                 1u << 8);
                }
                n = AppendIncrSyncpt(w, n, esyncpt, L.op_done);
                armDCacheFlush(g_vic_cmd_buf, VicCmdSize);

                alignas(8) u8 sb[16 + 12 + 20 + 4] = {};
                u32 off = 0;
                auto put = [&](u32 v) { std::memcpy(sb + off, std::addressof(v), 4); off += 4; };
                put(1); put(0); put(1); put(1);
                put(cmd_handle); put(0); put(n);
                put(esyncpt); put(1); put(0); put(0); put(0);
                const u32 fence_off = off; put(0);
                const u32 sz = off;
                const u32 req = (UINT32_C(3) << 30) | (sz << 16) | (0x00u << 8) | 0x01u;
                u32 fence_val = 0; nverr = 0;
                const auto rc = NvIoctl(efd, req, sb, sz, std::addressof(nverr));
                std::memcpy(std::addressof(fence_val), sb + fence_off, 4);

                if (R_FAILED(rc) || nverr != 0) {
                    LogLine("   %-44s SUBMIT REJECTED rc=0x%x nverr=%u", L.name, rc, nverr);
                    continue;
                }

                const u32 cfd = CtrlFd();
                bool done = false;
                u32 seen = 0;
                if (cfd != 0) {
                    struct { u32 id; u32 thresh; u32 timeout; } a = { esyncpt, fence_val, 200 };
                    u32 we = 0;
                    NvIoctl(cfd, NvHostIocCtrlSyncptWait, std::addressof(a), sizeof(a), std::addressof(we));
                    struct { u32 id; u32 value; } r = { esyncpt, 0 };
                    for (u32 spin = 0; spin < 400 && !done; ++spin) {
                        u32 e2 = 0; r.value = 0;
                        NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e2));
                        seen = r.value;
                        done = (seen >= fence_val);
                        if (!done) { os::SleepThread(TimeSpan::FromMicroSeconds(200)); }
                    }
                }
                armDCacheFlush(g_vic_dst_buf, EncEnd);

                if (L.depth < 3) {
                    LogLine("   %-44s words=%2u fence %u/%u  %s", L.name, n, seen, fence_val,
                            done ? "REACHED" : "*** STALLED - this layer is the problem ***");
                } else {
                    const u32 ucode = stat->ucode_error_status;
                    const u32 est   = stat->error_status;
                    u32 nz = 0;
                    for (u32 i = 0; i < 4096; ++i) { if (g_vic_dst_buf[EncBitsOff + i] != 0) { ++nz; } }
                    LogLine("   %-44s words=%2u fence %u/%u %s  err=%u ucode=0x%08x (%s)  bitstream %u/4096 B nonzero  bits=%u",
                            L.name, n, seen, fence_val, done ? "REACHED" : "STALLED",
                            est, ucode, nvenc::ErrName(ucode), nz, stat->total_bit_count);
                }
            }

            UnmapCmdBuffer(efd, dst_handle);
            NvClose(efd);
            VicStage("nvp:done");
        }

        bool NvjpgOneAttempt(u32 nvmap_fd, u32 cmd_handle, u32 dst_handle,
                             const NvjpgLayout &L, u32 memory_mode, u32 input_type,
                             u32 *out_scan_len, u32 *out_err) {
            *out_scan_len = 0; *out_err = 0xFFFFFFFFu;

            u32 jfd = 0, nverr = 0;
            if (R_FAILED(NvOpen("/dev/nvhost-nvjpg", std::addressof(jfd), std::addressof(nverr))) || nverr != 0) {
                LogLine("   nvjpg: /dev/nvhost-nvjpg open FAILED nverr=%u", nverr);
                return false;
            }

            u32 syncpt = 0;
            {
                struct { u32 module_id; u32 syncpt; } gs = { 0, 0 };
                nverr = 0;
                NvIoctl(jfd, NvHostIocChannelGetSyncpoint, std::addressof(gs), sizeof(gs), std::addressof(nverr));
                syncpt = gs.syncpt;
            }
            if (syncpt == 12) {
                LogLine("   nvjpg: syncpt is 12, the SAME one nvnflinger composites on - REFUSING");
                NvClose(jfd);
                return false;
            }
            { struct { u32 fd; } sn = { nvmap_fd }; nverr = 0;
              NvIoctl(jfd, NvHostIocChannelSetNvmapFd, std::addressof(sn), sizeof(sn), std::addressof(nverr)); }

            u32 buf_addr = 0, cmd_addr = 0;
            MapCmdBuffer(jfd, dst_handle, std::addressof(buf_addr), "nvjpg-buf", 0);
            MapCmdBuffer(jfd, cmd_handle, std::addressof(cmd_addr), "nvjpg-cmd", 0);
            if (buf_addr == 0) {
                LogLine("   nvjpg: buffer pin failed - refusing to submit");
                NvClose(jfd);
                return false;
            }

            const u32 a_param = buf_addr + L.param_off,  a_status = buf_addr + L.status_off;
            const u32 a_bits  = buf_addr + L.bits_off,   a_luma   = buf_addr + L.luma_off;
            const u32 a_chroma = buf_addr + L.chroma_off;
            const u32 a_chroma_v = (memory_mode == 1) ? (a_chroma + L.w * L.h / 4) : 0;
            if (a_param == 0 || a_status == 0 || a_bits == 0 || a_luma == 0 || a_chroma == 0 ||
                (memory_mode == 1 && a_chroma_v == 0)) {
                LogLine("   nvjpg: REFUSING to submit, a surface address is zero "
                        "(param=%#x status=%#x bits=%#x luma=%#x cu=%#x cv=%#x)",
                        a_param, a_status, a_bits, a_luma, a_chroma, a_chroma_v);
                UnmapCmdBuffer(jfd, dst_handle);
                NvClose(jfd);
                return false;
            }

            NvjpgFillParams(g_vic_dst_buf, L, memory_mode, input_type);
            std::memset(g_vic_dst_buf + L.status_off, 0, sizeof(nvjpg_enc_status));
            std::memset(g_vic_dst_buf + L.bits_off, 0, 0x1000);
            armDCacheFlush(g_vic_dst_buf, VicDstSize);

            auto *w = reinterpret_cast<u32 *>(g_vic_cmd_buf);
            u32 n = NvjpgBuildCmdbuf(w, a_param, a_status, a_bits, a_luma, a_chroma, a_chroma_v);
            n = AppendIncrSyncpt(w, n, syncpt, true);
            armDCacheFlush(g_vic_cmd_buf, VicCmdSize);

            alignas(8) u8 sb[16 + 12 + 20 + 4] = {};
            u32 off = 0;
            auto put = [&](u32 v) { std::memcpy(sb + off, std::addressof(v), 4); off += 4; };
            put(1); put(0); put(1); put(1);
            put(cmd_handle); put(0); put(n);
            put(syncpt); put(1); put(0); put(0); put(0);
            const u32 fence_off = off; put(0);
            const u32 sz = off;
            const u32 req = (UINT32_C(3) << 30) | (sz << 16) | (0x00u << 8) | 0x01u;
            u32 fence_val = 0; nverr = 0;
            const auto rc = NvIoctl(jfd, req, sb, sz, std::addressof(nverr));
            std::memcpy(std::addressof(fence_val), sb + fence_off, 4);

            bool done = false;
            u32 seen = 0;
            if (R_SUCCEEDED(rc) && nverr == 0) {
                const u32 cfd = CtrlFd();
                if (cfd != 0) {
                    struct { u32 id; u32 thresh; u32 timeout; } a = { syncpt, fence_val, 300 };
                    u32 we = 0;
                    NvIoctl(cfd, NvHostIocCtrlSyncptWait, std::addressof(a), sizeof(a), std::addressof(we));
                    struct { u32 id; u32 value; } r = { syncpt, 0 };
                    for (u32 spin = 0; spin < 400 && !done; ++spin) {
                        u32 e2 = 0; r.value = 0;
                        NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e2));
                        seen = r.value;
                        done = (seen >= fence_val);
                        if (!done) { os::SleepThread(TimeSpan::FromMicroSeconds(200)); }
                    }
                }
            }
            armDCacheFlush(g_vic_dst_buf, VicDstSize);

            const auto *st = reinterpret_cast<const nvjpg_enc_status *>(g_vic_dst_buf + L.status_off);
            *out_err      = st->error_status;
            *out_scan_len = st->bitstream_size;

            LogLine("   nvjpg mem_mode=%u input_type=%u words=%u rc=0x%x nverr=%u fence %u/%u %s"
                    "  err=%u bytes=%u mcu=%ux%u cycles=%u",
                    memory_mode, input_type, n, rc, nverr, seen, fence_val,
                    done ? "REACHED" : "STALLED",
                    st->error_status, st->bitstream_size, st->mcu_x, st->mcu_y, st->cycle_count);

            UnmapCmdBuffer(jfd, dst_handle);
            NvClose(jfd);
            return done && st->error_status == 0 && st->bitstream_size > 0;
        }

        void TryNvjpgProbe(u32 nvmap_fd, u32 cmd_handle, u32 dst_handle) {
            if (g_engine_wedged) { LogLine("   nvjpg-enc: skipped - an engine job is stuck this boot"); return; }
            if (g_clk_armed) { ClocksHoldForEngines("nvjpg-enc"); }
            VicStage("jpg:1");

            NvjpgLayout L = {};
            L.param_off  = 0x000000;
            L.status_off = 0x002000;
            L.bits_off   = 0x003000;
            L.bits_size  = 0x080000;
            L.luma_off   = 0x090000;
            L.w = 1280; L.h = 720;
            L.chroma_off = L.luma_off + L.w * L.h;
            L.quality    = g_nvjpg_quality;
            static_assert(0x090000 + 1280 * 720 * 3 / 2 <= VicDstSize, "nvjpg probe surfaces must fit");

            for (u32 y = 0; y < L.h; ++y) {
                u8 *row = g_vic_dst_buf + L.luma_off + y * L.w;
                for (u32 x = 0; x < L.w; ++x) {
                    row[x] = static_cast<u8>(((x * 255) / L.w) ^ ((y < L.h / 2) ? 0 : 0xFF));
                }
            }
            for (u32 y = 0; y < L.h / 2; ++y) {
                u8 *row = g_vic_dst_buf + L.chroma_off + y * L.w;
                for (u32 x = 0; x < L.w / 2; ++x) {
                    row[2 * x + 0] = static_cast<u8>((x * 255) / (L.w / 2));
                    row[2 * x + 1] = static_cast<u8>((y * 255) / (L.h / 2));
                }
            }

            auto wrap_and_save = [&](u32 scan_len, u32 attempt) {
                if (scan_len == 0 || scan_len > L.bits_size) { return; }
                u8 *out = g_vic_dst_buf + L.chroma_off + L.w * L.h / 2;
                const u32 avail = VicDstSize - static_cast<u32>(out - g_vic_dst_buf);
                if (scan_len + 0x400 > avail) { return; }
                const u32 total = NvjpgWriteContainer(out, L.w, L.h, L.quality,
                                                      g_vic_dst_buf + L.bits_off, scan_len);
                char path[64];
                std::snprintf(path, sizeof(path), "sdmc:/nvjpg-%03u.jpg", attempt);
                if (WriteEngineOutputToSd(path, out, total, nullptr)) {
                    LogLine("   *** NVJPG ENCODED %u BYTES -> %s (%u B file) ***", scan_len, path, total);
                }
            };

            const u32 attempts = g_nvjpg_attempts;
            LogLine("   ---- NVJPG POWER-GATING TEST (quality %u, %u attempts, ~1/s) ----",
                    g_nvjpg_quality, attempts);
            LogLine("   TAKE SCREENSHOTS NOW - press Capture repeatedly for the next ~%u seconds.", attempts);
            LogLine("   The system encodes every screenshot with this engine; if one of our jobs");
            LogLine("   completes in the same second, power gating is confirmed.");

            u32 n_ok = 0, n_fail = 0, first_ok = 0;
            for (u32 i = 0; i < attempts; ++i) {
                u32 scan_len = 0, err = 0;
                const bool ok = NvjpgOneAttempt(nvmap_fd, cmd_handle, dst_handle, L,
                                                0 ,
                                                0 ,
                                                std::addressof(scan_len), std::addressof(err));
                if (ok) {
                    if (n_ok == 0) { first_ok = i; }
                    ++n_ok;
                    wrap_and_save(scan_len, i);
                    if (n_ok >= 3) { break; }
                } else {
                    ++n_fail;
                }
                os::SleepThread(TimeSpan::FromMilliSeconds(1000));
            }

            LogLine("   ---- GATING TEST RESULT: %u completed, %u failed%s ----",
                    n_ok, n_fail,
                    n_ok > 0 ? "  *** THE ENGINE CAN BE MADE TO RUN ***"
                             : "  (no attempt ran - power gating is NOT the explanation)");
            if (n_ok > 0) {
                LogLine("   first success was attempt #%u - correlate that second with when "
                        "a screenshot was taken", first_ok);
            }
            VicStage("jpg:done");
        }

        void TryNvjpgDecodeControl(u32 nvmap_fd, u32 cmd_handle, u32 dst_handle) {
            namespace ctl = nvjpg_dec_control;
            VicStage("jd:1");
            LogLine("   ---- NVJPG DECODE POSITIVE CONTROL (M77: clock ensured after open, read at submit) ----");
            if (g_engine_wedged) { LogLine("   an earlier engine job never completed - not submitting"); return; }
            ON_SCOPE_EXIT { ClockWatchStop(); };

            ClockSurvey("jpgdec-pre");

            constexpr u32 PicOff = 0x000000, StatOff = 0x001000, ScanOff = 0x002000, OutOff = 0x010000;
            constexpr u32 OutSize = ctl::Pitch * ctl::Height;
            static_assert(sizeof(ctl::PictureInfo) <= StatOff - PicOff);
            static_assert(sizeof(ctl::ScanData) <= OutOff - ScanOff);
            static_assert(OutOff + OutSize <= VicDstSize);

            u32 jfd = 0, nverr = 0;
            if (R_FAILED(NvOpen("/dev/nvhost-nvjpg", std::addressof(jfd), std::addressof(nverr))) || nverr != 0) {
                LogLine("   jpgdec: /dev/nvhost-nvjpg open FAILED nverr=%u", nverr);
                VicStage("jd:open_FAILED");
                return;
            }
            u32 syncpt = 0;
            {
                struct { u32 module_id; u32 syncpt; } gs = { 0, 0 };
                nverr = 0;
                NvIoctl(jfd, NvHostIocChannelGetSyncpoint, std::addressof(gs), sizeof(gs), std::addressof(nverr));
                syncpt = gs.syncpt;
            }
            if (syncpt == 0 || syncpt == 12) {
                LogLine("   jpgdec: syncpt=%u (%s) - REFUSING", syncpt, syncpt == 12 ? "nvnflinger's" : "none");
                NvClose(jfd);
                VicStage("jd:bad_syncpt");
                return;
            }
            { struct { u32 fd; } sn = { nvmap_fd }; nverr = 0;
              NvIoctl(jfd, NvHostIocChannelSetNvmapFd, std::addressof(sn), sizeof(sn), std::addressof(nverr)); }

            u32 buf_addr = 0, cmd_addr = 0;
            MapCmdBuffer(jfd, dst_handle, std::addressof(buf_addr), "jpgdec-buf", 0);
            MapCmdBuffer(jfd, cmd_handle, std::addressof(cmd_addr), "jpgdec-cmd", 0);
            if (buf_addr == 0 || cmd_addr == 0) {
                LogLine("   jpgdec: pin failed (buf=%#x cmd=%#x) - refusing to submit", buf_addr, cmd_addr);
                if (buf_addr != 0) { UnmapCmdBuffer(jfd, dst_handle); }
                if (cmd_addr != 0) { UnmapCmdBuffer(jfd, cmd_handle); }
                NvClose(jfd);
                VicStage("jd:pin_FAILED");
                return;
            }
            const u32 a_pic = buf_addr + PicOff, a_stat = buf_addr + StatOff;
            const u32 a_scan = buf_addr + ScanOff, a_out = buf_addr + OutOff;

            std::memset(g_vic_dst_buf, 0, OutOff);
            std::memcpy(g_vic_dst_buf + PicOff,  ctl::PictureInfo, sizeof(ctl::PictureInfo));
            std::memcpy(g_vic_dst_buf + ScanOff, ctl::ScanData,    sizeof(ctl::ScanData));
            std::memset(g_vic_dst_buf + OutOff, 0xAB, OutSize);
            armDCacheFlush(g_vic_dst_buf, OutOff + OutSize);

            auto *w = reinterpret_cast<u32 *>(g_vic_cmd_buf);
            u32 n = 0;
            auto m = [&](u32 method, u32 value) {
                w[n++] = vic::Host1xOpcodeIncr(vic::UCLASS_METHOD_OFFSET, 2);
                w[n++] = method >> 2;
                w[n++] = value;
            };
            w[n++] = vic::Host1xOpcodeSetClass(0, 0xC0, 0);
            m(0x200, 1);
            m(0x708, a_pic  >> 8);
            m(0x70C, a_stat >> 8);
            m(0x710, a_scan >> 8);
            m(0x714, a_out  >> 8);
            m(0x300, 0x100);
            n = AppendIncrSyncpt(w, n, syncpt, true);
            armDCacheFlush(g_vic_cmd_buf, VicCmdSize);
            LogLine("   jpgdec: syncpt=%u pic=%#x stat=%#x scan=%#x (%zu B) out=%#x, %u words",
                    syncpt, a_pic, a_stat, a_scan, sizeof(ctl::ScanData), a_out, n);

            alignas(8) u8 sb[16 + 12 + 20 + 4] = {};
            u32 off = 0;
            auto put = [&](u32 v) { std::memcpy(sb + off, std::addressof(v), 4); off += 4; };
            put(1); put(0); put(1); put(1);
            put(cmd_handle); put(0); put(n);
            put(syncpt); put(1); put(0); put(0); put(0);
            const u32 fence_off = off; put(0);
            const u32 sz = off;
            const u32 req = (UINT32_C(3) << 30) | (sz << 16) | (0x00u << 8) | 0x01u;

            LogMark("jd:clock");
            const u32 ensured_hz = ClockEnsure(PcvModule_NVJPG, "jpgdec");
            LogMark("jd:submit");
            u32 jpg_hz = 0;
            const bool readable = ClockRateOf(PcvModule_NVJPG, std::addressof(jpg_hz));
            const u64 t_read = armTicksToNs(armGetSystemTick());
            if (ensured_hz == 0 || !readable || jpg_hz == 0) {
                LogLine("   jpgdec: NOT submitting - ClockEnsure=%u Hz, final read %s %u Hz. An engine that",
                        ensured_hz, readable ? "=" : "UNREADABLE", jpg_hz);
                LogLine("   cannot run is what wedged M74. The clk-ensure lines above are this run's result.");
                UnmapCmdBuffer(jfd, dst_handle);
                UnmapCmdBuffer(jfd, cmd_handle);
                NvClose(jfd);
                VicStage("jd:no_clock");
                return;
            }

            u32 fence_val = 0; nverr = 0;
            const auto rc = NvIoctl(jfd, req, sb, sz, std::addressof(nverr));
            const u64 t_submit = armTicksToNs(armGetSystemTick());
            std::memcpy(std::addressof(fence_val), sb + fence_off, 4);
            LogMark("jd:submitted");

            bool done = false;
            u32 seen = 0;
            const u64 t0 = armTicksToNs(armGetSystemTick());
            if (R_SUCCEEDED(rc) && nverr == 0) {
                const u32 cfd = CtrlFd();
                if (cfd != 0) {
                    struct { u32 id; u32 thresh; u32 timeout; } a = { syncpt, fence_val, 300 };
                    u32 we = 0;
                    NvIoctl(cfd, NvHostIocCtrlSyncptWait, std::addressof(a), sizeof(a), std::addressof(we));
                    struct { u32 id; u32 value; } r = { syncpt, 0 };
                    for (u32 spin = 0; spin < 400 && !done; ++spin) {
                        u32 e2 = 0; r.value = 0;
                        NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e2));
                        seen = r.value;
                        done = (seen >= fence_val);
                        if (!done) { os::SleepThread(TimeSpan::FromMicroSeconds(200)); }
                    }
                }
            }
            const u64 us = (armTicksToNs(armGetSystemTick()) - t0) / 1000;
            armDCacheFlush(g_vic_dst_buf, OutOff + OutSize);

            struct Status { u32 used_bytes, mcu_x, mcu_y, reserved, result, pad[3]; };
            static_assert(sizeof(Status) == 0x20);
            Status st;
            std::memcpy(std::addressof(st), g_vic_dst_buf + StatOff, sizeof(st));

            u32 written = 0;
            for (u32 i = 0; i < OutSize; ++i) { if (g_vic_dst_buf[OutOff + i] != 0xAB) { ++written; } }
            u32 after_hz = 0;
            const bool after_ok = ClockRateOf(PcvModule_NVJPG, std::addressof(after_hz));
            LogLine("   jpgdec: submit rc=0x%x nverr=%u fence %u/%u %s after %llu us | NVJPG %u Hz read %llu us before submit returned, %u Hz%s after"
                    " | status used=%u mcu=%ux%u result=%u | %u/%u output bytes written",
                    rc, nverr, seen, fence_val, done ? "REACHED" : "STALLED",
                    static_cast<unsigned long long>(us), jpg_hz,
                    static_cast<unsigned long long>((t_submit - t_read) / 1000), after_hz, after_ok ? "" : " (unreadable)",
                    st.used_bytes, st.mcu_x, st.mcu_y, st.result, written, OutSize);

            if (done) {
                u32 worst = 0, worst_block = 0;
                for (u32 by = 0; by < ctl::Height / 8; ++by) {
                    for (u32 bx = 0; bx < ctl::Width / 8; ++bx) {
                        for (u32 ch = 0; ch < 3; ++ch) {
                            u32 sum = 0;
                            for (u32 y = by * 8; y < by * 8 + 8; ++y) {
                                for (u32 x = bx * 8; x < bx * 8 + 8; ++x) {
                                    sum += g_vic_dst_buf[OutOff + y * ctl::Pitch + x * 4 + ch];
                                }
                            }
                            const u32 mean = (sum + 32) / 64;
                            const u32 idx = (by * (ctl::Width / 8) + bx) * 3 + ch;
                            const u32 want = ctl::ExpectedBlockMeans[idx];
                            const u32 d = mean > want ? mean - want : want - mean;
                            if (d > worst) { worst = d; worst_block = idx / 3; }
                        }
                    }
                }
                const u8 *p0 = g_vic_dst_buf + OutOff;
                LogLine("   jpgdec: px(0,0)=%02x %02x %02x %02x  px(63,0)=%02x %02x %02x %02x  px(0,63)=%02x %02x %02x %02x",
                        p0[0], p0[1], p0[2], p0[3],
                        p0[63 * 4], p0[63 * 4 + 1], p0[63 * 4 + 2], p0[63 * 4 + 3],
                        p0[63 * ctl::Pitch], p0[63 * ctl::Pitch + 1], p0[63 * ctl::Pitch + 2], p0[63 * ctl::Pitch + 3]);
                LogLine("   jpgdec: worst 8x8 block-mean deviation %u (block %u)  ->  %s", worst, worst_block,
                        worst <= 6 ? "*** NVJPG RAN AND DECODED CORRECTLY - this process can drive a non-VIC engine ***"
                                   : "engine ran, output differs - check nvjpg-dec.rgba on the PC");

                static_assert(OutSize <= 0x10000);
                u32 fnv = 0;
                WriteEngineOutputToSd("sdmc:/nvjpg-dec.rgba", g_vic_dst_buf + OutOff, OutSize, std::addressof(fnv));
                UnmapCmdBuffer(jfd, dst_handle);
                UnmapCmdBuffer(jfd, cmd_handle);
                NvClose(jfd);
                VicStage(worst <= 6 ? "jd:MATCH" : "jd:ran_mismatch");
            } else if (!(R_SUCCEEDED(rc) && nverr == 0)) {
                LogLine("   jpgdec: submit REJECTED (rc=0x%x nverr=%u) - nothing reached the engine", rc, nverr);
                UnmapCmdBuffer(jfd, dst_handle);
                UnmapCmdBuffer(jfd, cmd_handle);
                NvClose(jfd);
                VicStage("jd:submit_rejected");
            } else {
                g_engine_wedged = true;
                LogLine("   jpgdec: STALLED with NVJPG clocked at %u Hz. Leaving the channel OPEN on purpose", jpg_hz);
                LogLine("   (M74: tearing down a channel with a stuck job took the compositor with it).");
                LogLine("   No further engine work this boot. Reboot the console when convenient.");
                ClockSurvey("jpgdec-stall");
                VicStage("jd:STALLED_left_open");
            }
        }

        constexpr u32 NvgrcPictureIndex = 0x4D383000u;

        constexpr u32 SetupRc = __builtin_offsetof(nvenc_h264_drv_pic_setup_s, rate_control);
        constexpr u32 SetupRcHrdType = SetupRc + __builtin_offsetof(nvenc_h264_rc_s, hrd_type);
        constexpr u32 SetupRcTwoPass = SetupRc + __builtin_offsetof(nvenc_h264_rc_s, two_pass_rc);
        static_assert(SetupRcHrdType == 0x70 && SetupRcTwoPass == 0xBA, "nvsetup-dump reads these offsets");
        static_assert(nvenc_grc_idr::Setup[SetupRcHrdType] == 1 && nvenc_grc_idr::Setup[SetupRcTwoPass] == 1,
                      "grc's IDR setup has VCL HRD and two_pass_rc 1");
        struct NvgrcVariant { char tag; u32 control; bool noise; u32 patch_off; u8 patch_val; const char *what; };
        constexpr NvgrcVariant NvgrcVariants[] = {
            { 'a', 0x12001103u, false, 0, 0, "grc's job, flat stripes (Run F)" },
            { 'b', 0x12001103u, true,  0, 0, "grc's job, stripes + balanced noise" },
            { 'c', 0x00001103u, false, 0, 0, "RCMODE 0 (constant QP), flat stripes" },
            { 'd', 0x12001103u, false, SetupRcHrdType, 0, "grc's job, setup rc hrd_type 0 (no HRD), flat stripes" },
            { 'e', 0x12001103u, false, SetupRcTwoPass, 0, "grc's job, setup two_pass_rc 0, flat stripes" },
        };

        void TryNvencGrcReplay(u32 nvmap_fd, u32 cmd_handle) {
            namespace idr = nvenc_grc_idr;
            VicStage("ng:1");
            LogLine("   ---- NVENC: REPLAY OF GRC'S IDR JOB (M81: five variants) ----");
            if (g_engine_wedged) { LogLine("   an earlier engine job never completed - not submitting"); return; }
            ON_SCOPE_EXIT { ClockWatchStop(); };

            constexpr u32 LumaAlloc = idr::Width * ((idr::Height + 31) / 32 * 32);
            constexpr u32 ChromaAlloc = idr::Width * ((idr::Height / 2 + 31) / 32 * 32);
            constexpr u32 OffSetup  = 0x000000;
            constexpr u32 OffStatus = 0x003000;
            constexpr u32 OffRc     = OffStatus + 0x20000;
            constexpr u32 OffBits   = OffRc + 0x20000;      constexpr u32 BitsSize = 0x160000;
            constexpr u32 OffHist   = OffBits + BitsSize;   constexpr u32 HistSize = 0x0C0000;
            constexpr u32 OffCur    = OffHist + HistSize;
            constexpr u32 OffCurUV  = OffCur + LumaAlloc;
            constexpr u32 RefSize   = 0x160000;
            constexpr u32 OffRefOut = (OffCurUV + ChromaAlloc + 0xFFF) & ~0xFFFu;
            constexpr u32 ArenaSize = OffRefOut + RefSize;
            constexpr u32 ArenaOff  = 0x10000;
            constexpr u32 ReconY    = idr::Width * idr::Height;
            constexpr u32 BandBytes = idr::Width * 32;
            static_assert(sizeof(idr::Setup) <= OffStatus - OffSetup);
            static_assert(ArenaOff + ArenaSize <= FbBlockRowStage, "the arena must stay clear of the stage buffers");
            static_assert(ReconY + idr::Width * idr::Height / 2 <= RefSize);
            static_assert(HistSize >= 706560 && BitsSize >= 1382400, "grc's setup names these sizes");
            static_assert(BandBytes % 2 == 0);
            if (g_ind_buf == nullptr || g_ind_size < FbBlockRowStage + 2 * ((ReconY + 0xFFF) & ~0xFFFu)) {
                LogLine("   nvgrc: heap too small (capture region %zu KB) - not running", g_ind_size / 1024);
                return;
            }
            u8 *const a = g_ind_buf + ArenaOff;
            std::memset(a, 0, ArenaSize);

            u32 ah = 0, aid = 0;
            if (R_FAILED(NvmapOwn(nvmap_fd, a, ArenaSize, 0, std::addressof(ah), std::addressof(aid), true))) {
                LogLine("   nvgrc: nvmap of the %u KB arena failed - not running", ArenaSize / 1024);
                VicStage("ng:nvmap_FAILED");
                return;
            }

            u32 efd = 0, nverr = 0;
            if (R_FAILED(NvOpen("/dev/nvhost-msenc", std::addressof(efd), std::addressof(nverr))) || nverr != 0) {
                LogLine("   nvgrc: /dev/nvhost-msenc open FAILED nverr=%u", nverr);
                VicStage("ng:open_FAILED");
                return;
            }
            u32 syncpt = 0;
            {
                struct { u32 module_id; u32 syncpt; } gs = { 0, 0 };
                nverr = 0;
                NvIoctl(efd, NvHostIocChannelGetSyncpoint, std::addressof(gs), sizeof(gs), std::addressof(nverr));
                syncpt = gs.syncpt;
            }
            if (syncpt == 0 || syncpt == 12) {
                LogLine("   nvgrc: syncpt=%u - REFUSING", syncpt);
                NvClose(efd);
                VicStage("ng:bad_syncpt");
                return;
            }
            { struct { u32 fd; } sn = { nvmap_fd }; nverr = 0;
              NvIoctl(efd, NvHostIocChannelSetNvmapFd, std::addressof(sn), sizeof(sn), std::addressof(nverr)); }

            u32 base = 0, cmd_addr = 0;
            MapCmdBuffer(efd, ah, std::addressof(base), "nvgrc-arena", 0);
            MapCmdBuffer(efd, cmd_handle, std::addressof(cmd_addr), "nvgrc-cmd", 0);
            auto close_all = [&]() {
                if (base != 0) { UnmapCmdBuffer(efd, ah); }
                if (cmd_addr != 0) { UnmapCmdBuffer(efd, cmd_handle); }
                NvClose(efd);
            };
            if (base == 0 || cmd_addr == 0) {
                LogLine("   nvgrc: pin failed (arena=%#x cmd=%#x) - refusing to submit", base, cmd_addr);
                close_all();
                VicStage("ng:pin_FAILED");
                return;
            }
            LogLine("   nvgrc: syncpt=%u arena=%#x (%u KB) setup=%#x status=%#x bits=%#x cur=%#x/%#x ref out=%#x",
                    syncpt, base, ArenaSize / 1024, base + OffSetup, base + OffStatus, base + OffBits,
                    base + OffCur, base + OffCurUV, base + OffRefOut);

            u32 n_encoded = 0;
            for (u32 vi = 0; vi < sizeof(NvgrcVariants) / sizeof(NvgrcVariants[0]); ++vi) {
                const NvgrcVariant &v = NvgrcVariants[vi];
                const u32 pic_index = NvgrcPictureIndex + vi;
                LogLine("   nvgrc[%c]: %s, SET_CONTROL_PARAMS %#010x, picture index %#x%s", v.tag, v.what, v.control, pic_index,
                        v.patch_off != 0 ? ", setup patched" : "");

                std::memset(a + OffStatus, 0, OffCur - OffStatus);
                std::memset(a + OffRefOut, 0, RefSize);
                std::memcpy(a + OffSetup, idr::Setup, sizeof(idr::Setup));
                if (v.patch_off != 0) {
                    LogLine("   nvgrc[%c]: setup byte %#x: %u -> %u", v.tag, v.patch_off, a[OffSetup + v.patch_off], v.patch_val);
                    a[OffSetup + v.patch_off] = v.patch_val;
                }
                u32 lcg = 0x4D383100u + vi;
                for (u32 k = 0; k < LumaAlloc / BandBytes; ++k) {
                    const s32 y = static_cast<s32>(32 + 8 * k > 235 ? 235 : 32 + 8 * k);
                    u8 *band = a + OffCur + k * BandBytes;
                    if (!v.noise) {
                        std::memset(band, y, BandBytes);
                        continue;
                    }
                    for (u32 i = 0; i < BandBytes / 2; ++i) {
                        lcg = lcg * 1664525u + 1013904223u;
                        const s32 d = static_cast<s32>((lcg >> 16) % 49u) - 24;
                        band[i] = static_cast<u8>(y + d);
                        band[BandBytes / 2 + i] = static_cast<u8>(y - d);
                    }
                }
                std::memset(a + OffCurUV, 128, ChromaAlloc);
                std::memset(a + OffStatus, 0xA5, sizeof(nvenc_pic_stat_s));
                armDCacheFlush(a, ArenaSize);

                auto *w = reinterpret_cast<u32 *>(g_vic_cmd_buf);
                u32 n = 0;
                auto m = [&](u32 method, u32 value) {
                    w[n++] = vic::Host1xOpcodeIncr(vic::UCLASS_METHOD_OFFSET, 2);
                    w[n++] = method >> 2;
                    w[n++] = value;
                };
                w[n++] = vic::Host1xOpcodeSetClass(0, 0x21, 0);
                w[n++] = vic::Host1xOpcodeIncr(0x0B, 1);
                w[n++] = 0;
                m(0x700, v.control);
                m(0x704, pic_index);
                m(0x200, 1);
                m(0x710, (base + OffSetup)  >> 8);
                m(0x718, (base + OffStatus) >> 8);
                m(0x724, (base + OffRc)     >> 8);
                m(0x71C, (base + OffBits)   >> 8);
                m(0x720, (base + OffHist)   >> 8);
                m(0x734, (base + OffCur)    >> 8);
                m(0x740, (base + OffCurUV)  >> 8);
                m(0x730, (base + OffRefOut) >> 8);
                m(0x300, 0x100);
                n = AppendIncrSyncpt(w, n, syncpt, true);
                armDCacheFlush(g_vic_cmd_buf, VicCmdSize);

                alignas(8) u8 sb[16 + 12 + 20 + 4] = {};
                u32 off = 0;
                auto put = [&](u32 x) { std::memcpy(sb + off, std::addressof(x), 4); off += 4; };
                put(1); put(0); put(1); put(1);
                put(cmd_handle); put(0); put(n);
                put(syncpt); put(1); put(0); put(0); put(0);
                const u32 fence_off = off; put(0);
                const u32 sz = off;
                const u32 req = (UINT32_C(3) << 30) | (sz << 16) | (0x00u << 8) | 0x01u;

                const u32 ensured_hz = ClockEnsure(PcvModule_NVENC, "nvgrc");
                LogMark("ng:submit");
                u32 enc_hz = 0;
                const bool readable = ClockRateOf(PcvModule_NVENC, std::addressof(enc_hz));
                const u64 t_read = armTicksToNs(armGetSystemTick());
                if (ensured_hz == 0 || !readable || enc_hz == 0) {
                    LogLine("   nvgrc[%c]: NOT submitting - ClockEnsure=%u Hz, final read %s %u Hz", v.tag, ensured_hz,
                            readable ? "=" : "UNREADABLE", enc_hz);
                    close_all();
                    VicStage("ng:no_clock");
                    return;
                }

                u32 fence_val = 0; nverr = 0;
                const auto rc = NvIoctl(efd, req, sb, sz, std::addressof(nverr));
                const u64 t_submit = armTicksToNs(armGetSystemTick());
                std::memcpy(std::addressof(fence_val), sb + fence_off, 4);
                const bool submitted = R_SUCCEEDED(rc) && nverr == 0;

                bool fence_done = false, status_ours = false;
                u32 seen = 0;
                nvenc_pic_stat_s st = {};
                u64 t_status = 0;
                if (submitted) {
                    const u32 cfd = CtrlFd();
                    while (armTicksToNs(armGetSystemTick()) - t_submit < UINT64_C(1000000000) && !(fence_done && status_ours)) {
                        if (cfd != 0 && !fence_done) {
                            struct { u32 id; u32 value; } r = { syncpt, 0 };
                            u32 e2 = 0;
                            NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e2));
                            seen = r.value;
                            fence_done = (static_cast<s32>(seen - fence_val) >= 0);
                        }
                        armDCacheFlush(a + OffStatus, 0x1000);
                        std::memcpy(std::addressof(st), a + OffStatus, sizeof(st));
                        if (!status_ours && st.picture_index == pic_index) {
                            status_ours = true;
                            t_status = armTicksToNs(armGetSystemTick());
                        }
                        if (!(fence_done && status_ours)) { os::SleepThread(TimeSpan::FromMicroSeconds(250)); }
                    }
                }
                u32 after_hz = 0;
                static_cast<void>(ClockRateOf(PcvModule_NVENC, std::addressof(after_hz)));
                LogLine("   nvgrc[%c]: submit rc=0x%x nverr=%u fence %u/%u %s, status %s %llu us after submit | NVENC %u Hz read %llu us before submit returned, %u Hz after",
                        v.tag, rc, nverr, seen, fence_val, fence_done ? "REACHED" : "not reached",
                        status_ours ? "WRITTEN" : "NOT written",
                        static_cast<unsigned long long>(status_ours ? (t_status - t_submit) / 1000 : 0), enc_hz,
                        static_cast<unsigned long long>((t_submit - t_read) / 1000), after_hz);

                if (!submitted) {
                    LogLine("   nvgrc[%c]: submit REJECTED - nothing reached the engine", v.tag);
                    close_all();
                    VicStage("ng:submit_rejected");
                    return;
                }
                if (!status_ours) {
                    g_engine_wedged = true;
                    LogLine("   nvgrc[%c]: no status from our job after 1 s. Leaving the channel OPEN (M74: tearing down a", v.tag);
                    LogLine("   stuck channel took the compositor with it). No further engine work this boot.");
                    VicStage("ng:STALLED_left_open");
                    return;
                }

                armDCacheFlush(a, ArenaSize);
                std::memcpy(std::addressof(st), a + OffStatus, sizeof(st));
                LogLine("   nvgrc[%c]: status error_status=%u ucode_error_status=%#x total_bit_count=%u (%u B) pic_type=%u num_slices=%u avgQP=%u QP %u..%u hrdFullness=%d intra/inter MBs=%u/%u",
                        v.tag, st.error_status, st.ucode_error_status, st.total_bit_count, st.total_bit_count / 8,
                        st.pic_type, st.num_slices, st.avgQP, st.actual_min_qp_used, st.actual_max_qp_used,
                        st.hrdFullness, st.intra_mb_count, st.inter_mb_count);

                const u8 *bs = a + OffBits;
                u32 worst = 0;
                for (u32 t = 0; t < idr::Height / 16; ++t) {
                    u64 sum = 0;
                    const u8 *row = a + OffRefOut + t * idr::Width * 16;
                    for (u32 i = 0; i < idr::Width * 16; ++i) { sum += row[i]; }
                    const u32 mean = static_cast<u32>((sum + idr::Width * 8) / (idr::Width * 16));
                    const u32 want = 32 + 8 * (t / 2);
                    const u32 d = mean > want ? mean - want : want - mean;
                    if (d > worst) { worst = d; }
                }
                const bool encoded = st.total_bit_count != 0 && st.ucode_error_status == 0;
                if (encoded) { ++n_encoded; }
                LogLine("   nvgrc[%c]: bitstream starts %02x %02x %02x %02x %02x; reconstructed luma worst stripe deviation %u -> %s",
                        v.tag, bs[0], bs[1], bs[2], bs[3], bs[4], worst,
                        !encoded ? "NO OUTPUT" : (worst <= 8 ? "encoded, reconstruction matches the input" : "encoded, reconstruction differs"));

                u32 bits_len = st.total_bit_count / 8 + st.bitstream_start_pos + 16;
                if (st.last_valid_byte_offset + 1 > bits_len) { bits_len = st.last_valid_byte_offset + 1; }
                if (bits_len < 0x1000 || st.total_bit_count == 0) { bits_len = 0x10000; }
                if (bits_len > 0x100000) {
                    LogLine("   nvgrc[%c]: bitstream is %u B, saving only the first 1 MB (the stage buffer's limit)", v.tag, bits_len);
                    bits_len = 0x100000;
                }
                char path[48];
                std::snprintf(path, sizeof(path), "sdmc:/nvenc-%c-status.bin", v.tag);
                WriteEngineOutputToSd(path, a + OffStatus, 0x1000, nullptr);
                std::snprintf(path, sizeof(path), "sdmc:/nvenc-%c-bits.bin", v.tag);
                WriteEngineOutputToSd(path, bs, bits_len, nullptr);
                if (v.tag == 'a') {
                    WriteEngineOutputToSd("sdmc:/nvenc-a-recon-y.bin", a + OffRefOut, ReconY, nullptr);
                }
                u32 rc_used = 0;
                for (u32 i = OffBits - OffRc; i > 0; --i) {
                    if (a[OffRc + i - 1] != 0) { rc_used = i; break; }
                }
                LogLine("   nvgrc[%c]: RC-process buffer: last non-zero byte at +%#x of %#x", v.tag, rc_used, OffBits - OffRc);
                if (rc_used != 0) {
                    std::snprintf(path, sizeof(path), "sdmc:/nvenc-%c-rc.bin", v.tag);
                    WriteEngineOutputToSd(path, a + OffRc, (rc_used + 0xFFF) & ~0xFFFu, nullptr);
                }
            }

            close_all();
            LogLine("   nvgrc: %u of %u variants encoded", n_encoded, static_cast<u32>(sizeof(NvgrcVariants) / sizeof(NvgrcVariants[0])));
            VicStage(n_encoded != 0 ? "ng:ENCODED" : "ng:engine_error");
        }

        constexpr u32 NvfW = 1280, NvfH = 720;
        constexpr OutDesc NvfOut{ NvfW, NvfH, NvfW, vic::PIXFMT_Y8_U8V8_N420, vic::BLK_KIND_GENERIC_16Bx2, 1 };
        constexpr u32 NvfLuma = Nv12LumaBytes(NvfOut), NvfChroma = Nv12ChromaBytes(NvfOut);
        constexpr u32 NvfOffSetup  = 0x000000;
        constexpr u32 NvfOffStatus = 0x001000;
        constexpr u32 NvfOffRc     = 0x002000;
        constexpr u32 NvfOffCmd    = 0x006000;
        constexpr u32 NvfOffBits   = 0x010000;  constexpr u32 NvfBitsSize = 0x160000;
        constexpr u32 NvfOffHist   = NvfOffBits + NvfBitsSize;
        constexpr u32 NvfOffCur    = NvfOffHist + 0x0C0000;
        constexpr u32 NvfOffCurUV  = NvfOffCur + NvfLuma;
        constexpr u32 NvfOffRefOut = (NvfOffCurUV + NvfChroma + 0xFFF) & ~0xFFFu;
        constexpr u32 NvfArenaSize = NvfOffRefOut + 0x160000;
        constexpr size_t NvfArenaBase = FbBlockRowStage + 3 * StreamStageSize;
        constexpr size_t MaxSlotBytes = FbBlockRowStage + StreamStageSize;
        static_assert(NvfLuma == 1280 * 720 && NvfChroma == 1280 * 368, "16-row blocks: what NVENC reads");
        static_assert(NvfOffCurUV % 0x100 == 0 && NvfArenaBase % 0x1000 == 0);
        static_assert(sizeof(nvenc_grc_idr::Setup) <= NvfOffStatus && sizeof(nvenc_pic_stat_s) <= NvfOffRc - NvfOffStatus);
        constexpr u32 NvfPictureIndex = 0x4D383300u;
        constexpr u32 SetupRcQpI = SetupRc + __builtin_offsetof(nvenc_h264_rc_s, QP) + 2;
        static_assert(SetupRcQpI == 0x73 && nvenc_grc_idr::Setup[SetupRcQpI] == 24, "grc's I-frame QP is 24");
        constexpr u32 SetupIdrPicId = __builtin_offsetof(nvenc_h264_drv_pic_setup_s, pic_control)
                                    + __builtin_offsetof(nvenc_h264_pic_control_s, idr_pic_id);
        static_assert(SetupIdrPicId == 0x180 && nvenc_grc_idr::Setup[SetupIdrPicId] == 0,
                      "tools/nvenc_replay.py reads idr_pic_id at 0x180; grc's setup_8 has 0");

        constexpr CscProbe NvfBt709 = { "bt709", 1, vic_csc::Shift, {
            { vic_csc::Bt709[0][0], vic_csc::Bt709[0][1], vic_csc::Bt709[0][2], vic_csc::Bt709[0][3] },
            { vic_csc::Bt709[1][0], vic_csc::Bt709[1][1], vic_csc::Bt709[1][2], vic_csc::Bt709[1][3] },
            { vic_csc::Bt709[2][0], vic_csc::Bt709[2][1], vic_csc::Bt709[2][2], vic_csc::Bt709[2][3] } } };
        constexpr CscProbe NvfBt709Bgra = { "bt709-bgra", 1, vic_csc::Shift, {
            { vic_csc::Bt709[0][2], vic_csc::Bt709[0][1], vic_csc::Bt709[0][0], vic_csc::Bt709[0][3] },
            { vic_csc::Bt709[1][2], vic_csc::Bt709[1][1], vic_csc::Bt709[1][0], vic_csc::Bt709[1][3] },
            { vic_csc::Bt709[2][2], vic_csc::Bt709[2][1], vic_csc::Bt709[2][0], vic_csc::Bt709[2][3] } } };

        constexpr u64 CornerBlockRowBytes = 80ull * 8192;
        constexpr u32 CornerBlockRows = 6;
        static_assert(CornerBlockRows * 128 >= NvfH && CornerBlockRowBytes * 120 / 80 == FbBlockRow);

        struct NvfLayout {
            u32 w, h;
            OutDesc out;
            u32 luma, chroma;
            u32 off_setup, off_status, off_rc, off_cmd, off_bits, bits_size, off_hist;
            u32 off_cur, off_cur_uv, off_ref_out, arena_size;
            u32 off_setup_p, off_ref_b, off_me_a, off_me_b, arena_size_p;
            u32 off_cur2, off_cur2_uv, arena_size_pipe;
            const u8 *idr_setup, *p_setup;
            const u8 *hdrs; u32 hdrs_len;
        };
        const NvfLayout &Layout720();

        struct NvfCtx {
            u8  *a;
            u32 ah;
            u32 efd, esyncpt;
            u32 enc;
            const NvfLayout *lay = nullptr;
        };

        struct NvfJob {
            u32 setup  = NvfOffSetup;
            u32 ref_out = NvfOffRefOut;
            u32 ref_in = 0;
            u32 me_in = 0, me_out = 0;
            u32 cur = 0, cur_uv = 0;
        };

        constinit bool g_nvf_last_fence_done = false, g_nvf_last_ours = false;
        constinit u32  g_nvf_last_fence = 0, g_nvf_last_pic = 0;

        int NvfSubmit(const NvfCtx &x, u32 pic_index, const NvfJob &j, u32 *out_fence, u64 *out_t0) {
            u8 *const a = x.a;
            const NvfLayout &L = x.lay != nullptr ? *x.lay : Layout720();
            const u32 poison = 0xA5A5A5A5u;
            std::memcpy(a + L.off_status, std::addressof(poison), 4);
            armDCacheFlush(a + L.off_status, 0x40);

            auto *w = reinterpret_cast<u32 *>(a + L.off_cmd);
            u32 n = 0;
            auto m = [&](u32 method, u32 value) {
                w[n++] = vic::Host1xOpcodeIncr(vic::UCLASS_METHOD_OFFSET, 2);
                w[n++] = method >> 2;
                w[n++] = value;
            };
            w[n++] = vic::Host1xOpcodeSetClass(0, 0x21, 0);
            w[n++] = vic::Host1xOpcodeIncr(0x0B, 1);
            w[n++] = 0;
            m(0x700, 0x00001103u);
            m(0x704, pic_index);
            m(0x200, 1);
            m(0x710, (x.enc + j.setup)      >> 8);
            m(0x718, (x.enc + L.off_status) >> 8);
            m(0x724, (x.enc + L.off_rc)     >> 8);
            m(0x71C, (x.enc + L.off_bits)   >> 8);
            m(0x720, (x.enc + L.off_hist)   >> 8);
            m(0x734, (x.enc + (j.cur    != 0 ? j.cur    : L.off_cur))    >> 8);
            m(0x740, (x.enc + (j.cur_uv != 0 ? j.cur_uv : L.off_cur_uv)) >> 8);
            m(0x730, (x.enc + j.ref_out)    >> 8);
            if (j.ref_in != 0) {
                m(0x738, (x.enc + j.me_in)  >> 8);
                m(0x73C, (x.enc + j.me_out) >> 8);
                m(0x400, (x.enc + j.ref_in) >> 8);
            }
            m(0x300, 0x100);
            n = AppendIncrSyncpt(w, n, x.esyncpt, true);
            armDCacheFlush(w, 0x1000);

            alignas(8) u8 sb[16 + 12 + 20 + 4] = {};
            u32 off = 0;
            auto put = [&](u32 v) { std::memcpy(sb + off, std::addressof(v), 4); off += 4; };
            put(1); put(0); put(1); put(1);
            put(x.ah); put(L.off_cmd); put(n);
            put(x.esyncpt); put(1); put(0); put(0); put(0);
            const u32 fence_off = off; put(0);
            const u32 sz = off;
            const u32 req = (UINT32_C(3) << 30) | (sz << 16) | (0x00u << 8) | 0x01u;

            const u64 t0 = armTicksToNs(armGetSystemTick());
            u32 nverr = 0, fence_val = 0;
            const auto rc = NvIoctl(x.efd, req, sb, sz, std::addressof(nverr));
            std::memcpy(std::addressof(fence_val), sb + fence_off, 4);
            if (R_FAILED(rc) || nverr != 0) { return 1; }
            *out_fence = fence_val;
            *out_t0 = t0;
            return 0;
        }

        int NvfWaitDone(const NvfCtx &x, u32 pic_index, u32 fence_val, u64 t0, nvenc_pic_stat_s *st, u64 *enc_ns) {
            u8 *const a = x.a;
            const NvfLayout &L = x.lay != nullptr ? *x.lay : Layout720();
            const u64 w0 = armTicksToNs(armGetSystemTick());
            const u32 cfd = CtrlFd();
            bool fence_done = false, ours = false;
            auto sample_completion = [&]() {
                if (cfd != 0 && !fence_done) {
                    struct { u32 id; u32 value; } r = { x.esyncpt, 0 };
                    u32 e2 = 0;
                    const Result rc = NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e2));
                    if (R_SUCCEEDED(rc) && e2 == 0) {
                        fence_done = (static_cast<s32>(r.value - fence_val) >= 0);
                    }
                }
                armDCacheFlush(a + L.off_status, 0x80);
                std::memcpy(st, a + L.off_status, sizeof(*st));
                ours = (st->picture_index == pic_index);
            };
            for (;;) {
                sample_completion();
                if (fence_done && ours) { break; }
                if (armTicksToNs(armGetSystemTick()) - w0 >= UINT64_C(1000000000)) {
                    sample_completion();
                    break;
                }
                os::SleepThread(TimeSpan::FromMicroSeconds(50));
            }
            *enc_ns = armTicksToNs(armGetSystemTick()) - t0;
            g_nvf_last_fence_done = fence_done;
            g_nvf_last_ours = ours;
            g_nvf_last_fence = fence_val;
            g_nvf_last_pic = st->picture_index;
            return (fence_done && ours) ? 0 : 2;
        }

        bool NvfPollDone(const NvfCtx &x, u32 pic_index, u32 fence_val) {
            const u32 cfd = CtrlFd();
            if (cfd == 0) { return false; }
            struct { u32 id; u32 value; } r = { x.esyncpt, 0 };
            u32 e2 = 0;
            NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e2));
            if (static_cast<s32>(r.value - fence_val) < 0) { return false; }
            const NvfLayout &L = x.lay != nullptr ? *x.lay : Layout720();
            armDCacheFlush(x.a + L.off_status, 0x80);
            u32 pi = 0;
            std::memcpy(std::addressof(pi), x.a + L.off_status, 4);
            return pi == pic_index;
        }

        int NvfEncode(const NvfCtx &x, u32 pic_index, nvenc_pic_stat_s *st, u64 *enc_ns, const NvfJob &j = NvfJob{}) {
            u32 fence = 0;
            u64 t0 = 0;
            if (NvfSubmit(x, pic_index, j, std::addressof(fence), std::addressof(t0)) != 0) { *enc_ns = 0; return 1; }
            return NvfWaitDone(x, pic_index, fence, t0, st, enc_ns);
        }

        constexpr u64 BlOffset(u32 x, u32 y, u32 stride_px, u32 bh_log2) {
            const u32 xb = x * 4;
            const u32 block_rows = 8u << bh_log2, block_bytes = 512u << bh_log2;
            const u64 gobs_x = (static_cast<u64>(stride_px) * 4 + 63) / 64;
            return static_cast<u64>(y / block_rows) * gobs_x * block_bytes + static_cast<u64>(xb / 64) * block_bytes
                 + ((y % block_rows) / 8) * 512 + ((xb % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xb % 32) / 16) * 32 + (y % 2) * 16 + (xb % 16);
        }
        constexpr u64 Mk8Offset(u32 x, u32 y) {
            const u32 xb = x * 4;
            return (y / 128) * 120 * 8192 + (xb / 64) * 8192 + ((y % 128) / 8) * 512
                 + ((xb % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xb % 32) / 16) * 32 + (y % 2) * 16 + (xb % 16);
        }
        static_assert(BlOffset(1300, 10, 1920, 4) == Mk8Offset(1300, 10) && BlOffset(1900, 1070, 1920, 4) == Mk8Offset(1900, 1070) &&
                      BlOffset(10, 740, 1920, 4) == Mk8Offset(10, 740) && BlOffset(1279, 719, 1920, 4) == Mk8Offset(1279, 719));

        bool SlotContentIs720(const u8 *slot, u32 *out_samples, u32 stride_px, u32 bh_log2) {
            auto px = [&](u32 x, u32 y) -> const u8 * { return slot + BlOffset(x, y, stride_px, bh_log2); };
            u32 n = 0;
            bool zero = true;
            for (u32 y = 10; y < 1080; y += 100) {
                for (u32 x = 1300; x < 1920; x += 100) { const u8 *p = px(x, y); zero &= (p[0] | p[1] | p[2]) == 0; ++n; }
            }
            for (u32 y = 740; y < 1080; y += 50) {
                for (u32 x = 10; x < 1280; x += 150) { const u8 *p = px(x, y); zero &= (p[0] | p[1] | p[2]) == 0; ++n; }
            }
            *out_samples = n;
            return zero;
        }

        struct ContentProbe { bool ok, outside_black, inside_black; u32 outside_hash, inside_hash; };

        struct SizeHint { u64 pid; u32 gen; s32 corner; };
        constinit SizeHint g_size_hint = { 0, 0, -1 };

        ContentProbe ProbeContent(::ams::svc::Handle dbg, u64 base, u32 stride_px, u32 bh_log2) {
            auto off = [&](u32 x, u32 y) -> u64 { return BlOffset(x, y, stride_px, bh_log2); };
            ContentProbe r = { true, true, true, 2166136261u, 2166136261u };
            alignas(8) u8 px[8];
            auto sample = [&](u32 x, u32 y, u32 *h, bool outside) {
                if (!r.ok) { return; }
                if (!DbgRead(dbg, px, base + off(x, y), 4)) { r.ok = false; return; }
                if ((px[0] | px[1] | px[2]) != 0) { (outside ? r.outside_black : r.inside_black) = false; }
                for (u32 i = 0; i < 3; ++i) { *h = (*h ^ px[i]) * 16777619u; }
            };
            for (u32 y = 10; y < 1080; y += 100) { for (u32 x = 1300; x < 1920; x += 100) { sample(x, y, std::addressof(r.outside_hash), true); } }
            for (u32 y = 740; y < 1080; y += 50) { for (u32 x = 10; x < 1280; x += 150) { sample(x, y, std::addressof(r.outside_hash), true); } }
            for (u32 y = 30; y < 720; y += 90) { for (u32 x = 40; x < 1280; x += 160) { sample(x, y, std::addressof(r.inside_hash), false); } }
            return r;
        }

        constinit u64 g_stale_switch_pid = 0, g_stale_switch_ns = 0;
        constinit u64 g_stale_unreliable_pid = 0;
        constinit u64 g_stale_unreliable_tid = 0;

        struct SizeCheck {
            bool started_corner = false;
            u32 small_streak = 0, big_streak = 0, stale_streak = 0;
            u32 crop_small_streak = 0;
            u32 crop_720_streak = 0;
            bool was_full = false;
            ContentProbe last[8] = {};
            u64 next_ns = 0;

            static bool CropFull(u32 crop) { return (crop >> 16) >= 1920 && (crop & 0xFFFF) >= 1080; }
            static bool CropSmall(u32 crop) { return crop != 0 && (crop >> 16) <= 1280 && (crop & 0xFFFF) <= 720; }
            static bool CropBig(u32 crop) { return crop != 0 && ((crop >> 16) > 1280 || (crop & 0xFFFF) > 720); }

            explicit SizeCheck(bool corner) : started_corner(corner),
                was_full(CropFull(g_queue_crop_wh.load(std::memory_order_relaxed))),
                next_ns(armTicksToNs(armGetSystemTick()) + UINT64_C(250000000)) {}

            const char *Step(::ams::svc::Handle dbg, u64 addr, s32 slot, u32 stride_px, u32 bh_log2) {
                const u64 now = armTicksToNs(armGetSystemTick());
                if (now < next_ns) { return nullptr; }
                next_ns = now + UINT64_C(250000000);
                const u32 crop = g_queue_crop_origin0.load(std::memory_order_relaxed) ? g_queue_crop_wh.load(std::memory_order_relaxed) : 0;
                const bool crop_full = CropFull(crop);
                was_full = was_full || crop_full;
                crop_small_streak = CropSmall(crop) ? crop_small_streak + 1 : 0;
                crop_720_streak = crop == (1280u << 16 | 720u) ? crop_720_streak + 1 : 0;
                const bool small_now = crop_720_streak >= 2 || crop_small_streak >= 8;
                if (!started_corner && was_full && small_now) { return Switch(true, "the game switched to a 720p picture (its picture size went to 1280x720)"); }
                const u64 pid = g_app_pid.load(std::memory_order_relaxed);
                if (pid != 0 && g_crop_full_pid.load(std::memory_order_relaxed) == pid) {
                    if (!started_corner && small_now) { return Switch(true, "the game switched to a 720p picture (its picture size is 1280x720)"); }
                    if (started_corner && (crop_full || CropBig(crop))) { return Switch(false, "the game switched to a 1080p picture (its picture is bigger than 1280x720)"); }
                    return nullptr;
                }
                const ContentProbe pr = ProbeContent(dbg, addr, stride_px, bh_log2);
                ContentProbe &prev = last[static_cast<u32>(slot) & 7];
                const u64 tid = g_app_tid.load(std::memory_order_relaxed);
                const bool stale_ok = pid == 0 || (g_stale_unreliable_pid != pid && (tid == 0 || g_stale_unreliable_tid != tid));
                s32 vote = -1;
                if (pr.ok) {
                    const bool moved_in  = prev.ok && pr.inside_hash != prev.inside_hash;
                    const bool moved_out = prev.ok && pr.outside_hash != prev.outside_hash;
                    if (crop_full)                                { vote = 0; }
                    else if (pr.outside_black && pr.inside_black) { vote = -1; }
                    else if (pr.outside_black)                    { vote = 1; }
                    else if (moved_out)                           { vote = 0; }
                    else if (moved_in && stale_ok)                { vote = 2; }
                }
                prev = pr;
                small_streak = (vote == 1 || vote == 2) ? small_streak + 1 : 0;
                stale_streak = vote == 2 ? stale_streak + 1 : 0;
                big_streak   = vote == 0 ? big_streak + 1 : 0;
                if (!started_corner && small_streak >= 8) {
                    if (stale_streak != 0) { g_stale_switch_pid = pid; g_stale_switch_ns = now; }
                    return Switch(true, stale_streak != 0 ? "the game switched to a 720p picture (an unchanging old picture outside the corner)"
                                                          : "the game switched to a 720p picture (black outside the corner)");
                }
                if (started_corner && big_streak >= 2) {
                    if (pid != 0 && g_stale_switch_pid == pid && now - g_stale_switch_ns < UINT64_C(30000000000) && g_stale_unreliable_pid != pid) {
                        g_stale_unreliable_pid = pid;
                        g_stale_unreliable_tid = g_app_tid.load(std::memory_order_relaxed);
                        LogLine("   nvstream: the picture outside the corner only stood still - this game is not taken for 720p by that again");
                    }
                    return Switch(false, "the game switched to a 1080p picture");
                }
                return nullptr;
            }

            const char *Switch(bool to720, const char *why) {
                g_size_hint = { g_app_pid.load(std::memory_order_relaxed), g_game_surface.generation, to720 ? 1 : 0 };
                return why;
            }
        };

        u32 NvfBitsLen(const nvenc_pic_stat_s &st) {
            u32 len = st.total_bit_count / 8 + st.bitstream_start_pos + 16;
            if (st.last_valid_byte_offset + 1 > len) { len = st.last_valid_byte_offset + 1; }
            if (len > 0x100000) { len = 0x100000; }
            return len;
        }

        struct SlotMatch { u64 addr[8]; u32 n; u32 buf; u64 pid; u32 gen; bool matched; s32 score; };
        constinit SlotMatch g_slot_match = {};

        u32 FindLibnxNvMaps(::ams::svc::Handle dbg, const u32 *ids, u32 n, u32 min_size, u64 *out) {
            if (g_ind_buf == nullptr || g_ind_size < 0x10000 || n == 0 || n > 8) { return 0; }
            const u64 t0 = armTicksToNs(armGetSystemTick());
            const size_t chunk = g_ind_size >= 0x100000 ? 0x100000 : 0x10000;
            u32 found = 0;
            for (u32 k = 0; k < n; ++k) { out[k] = 0; }
            u64 scanned = 0, addr = 0;
            constexpr u64 Budget = UINT64_C(1024) << 20;
            for (u32 steps = 0; steps < 4000 && found < n && scanned < Budget; ++steps) {
                ::ams::svc::MemoryInfo mi = {};
                ::ams::svc::PageInfo   pi = {};
                if (!DbgQuery(dbg, std::addressof(mi), std::addressof(pi), addr) || mi.size == 0) { break; }
                const u32 attr = static_cast<u32>(mi.attribute);
                const bool dev = (attr & ::ams::svc::MemoryAttribute_DeviceShared) != 0 || mi.device_count > 0;
                const u32 st = static_cast<u32>(mi.state);
                const bool rw = (static_cast<u32>(mi.permission) & 3) == 3;
                const bool data = st == static_cast<u32>(::ams::svc::MemoryState_CodeData) || st == static_cast<u32>(::ams::svc::MemoryState_AliasCodeData) ||
                                  st == static_cast<u32>(::ams::svc::MemoryState_Normal);
                if (rw && data && !dev) {
                    for (u64 off = 0; off < mi.size && found < n && scanned < Budget; off += chunk) {
                        const size_t len = static_cast<size_t>(mi.size - off < chunk ? mi.size - off : chunk);
                        if (!DbgRead(dbg, g_ind_buf, mi.base_address + off, len)) { break; }
                        scanned += len;
                        for (size_t q = 0; q + 24 <= len; q += 8) {
                            u32 f[4];
                            std::memcpy(f, g_ind_buf + q, sizeof(f));
                            if (f[0] == 0 || f[2] < min_size) { continue; }
                            for (u32 k = 0; k < n; ++k) {
                                if (out[k] != 0 || f[1] != ids[k]) { continue; }
                                u64 va = 0;
                                std::memcpy(std::addressof(va), g_ind_buf + q + 16, sizeof(va));
                                if (va == 0 || (va & 0xFFF) != 0) { continue; }
                                ::ams::svc::MemoryInfo vm = {};
                                ::ams::svc::PageInfo   vp = {};
                                if (!DbgQuery(dbg, std::addressof(vm), std::addressof(vp), va)) { continue; }
                                const bool vdev = (static_cast<u32>(vm.attribute) & ::ams::svc::MemoryAttribute_DeviceShared) != 0 || vm.device_count > 0;
                                if (!vdev || va + f[2] > vm.base_address + vm.size) { continue; }
                                out[k] = va;
                                ++found;
                                for (u32 j = k + 1; j < n; ++j) { if (ids[j] == ids[k] && out[j] == 0) { out[j] = va; ++found; } }
                            }
                        }
                    }
                }
                const u64 next = mi.base_address + mi.size;
                if (next <= addr) { break; }
                addr = next;
            }
            char line[200];
            int len = std::snprintf(line, sizeof(line), "   swapchain: libnx's nvmap records - %u of %u found in %llu MB of the app's memory (%llu ms):", found, n,
                                    static_cast<unsigned long long>(scanned >> 20), static_cast<unsigned long long>((armTicksToNs(armGetSystemTick()) - t0) / 1000000));
            for (u32 k = 0; k < n && len > 0 && static_cast<size_t>(len) < sizeof(line); ++k) {
                len += std::snprintf(line + len, sizeof(line) - len, " %u=%llx", ids[k], static_cast<unsigned long long>(out[k]));
            }
            LogLine("%s", line);
            return found;
        }

        struct NvfSession {
            u32 vfd = 0, cmd_handle = 0, vsyncpt = 0, cfg_addr = 0;
            u32 sh = 0, slot_vic = 0, arena_vic = 0;
            NvfCtx x = {};
            bool efd_open = false, keep_open = false;
            SrcDesc src{ 1920, 1080, 1920, vic::BLK_KIND_GENERIC_16Bx2, 4, vic::PIXFMT_A8R8G8B8, vic::CACHE_WIDTH_64Bx4, 1920, 1080 };
            bool corner = false;
            u32 seen = 0;
            u64 present_tick = 0;
            u32 present_flip = 0;
            u32 fence_waits = 0, fence_timeouts = 0, fence_unknown = 0;
            u32 reread = 0;
            u64 fence_wait_sum = 0, fence_wait_max = 0;
            size_t arena_base = NvfArenaBase;
            u32 arena_size = NvfArenaSize;
            const NvfLayout *lay = nullptr;
            const NvfLayout &Lay() const { return lay != nullptr ? *lay : Layout720(); }
            bool vic_only = false;
            u32 nvfd = 0;
            u32 slot_off[8] = { 0, 0x870000, 0x10E0000 };
            u32 nslots = 3;
            u32 buf_bytes = static_cast<u32>(FbSlotSize);
            bool mk8_layout = true;
            bool size_check_ok = true;
            bool CornerFast() const { return corner && mk8_layout && cap_crop_ow == 0; }

            static constexpr u32 MaxSlotCand = slotmatch::MaxCand;
            static constexpr u32 SigPoints = 32;
            bool sep_layout = false;
            bool sep = false;
            bool sep_locked = false;
            bool still_guessed = false;
            bool chain_display = false;
            u32 slot_nv[8] = {};
            u64 slot_cpu[8] = {};
            bool guess_trusted = false;
            u32 held_ms = 0;
            bool hold_done = false;
            u64 cand[MaxSlotCand] = {};
            u32 ncand = 0;
            u8 slot_cand[8] = {};
            slotmatch::Matcher matcher;

            u64 SlotAddr(u64 slot_base, s32 slot) const {
                const bool valid = slot >= 0 && static_cast<u32>(slot) < nslots;
                if (sep) { return cand[slot_cand[valid ? slot : 0]]; }
                return slot_base + (valid ? slot_off[slot] : 0);
            }

            void PrepareSlots(::ams::svc::Handle dbg, u64 slot_base, const char *who) {
                sep = sep_locked = still_guessed = false;
                u64 span = buf_bytes;
                for (u32 k = 0; k < nslots; ++k) { if (slot_off[k] + static_cast<u64>(buf_bytes) > span) { span = slot_off[k] + static_cast<u64>(buf_bytes); } }
                ::ams::svc::MemoryInfo mi = {};
                ::ams::svc::PageInfo   pi = {};
                const bool exact = DbgQuery(dbg, std::addressof(mi), std::addressof(pi), slot_base) &&
                                   mi.base_address == slot_base && mi.size == span;
                if (!sep_layout && exact) { return; }
                u64 nv_base[8] = {};
                u32 nv_found = 0;
                if (g_vim_program.load(std::memory_order_relaxed) == g_app_tid.load(std::memory_order_relaxed) && nslots <= 8) {
                    u64 *const base = nv_base;
                    nv_found = FindLibnxNvMaps(dbg, slot_nv, nslots, sep_layout ? buf_bytes : buf_bytes / 2, base);
                    if (nv_found == nslots) {
                        ncand = nslots;
                        for (u32 k = 0; k < nslots; ++k) { cand[k] = base[k] + slot_off[k]; slot_cand[k] = static_cast<u8>(k); slot_cpu[k] = cand[k]; }
                        sep = true;
                        sep_locked = true;
                        guess_trusted = true;
                        char line[200];
                        int len = std::snprintf(line, sizeof(line), "   %s: slots from the app's nvmap records:", who);
                        for (u32 k = 0; k < nslots && len > 0 && static_cast<size_t>(len) < sizeof(line); ++k) {
                            len += std::snprintf(line + len, sizeof(line) - len, " %u->0x%010llx", k, static_cast<unsigned long long>(cand[k]));
                        }
                        LogLine("%s", line);
                        return;
                    }
                }
                u32 devc[MaxSlotCand] = {};
                {
                    static constexpr u32 MaxScan = 192;
                    static u64 all[MaxScan];
                    static u32 all_devc[MaxScan];
                    const u32 nall = FindSlotCandidates(dbg, buf_bytes, slot_off, nslots, sep_layout, all, MaxScan, all_devc);
                    ncand = 0;
                    if (nall <= MaxSlotCand) {
                        for (u32 c = 0; c < nall; ++c) { cand[c] = all[c]; devc[c] = all_devc[c]; }
                        ncand = nall;
                    } else {
                        bool taken[MaxScan] = {};
                        const u64 sb_end = slot_base + mi.size;
                        for (u32 pass = 0; pass < 3; ++pass) {
                            for (u32 c = 0; c < nall && ncand < MaxSlotCand; ++c) {
                                if (taken[c]) { continue; }
                                const bool in_base = sep_layout && mi.base_address == slot_base && all[c] >= slot_base && all[c] < sb_end;
                                if ((pass == 0 && in_base) || (pass == 1 && all_devc[c] >= 2) || pass == 2) {
                                    cand[ncand] = all[c]; devc[ncand] = all_devc[c]; ++ncand; taken[c] = true;
                                }
                            }
                        }
                        for (u32 i = 1; i < ncand; ++i) {
                            for (u32 j = i; j > 0 && cand[j - 1] > cand[j]; --j) { std::swap(cand[j - 1], cand[j]); std::swap(devc[j - 1], devc[j]); }
                        }
                        LogLine("   %s: %u candidate buffers in the game's memory - kept %u: the swapchain's region, then those the display maps too", who, nall, ncand);
                    }
                }
                if (!sep_layout && ncand > nslots) {
                    u32 kept = 0;
                    for (u32 c = 0; c < ncand; ++c) { if (devc[c] >= 2) { ++kept; } }
                    if (kept >= nslots && kept < ncand) {
                        u32 k2 = 0;
                        for (u32 c = 0; c < ncand; ++c) { if (devc[c] >= 2) { cand[k2] = cand[c]; devc[k2] = devc[c]; ++k2; } }
                        LogLine("   %s: %u of %u candidates kept - the regions mapped for 2 or more devices (the display's too)", who, kept, ncand);
                        ncand = kept;
                    }
                }
                if (ncand == MaxSlotCand) { LogLine("   %s: the candidate list is full (%u) - the slots may be past it", who, ncand); }
                if (ncand < nslots) {
                    LogLine("   %s: %u slot(s)%s, but %u candidate buffer(s) of %u B in the game's memory - reading from 0x%010llx as before",
                            who, nslots, sep_layout ? ", each its own buffer" : "", ncand, buf_bytes, static_cast<unsigned long long>(slot_base));
                    return;
                }
                for (u32 k = 0; k < nslots; ++k) {
                    slot_cand[k] = static_cast<u8>(k);
                    const u64 want_addr = sep_layout ? slot_base + static_cast<u64>(k) * buf_bytes : slot_base + slot_off[k];
                    for (u32 c = 0; c < ncand; ++c) {
                        if (cand[c] == want_addr) { slot_cand[k] = static_cast<u8>(c); break; }
                    }
                }
                chain_display = false;
                bool anchored = false;
                if (sep_layout && nslots >= 2 && ncand >= nslots) {
                    auto run_at = [&](u32 c0) -> bool {
                        for (u32 k = 1; k < nslots; ++k) {
                            const u64 gap = cand[c0 + k] - cand[c0 + k - 1];
                            if (gap < buf_bytes || gap > static_cast<u64>(buf_bytes) + 0x40000) { return false; }
                        }
                        return true;
                    };
                    s32 pick = -1, pick_base = -1, pick_any = -1;
                    for (u32 c0 = 0; c0 + nslots <= ncand; ++c0) {
                        if (!run_at(c0)) { continue; }
                        bool disp = true;
                        for (u32 k = 0; k < nslots; ++k) { disp &= devc[c0 + k] >= 2; }
                        if (disp && pick < 0) { pick = static_cast<s32>(c0); }
                        if (pick_base < 0 && cand[c0] <= slot_base && slot_base < cand[c0 + nslots - 1] + buf_bytes) { pick_base = static_cast<s32>(c0); }
                        if (pick_any < 0) { pick_any = static_cast<s32>(c0); }
                    }
                    chain_display = pick >= 0;
                    if (pick < 0) { pick = pick_base >= 0 ? pick_base : pick_any; }
                    if (pick >= 0) {
                        for (u32 k = 0; k < nslots; ++k) { slot_cand[k] = static_cast<u8>(pick + k); }
                        LogLine("   %s: first guess - %u buffers end to end at 0x%010llx%s", who, nslots, static_cast<unsigned long long>(cand[pick]),
                                chain_display ? ", mapped for the display too" : (pick == pick_base ? ", where the swapchain was found" : ""));
                    }
                    if (nv_found > 0 && nv_found < nslots && sep_layout) {
                        for (u32 c0 = 0; c0 + nslots <= ncand; ++c0) {
                            if (!run_at(c0)) { continue; }
                            bool all = true;
                            for (u32 k = 0; k < nslots && all; ++k) { all = nv_base[k] == 0 || cand[c0 + k] == nv_base[k] + slot_off[k]; }
                            if (!all) { continue; }
                            for (u32 k = 0; k < nslots; ++k) { slot_cand[k] = static_cast<u8>(c0 + k); }
                            sep = true;
                            sep_locked = true;
                            guess_trusted = true;
                            chain_display = true;
                            anchored = true;
                            LogLine("   %s: %u of %u slots from the app's records, the rest the buffers end to end beside them (0x%010llx...)", who, nv_found, nslots,
                                    static_cast<unsigned long long>(cand[c0]));
                            break;
                        }
                    }
                    char line[240];
                    int len = std::snprintf(line, sizeof(line), "   %s: candidates (address/device maps):", who);
                    for (u32 c = 0; c < ncand && c < 12 && len > 0 && static_cast<size_t>(len) < sizeof(line); ++c) {
                        len += std::snprintf(line + len, sizeof(line) - len, " %llx/%u", static_cast<unsigned long long>(cand[c]), devc[c]);
                    }
                    LogLine("%s%s", line, ncand > 12 ? " ..." : "");
                }
                bool reused = false;
                if (!anchored && g_slot_match.n == nslots && g_slot_match.buf == buf_bytes) {
                    u8 m[8] = {};
                    u32 hits = 0;
                    for (u32 k = 0; k < nslots; ++k) {
                        for (u32 c = 0; c < ncand; ++c) {
                            if (cand[c] == g_slot_match.addr[k]) { m[k] = static_cast<u8>(c); ++hits; break; }
                        }
                    }
                    if (hits == nslots) { std::memcpy(slot_cand, m, sizeof(slot_cand)); reused = true; }
                    guess_trusted = reused;
                    if (reused && g_slot_match.matched && g_slot_match.score >= 900 && g_slot_match.pid == g_app_pid.load(std::memory_order_relaxed) &&
                        g_slot_match.gen == g_game_surface.generation) {
                        sep_locked = true;
                        LogLine("   %s: the same buffers as the last session - keeping its slot match", who);
                    }
                }
                matcher.Reset(nslots, ncand);
                sep = true;
                LogLine("   %s: %u slot(s), %s; %u candidate buffer(s) of %u B from 0x%010llx%s - matching them to slots while streaming",
                        who, nslots, sep_layout ? "each its own nvmap object" : "no region exactly the swapchain's size",
                        ncand, buf_bytes, static_cast<unsigned long long>(cand[0]),
                        reused ? " (starting from the last session's match)" : "");
            }

            u32 SampleSig(::ams::svc::Handle dbg, u64 base, bool *varied) const {
                const u64 step = (buf_bytes / SigPoints) & ~UINT64_C(63);
                u32 h = 2166136261u, first = 0;
                bool var = false;
                for (u32 j = 0; j < SigPoints; ++j) {
                    alignas(64) u8 b[64];
                    if (!DbgRead(dbg, b, base + j * step + ((step / 2) & ~UINT64_C(63)), sizeof(b))) { *varied = false; return 0; }
                    if (j == 0) { std::memcpy(std::addressof(first), b, sizeof(first)); }
                    for (u32 i = 0; i < sizeof(b); i += 4) { var |= std::memcmp(b + i, std::addressof(first), 4) != 0; }
                    for (u32 i = 0; i < sizeof(b); ++i) { h = (h ^ b[i]) * 16777619u; }
                }
                *varied = var;
                return h;
            }

            s32 seed_last_slot = 0;
            u32 pick_seeded = 0;
            u32 seed_reread = 0;
            u32 seed_dropped = 0;

            s32 PickSeededPresent(::ams::svc::Handle dbg, u64 slot_base) {
                if (nslots < 2) { return 0; }
                bool v0 = false, v1 = false;
                u32 a0 = this->SampleSig(dbg, this->SlotAddr(slot_base, 0), std::addressof(v0));
                u32 a1 = this->SampleSig(dbg, this->SlotAddr(slot_base, 1), std::addressof(v1));
                s32 last_changed = seed_last_slot;
                bool saw_change = false;
                u32 quiet = 0;
                for (u32 spins = 0; spins < 80; ++spins) {
                    if (early_poll) { idle_work.Poll(); }
                    if (!g_album_seed_no_binder.load(std::memory_order_relaxed) &&
                        g_queue_count.load(std::memory_order_acquire) != seen) {
                        return -1;
                    }
                    os::SleepThread(TimeSpan::FromMilliSeconds(1));
                    bool nv0 = false, nv1 = false;
                    const u32 b0 = this->SampleSig(dbg, this->SlotAddr(slot_base, 0), std::addressof(nv0));
                    const u32 b1 = this->SampleSig(dbg, this->SlotAddr(slot_base, 1), std::addressof(nv1));
                    const bool c0 = (b0 != a0);
                    const bool c1 = (b1 != a1);
                    a0 = b0; a1 = b1;
                    if (c0 || c1) {
                        saw_change = true;
                        quiet = 0;
                        if (c0) { last_changed = 0; }
                        if (c1) { last_changed = 1; }
                        continue;
                    }
                    ++quiet;
                    if (saw_change && quiet >= 2) {
                        seed_last_slot = last_changed;
                        return last_changed;
                    }
                    if (!saw_change && spins >= 3) {
                        return seed_last_slot;
                    }
                }
                return seed_last_slot;
            }

            void Calibrate(::ams::svc::Handle dbg, u32 count, s32 slot) {
                if (!sep || sep_locked || slot < 0 || static_cast<u32>(slot) >= nslots) { return; }
                u32 sig[MaxSlotCand];
                bool varied[MaxSlotCand];
                for (u32 c = 0; c < ncand; ++c) { sig[c] = this->SampleSig(dbg, cand[c], std::addressof(varied[c])); }
                u32 drawn = 0;
                if (matcher.have && count > matcher.last && count - matcher.last <= PresentRingSize - 2) {
                    for (u32 c = matcher.last + 1; c <= count; ++c) {
                        PresentSnap p;
                        if (c == count) { drawn |= 1u << slot; continue; }
                        if (!ReadPresent(c, std::addressof(p)) || p.slot < 0 || static_cast<u32>(p.slot) >= nslots) { drawn = 0; break; }
                        drawn |= 1u << p.slot;
                    }
                }
                matcher.AddSpan(count, drawn, sig);
                u8 m[8] = {};
                s32 weakest = 0;
                if (matcher.Match(m, std::addressof(weakest))) {
                    std::memcpy(slot_cand, m, nslots);
                    sep_locked = true;
                    g_slot_match.n = nslots;
                    g_slot_match.buf = buf_bytes;
                    g_slot_match.pid = g_app_pid.load(std::memory_order_relaxed);
                    g_slot_match.gen = g_game_surface.generation;
                    g_slot_match.matched = true;
                    g_slot_match.score = weakest;
                    char line[200];
                    int len = std::snprintf(line, sizeof(line), "   swapchain: slots matched to their buffers after %u frame intervals (weakest score %d/1000):",
                                            matcher.intervals, weakest);
                    for (u32 s = 0; s < nslots && len > 0 && static_cast<size_t>(len) < sizeof(line); ++s) {
                        g_slot_match.addr[s] = cand[m[s]];
                        len += std::snprintf(line + len, sizeof(line) - len, " %u->0x%010llx", s, static_cast<unsigned long long>(cand[m[s]]));
                    }
                    LogLine("%s", line);
                } else if (u8 g[8] = {}; !still_guessed && !chain_display && slotmatch::StillGuess(sig, varied, ncand, nslots, !sep_layout, g)) {
                    std::memcpy(slot_cand, g, nslots);
                    still_guessed = true;
                    LogLine("   swapchain: the picture stands still - slot 0 shows 0x%010llx (the same picture in every slot) until motion tells them apart",
                            static_cast<unsigned long long>(cand[g[0]]));
                } else if (matcher.GaveUp()) {
                    sep_locked = true;
                    LogLine("   swapchain: slots not told apart in %u samples (%u with a change) - keeping the first guess", matcher.samples, matcher.intervals);
                }
            }

            bool SetGeometry(const GameSurface &g, const char *who) {
                if (g.num_slots == 0 || g.buf_size == 0 || g.width == 0 || g.height == 0) {
                    LogLine("   %s: no buffer geometry recorded for this game", who); return false;
                }
                if (g.layout != 3) {
                    LogLine("   %s: swapchain layout %u is not block-linear - not supported", who, g.layout); return false;
                }
                if (g.buf_size > MaxSlotBytes || g.width > 1920 || g.height > 1088) {
                    LogLine("   %s: buffer %ux%u, %u B - larger than %zu B, not supported", who, g.width, g.height, g.buf_size, MaxSlotBytes); return false;
                }
                for (u32 k = 0; k < 8; ++k) { slot_off[k] = g.slot_offset[k]; slot_nv[k] = g.slot_nvmap[k]; }
                nslots = g.num_slots;
                sep_layout = false;
                for (u32 k = 1; k < g.num_slots && k < 8; ++k) { if (g.slot_nvmap[k] != g.slot_nvmap[0]) { sep_layout = true; } }
                buf_bytes = g.buf_size;
                const bool bgra = g.color_format == 0x100d12120ull || g.color_format == 0x10ad12120ull;
                const bool rgb565 = g.color_format == 0x10a881210ull;
                const bool known_fmt = g.color_format == 0x100532120ull || g.color_format == 0x10a532120ull  || bgra;
                src = SrcDesc{ g.width, g.height, g.stride_px, vic::BLK_KIND_GENERIC_16Bx2, g.block_h_log2,
                               rgb565 ? vic::PIXFMT_B5G6R5 : vic::PIXFMT_A8R8G8B8, vic::CACHE_WIDTH_64Bx4, g.width, g.height };
                src.bgra = bgra;
                mk8_layout = g.width == 1920 && g.height == 1080 && g.stride_px == 1920 && g.block_h_log2 == 4;
                size_check_ok = g.width == 1920 && g.height == 1080 && g.stride_px >= 1920 && g.block_h_log2 <= 5;
                LogLine("   %s: game swapchain %ux%u stride %u px, block height 2^%u, %u slot(s) of %u B, format %#llx%s",
                        who, g.width, g.height, g.stride_px, g.block_h_log2, nslots, buf_bytes,
                        static_cast<unsigned long long>(g.color_format), known_fmt ? "" : rgb565 ? " (16-bit RGB565)" : " (not MK8's - colours unverified)");
                return true;
            }

            ~NvfSession() {
                if (efd_open && !keep_open) {
                    if (x.enc != 0) { UnmapCmdBuffer(x.efd, x.ah); }
                    NvClose(x.efd);
                }
                if (arena_vic != 0) { UnmapCmdBuffer(vfd, x.ah); }
                if (slot_vic != 0) { UnmapCmdBuffer(vfd, sh); }
                if (!keep_open && nvfd != 0) {
                    if (x.ah != 0 && NvmapFree(nvfd, x.ah)) { --g_capture_handles; }
                    if (sh != 0 && NvmapFree(nvfd, sh)) { --g_capture_handles; }
                }
            }

            bool Open(u32 v, u32 nvmap_fd, u32 cmd, u32 vsp, u32 cfg, const char *who) {
                vfd = v; cmd_handle = cmd; vsyncpt = vsp; cfg_addr = cfg;
                if (g_engine_wedged) { LogLine("   %s: an earlier engine job never completed - not submitting", who); return false; }
                if (g_ind_buf == nullptr || g_ind_size < arena_base + arena_size || cfg_addr == 0) {
                    LogLine("   %s: capture region %zu KB, need %zu KB (cfg %#x) - not running",
                            who, g_ind_size / 1024, (arena_base + arena_size) / 1024, cfg_addr);
                    return false;
                }
                u32 si = 0;
                const u32 slot_map = static_cast<u32>(buf_bytes > FbSlotSize ? ((buf_bytes + 0xFFFu) & ~0xFFFu) : FbSlotSize);
                if (R_FAILED(NvmapOwn(nvmap_fd, g_ind_buf, slot_map, 0, std::addressof(sh), std::addressof(si), true))) {
                    LogLine("   %s: nvmap of the slot copy failed", who); return false;
                }
                nvfd = nvmap_fd;
                ++g_capture_handles;
                HoldDeviceZero(vfd);
                MapCmdBuffer(vfd, sh, std::addressof(slot_vic), "nvf-slot", 0);
                if (slot_vic == 0) { LogLine("   %s: slot pin returned 0 - refusing", who); return false; }

                x.a = g_ind_buf + arena_base;
                x.lay = lay;
                std::memset(x.a, 0, arena_size);
                if (Lay().idr_setup != nullptr) { std::memcpy(x.a + Lay().off_setup, Lay().idr_setup, 0x1000); }
                armDCacheFlush(x.a, arena_size);
                u32 aid = 0;
                if (R_FAILED(NvmapOwn(nvmap_fd, x.a, arena_size, 0, std::addressof(x.ah), std::addressof(aid), true))) {
                    LogLine("   %s: nvmap of the %u KB arena failed", who, arena_size / 1024); return false;
                }
                ++g_capture_handles;
                MapCmdBuffer(vfd, x.ah, std::addressof(arena_vic), "nvf-arena(vic)", 0);
                if (arena_vic == 0) { LogLine("   %s: arena pin (VIC) returned 0 - refusing", who); return false; }
                if (vic_only) {
                    LogLine("   %s: arena %u KB at +%zu KB; VIC sees it at %#x; picture %#x/%#x (no encoder)",
                            who, arena_size / 1024, arena_base / 1024, arena_vic, arena_vic + Lay().off_cur, arena_vic + Lay().off_cur_uv);
                    seen = g_queue_count.load(std::memory_order_relaxed);
                    return true;
                }

                u32 nverr = 0;
                if (R_FAILED(NvOpen("/dev/nvhost-msenc", std::addressof(x.efd), std::addressof(nverr))) || nverr != 0) {
                    LogLine("   %s: /dev/nvhost-msenc open FAILED nverr=%u", who, nverr); return false;
                }
                efd_open = true;
                {
                    struct { u32 module_id; u32 syncpt; } gs = { 0, 0 };
                    nverr = 0;
                    NvIoctl(x.efd, NvHostIocChannelGetSyncpoint, std::addressof(gs), sizeof(gs), std::addressof(nverr));
                    x.esyncpt = gs.syncpt;
                }
                if (x.esyncpt == 0 || x.esyncpt == 12) { LogLine("   %s: msenc syncpt=%u - REFUSING", who, x.esyncpt); return false; }
                { struct { u32 fd; } sn = { nvmap_fd }; nverr = 0;
                  NvIoctl(x.efd, NvHostIocChannelSetNvmapFd, std::addressof(sn), sizeof(sn), std::addressof(nverr)); }
                MapCmdBuffer(x.efd, x.ah, std::addressof(x.enc), "nvf-arena(enc)", 0);
                if (x.enc == 0) { LogLine("   %s: arena pin (NVENC) returned 0 - refusing", who); return false; }
                LogLine("   %s: arena %u KB at +%zu KB; VIC sees it at %#x, NVENC at %#x (syncpt %u); picture %#x/%#x, bits %#x",
                        who, arena_size / 1024, arena_base / 1024, arena_vic, x.enc, x.esyncpt,
                        x.enc + Lay().off_cur, x.enc + Lay().off_cur_uv, x.enc + Lay().off_bits);
                seen = g_queue_count.load(std::memory_order_relaxed);
                return true;
            }

            EncodeIdleWork idle_work;
            bool early_poll = false;
            bool mid_poll = false;
            u64 prev_read_ns = 0;
            u32 mid_collects = 0;
            u64 mid_poll_ns = 0;
            u64 mid_poll_sum = 0;
            static constexpr u64 MidReadMinNs = UINT64_C(6000000);
            u64 last_flush_ns = 0;

            static constexpr u64 FenceTimeoutNs = UINT64_C(250000000);

            void WaitPresentFence(u32 n, const u64 fence[4]) {
                if (n == 0 || n > 4) { ++fence_unknown; return; }
                const u32 cfd = CtrlFd();
                if (cfd == 0) { ++fence_unknown; return; }
                const u64 t0 = armTicksToNs(armGetSystemTick());
                bool timed_out = false;
                for (u32 k = 0; k < n; ++k) {
                    const u64 f = fence[k];
                    const u32 id = static_cast<u32>(f >> 32), want = static_cast<u32>(f);
                    if (id == 0xFFFFFFFFu || id >= 192) { continue; }
                    for (;;) {
                        struct { u32 id; u32 value; } r = { id, 0 };
                        u32 e = 0;
                        if (R_FAILED(NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e))) || e != 0) { break; }
                        if (static_cast<s32>(r.value - want) >= 0) { break; }
                        if (armTicksToNs(armGetSystemTick()) - t0 > FenceTimeoutNs) { timed_out = true; break; }
                        if (idle_work.Poll()) { continue; }
                        os::SleepThread(TimeSpan::FromMicroSeconds(250));
                    }
                }
                const u64 w = armTicksToNs(armGetSystemTick()) - t0;
                ++fence_waits;
                fence_wait_sum += w;
                if (w > fence_wait_max) { fence_wait_max = w; }
                if (timed_out) { ++fence_timeouts; }
            }

            static int FenceState(u32 n, const u64 fence[4]) {
                if (n == 0 || n > 4) { return -1; }
                const u32 cfd = CtrlFd();
                if (cfd == 0) { return -1; }
                for (u32 k = 0; k < n; ++k) {
                    const u32 id = static_cast<u32>(fence[k] >> 32), want = static_cast<u32>(fence[k]);
                    if (id == 0xFFFFFFFFu || id >= 192) { continue; }
                    struct { u32 id; u32 value; } r = { id, 0 };
                    u32 e = 0;
                    if (R_FAILED(NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e))) || e != 0) { return -1; }
                    if (static_cast<s32>(r.value - want) < 0) { return 0; }
                }
                return 1;
            }

            struct PresentSnap { u32 count; s32 slot; u32 fence_n; u64 fence[4]; u64 tick; u32 transform; u32 crop; u32 crop_c; u32 crop_o; u32 crop_ow; };

            static bool ReadPresent(u32 c, PresentSnap *out) {
                PresentRec &r = g_present_ring[c % PresentRingSize];
                for (u32 tries = 0; tries < 8; ++tries) {
                    const u32 s1 = r.seq.load(std::memory_order_acquire);
                    if (s1 & 1) { continue; }
                    out->count   = r.count.load(std::memory_order_relaxed);
                    out->slot    = r.slot.load(std::memory_order_relaxed);
                    out->fence_n = r.fence_n.load(std::memory_order_relaxed);
                    for (u32 k = 0; k < 4; ++k) { out->fence[k] = r.fence[k].load(std::memory_order_relaxed); }
                    out->tick    = r.tick.load(std::memory_order_relaxed);
                    out->transform = r.transform.load(std::memory_order_relaxed);
                    out->crop    = r.crop.load(std::memory_order_relaxed);
                    out->crop_c  = r.crop_c.load(std::memory_order_relaxed);
                    out->crop_o  = r.crop_o.load(std::memory_order_relaxed);
                    out->crop_ow = r.crop_ow.load(std::memory_order_relaxed);
                    std::atomic_thread_fence(std::memory_order_acquire);
                    if (r.seq.load(std::memory_order_relaxed) == s1) { return out->count == c; }
                }
                return false;
            }

            u32 pick_ready = 0, pick_waited = 0, pick_legacy = 0;

            static constexpr u32 ReadPiece = 0x200000;

            bool SlotMayBeReused(u32 c) const {
                PresentSnap next;
                if (!ReadPresent(c + 1, std::addressof(next))) { return false; }
                if (FenceState(next.fence_n, next.fence) != 1) { return false; }
                return nslots <= 2 || g_queue_count.load(std::memory_order_acquire) - c >= nslots - 1;
            }

            void HoldForMatch(::ams::svc::Handle dbg) {
                if (hold_done || !sep || sep_locked || still_guessed || guess_trusted) { return; }
                hold_done = true;
                const u64 t0 = armTicksToNs(armGetSystemTick());
                u32 c = g_queue_count.load(std::memory_order_acquire);
                while (!sep_locked && !still_guessed && armTicksToNs(armGetSystemTick()) - t0 < UINT64_C(2500000000)) {
                    if (g_queue_count.load(std::memory_order_acquire) == c) { os::SleepThread(TimeSpan::FromMilliSeconds(1)); continue; }
                    ++c;
                    PresentSnap p;
                    if (!ReadPresent(c, std::addressof(p))) { c = g_queue_count.load(std::memory_order_acquire); continue; }
                    for (u32 w = 0; w < 100 && FenceState(p.fence_n, p.fence) == 0; ++w) { os::SleepThread(TimeSpan::FromMicroSeconds(500)); }
                    this->Calibrate(dbg, p.count, p.slot);
                }
                held_ms = static_cast<u32>((armTicksToNs(armGetSystemTick()) - t0) / 1000000);
                LogLine("   swapchain: the first frame waited %u ms for the slot match (%s)", held_ms,
                        sep_locked ? "matched" : still_guessed ? "a still picture" : "not yet - streaming the first guess");
                seen = g_queue_count.load(std::memory_order_acquire) - 1;
            }

            bool Capture(::ams::svc::Handle dbg, u64 slot_base, u64 *read_ns, u32 *sig, u32 *dropped = nullptr) {
                this->HoldForMatch(dbg);
                for (u32 spins = 0; g_queue_count.load(std::memory_order_acquire) == seen && spins < 50; ++spins) {
                    if (early_poll) { idle_work.Poll(); }
                    os::SleepThread(TimeSpan::FromMilliSeconds(1));
                }
                for (u32 retry = 0; ; ) {
                if (early_poll) { idle_work.Poll(); }
                const u32 qnow = g_queue_count.load(std::memory_order_acquire);
                if (qnow == seen) { break; }
                {
                    const u32 back = qnow - seen;
                    const u32 lo = back > PresentRingSize - 2 ? qnow - (PresentRingSize - 2) : seen;
                    PresentSnap pick = {}, oldest = {};
                    bool have_pick = false, have_oldest = false;
                    for (u32 c = qnow; c != lo; --c) {
                        PresentSnap p;
                        if (!ReadPresent(c, std::addressof(p))) { continue; }
                        if (FenceState(p.fence_n, p.fence) != 0) { pick = p; have_pick = true; break; }
                        oldest = p; have_oldest = true;
                    }
                    if (!have_pick && have_oldest) {
                        this->WaitPresentFence(oldest.fence_n, oldest.fence);
                        pick = oldest; have_pick = true;
                        ++pick_waited;
                    } else if (have_pick) {
                        ++fence_waits;
                        ++pick_ready;
                    }
                    if (have_pick) {
                        if (dropped != nullptr && pick.count - seen > 1) { *dropped += pick.count - seen - 1; }
                        seen = pick.count;
                        present_tick = pick.tick;
                        present_flip = pick.transform & 3;
                        cap_crop = pick.crop;
                        cap_crop_c = pick.crop_c;
                        cap_crop_o = pick.crop_o;
                        cap_crop_ow = pick.crop_ow;
                        const s32 slot = pick.slot;
                        this->Calibrate(dbg, pick.count, slot);
                        if (!this->ReadSlot(dbg, this->SlotAddr(slot_base, slot), read_ns, sig)) { return false; }
                        if (retry < 2 && SlotMayBeReused(pick.count) && ChangedSinceRead(dbg)) {
                            ++reread;
                            ++retry;
                            continue;
                        }
                        return true;
                    }
                }
                break;
                }
                if (g_album_seed_no_binder.load(std::memory_order_relaxed) && nslots >= 2) {
                    for (;;) {
                        if (!g_album_seed_no_binder.load(std::memory_order_relaxed) &&
                            g_queue_count.load(std::memory_order_acquire) != seen) {
                            break;
                        }
                        const s32 picked = this->PickSeededPresent(dbg, slot_base);
                        if (picked < 0) { break; }
                        present_tick = armGetSystemTick();
                        this->Calibrate(dbg, seen, picked);
                        if (!this->ReadSlot(dbg, this->SlotAddr(slot_base, picked), read_ns, sig)) { return false; }
                        if (!this->SeedTorn(dbg)) {
                            ++pick_seeded;
                            seed_last_slot = picked;
                            return true;
                        }
                        ++seed_reread;
                        ++seed_dropped;
                        os::SleepThread(TimeSpan::FromMilliSeconds((seed_dropped % 8u) == 0u ? 16 : 2));
                    }
                }
                ++pick_legacy;
                u32 now = 0, fn = 0;
                u64 fence[4] = {};
                s32 slot = -1;
                for (u32 tries = 0; tries < 8; ++tries) {
                    const u32 s1 = g_queue_seq.load(std::memory_order_acquire);
                    if (s1 & 1) { continue; }
                    now  = g_queue_count.load(std::memory_order_acquire);
                    slot = g_queue_slot.load(std::memory_order_relaxed);
                    fn   = g_queue_fence_n.load(std::memory_order_relaxed);
                    for (u32 k = 0; k < 4; ++k) { fence[k] = g_queue_fence[k].load(std::memory_order_relaxed); }
                    present_tick = g_queue_tick.load(std::memory_order_relaxed);
                    present_flip = g_queue_transform.load(std::memory_order_relaxed) & 3;
                    cap_crop = g_queue_crop_origin0.load(std::memory_order_relaxed) ? g_queue_crop_wh.load(std::memory_order_relaxed) : 0;
                    cap_crop_c = g_queue_crop_c.load(std::memory_order_relaxed);
                    cap_crop_o = g_queue_crop_o.load(std::memory_order_relaxed);
                    cap_crop_ow = g_queue_crop_ow.load(std::memory_order_relaxed);
                    std::atomic_thread_fence(std::memory_order_acquire);
                    if (g_queue_seq.load(std::memory_order_relaxed) == s1) { break; }
                }
                if (now != seen) { this->WaitPresentFence(fn, fence); }
                if (dropped != nullptr && now - seen > 1) { *dropped += now - seen - 1; }
                seen = now;
                this->Calibrate(dbg, now, slot);
                return this->ReadSlot(dbg, this->SlotAddr(slot_base, slot), read_ns, sig);
            }

            u64 last_read = 0;

            bool ChangedSinceRead(::ams::svc::Handle dbg) const {
                for (u32 k = 0; k < 8; ++k) {
                    const u64 off = CornerFast() ? static_cast<u64>(k % CornerBlockRows) * FbBlockRow + ((static_cast<u64>(k) * 0x2340u % CornerBlockRowBytes) & ~UINT64_C(63))
                                           : ((static_cast<u64>(last_read_bytes) / 8 * k + 1024) & ~UINT64_C(63));
                    alignas(64) u8 b[64];
                    if (!DbgRead(dbg, b, last_read + off, sizeof(b))) { return false; }
                    if (std::memcmp(b, g_ind_buf + off, sizeof(b)) != 0) { return true; }
                }
                return false;
            }

            bool SeedTorn(::ams::svc::Handle dbg) const {
                if (last_read == 0 || last_read_bytes < 64 || g_ind_buf == nullptr) { return true; }
                for (u32 k = 0; k < 24; ++k) {
                    const u64 off = ((static_cast<u64>(last_read_bytes) / 24 * k + 1536) & ~UINT64_C(63));
                    if (off + 64 > last_read_bytes) { continue; }
                    alignas(64) u8 b[64];
                    if (!DbgRead(dbg, b, last_read + off, sizeof(b))) { return true; }
                    if (std::memcmp(b, g_ind_buf + off, sizeof(b)) != 0) { return true; }
                }
                return false;
            }

            u32 ReadBytes() const {
                if (size_check_ok) { return buf_bytes; }
                const u32 rows = PresentRect(src, cap_crop) & 0xFFFF;
                const u32 blk_rows = 8u << src.blk_h_log2;
                const u32 row_bytes = src.stride_px * 4;
                const u64 blk_bytes = static_cast<u64>(row_bytes) * blk_rows;
                if (row_bytes % 64 != 0 || blk_bytes == 0 || buf_bytes % blk_bytes != 0) { return buf_bytes; }
                const u64 n = (static_cast<u64>(rows) + blk_rows - 1) / blk_rows * blk_bytes;
                return n < buf_bytes ? static_cast<u32>(n) : buf_bytes;
            }

            bool ReadSlot(::ams::svc::Handle dbg, u64 base, u64 *read_ns, u32 *sig) {
                const u64 t0 = armTicksToNs(armGetSystemTick());
                last_read = base;
                mid_poll_ns = 0;
                {
                    const size_t need = CornerFast() ? (CornerBlockRows - 1) * static_cast<size_t>(FbBlockRow) + CornerBlockRowBytes
                                                     : static_cast<size_t>(ReadBytes());
                    if (g_ind_buf == nullptr || need > g_ind_size) {
                        static bool said = false;
                        if (!said) { LogLine("   slot read of %zu KB refused: the capture region is %zu KB", need / 1024, g_ind_size / 1024); said = true; }
                        return false;
                    }
                }
                if (CornerFast()) {
                    for (u32 br = 0; br < CornerBlockRows; ++br) {
                        u8 *dst = g_ind_buf + br * FbBlockRow;
                        if (!DbgRead(dbg, dst, base + br * FbBlockRow, CornerBlockRowBytes)) {
                            return false;
                        }
                        armDCacheFlush(dst, CornerBlockRowBytes);
                    }
                } else {
                    const u32 bytes = ReadBytes();
                    last_read_bytes = bytes;
                    const bool mid = mid_poll && prev_read_ns >= MidReadMinNs;
                    for (u32 off = 0; off < bytes; off += ReadPiece) {
                        const u32 n = bytes - off < ReadPiece ? bytes - off : ReadPiece;
                        if (!DbgRead(dbg, g_ind_buf + off, base + off, n)) {
                            return false;
                        }
                        if (mid && off + n < bytes && idle_work.fn != nullptr) {
                            const u64 tp = armTicksToNs(armGetSystemTick());
                            const bool sent_it = idle_work.Poll();
                            const u64 dp = armTicksToNs(armGetSystemTick()) - tp;
                            mid_poll_ns += dp; mid_poll_sum += dp;
                            if (sent_it) { ++mid_collects; }
                        }
                    }
                    const u64 tf = armTicksToNs(armGetSystemTick());
                    armDCacheFlush(g_ind_buf, bytes);
                    last_flush_ns = armTicksToNs(armGetSystemTick()) - tf;
                }
                u32 h = 0;
                for (u32 k = 0; k < 8192; k += 8) { h = h * 31u + g_ind_buf[k]; }
                *sig = h;
                const u64 total = armTicksToNs(armGetSystemTick()) - t0;
                *read_ns = total > mid_poll_ns ? total - mid_poll_ns : total;
                mid_poll_ns = 0;
                prev_read_ns = *read_ns;
                return true;
            }

            void Decide(const char *who) {
                if (!size_check_ok) {
                    corner = false;
                    LogLine("   %s: the VIC converts the whole %ux%u picture to 1280x720", who, src.w, src.h);
                    return;
                }
                u32 samples = 0;
                corner = SlotContentIs720(g_ind_buf, std::addressof(samples), src.stride_px, src.blk_h_log2);
                if (g_size_hint.corner >= 0 && g_size_hint.pid == g_app_pid.load(std::memory_order_relaxed) && g_size_hint.gen == g_game_surface.generation) {
                    if ((g_size_hint.corner != 0) != corner) {
                        LogLine("   %s: the last check found a %s picture - starting at that size", who, g_size_hint.corner ? "720p" : "1080p");
                    }
                    corner = g_size_hint.corner != 0;
                }
                {
                    const u64 pid = g_app_pid.load(std::memory_order_relaxed);
                    const u32 crop = g_queue_crop_wh.load(std::memory_order_relaxed);
                    if (pid != 0 && g_crop_full_pid.load(std::memory_order_relaxed) == pid && crop != 0) {
                        const bool small = (crop >> 16) <= 1280 && (crop & 0xFFFF) <= 720;
                        if (small != corner) { LogLine("   %s: the game's picture size is %ux%u - starting at that size", who, crop >> 16, crop & 0xFFFF); }
                        corner = small;
                    }
                }
                if (corner) { src.rect_w = NvfW; src.rect_h = NvfH; }
                LogLine("   %s: content %s (%u samples outside the top-left 1280x720 %s) -> %s",
                        who, corner ? "1280x720 (handheld)" : "1920x1080 (docked)", samples, corner ? "all zero" : "not all zero",
                        corner ? (mk8_layout ? "the VIC converts the corner 1:1; later frames read only its 3.9 MB" : "the VIC converts the corner 1:1")
                               : "the VIC scales it to 1280x720");
            }

            u32 crop_used = 0;
            u32 cap_crop = 0;
            u32 cap_crop_c = 0;
            u32 cap_crop_o = 0;
            u32 cap_crop_ow = 0;
            u32 offset_frames = 0;
            u32 last_read_bytes = 0;
            static u32 PresentRect(const SrcDesc &d, u32 crop) {
                const u32 cw = crop >> 16, ch = crop & 0xFFFF;
                const bool ok = crop != 0
                             && cw >= 320 && ch >= 180 && cw <= d.w && ch <= d.h
                             && (cw * 9 > ch * 16 ? cw * 9 - ch * 16 : ch * 16 - cw * 9) <= cw * 9 / 50;
                return ok ? crop : (d.w << 16 | d.h);
            }
            void CropToPresent(SrcDesc &d) {
                const u32 use = PresentRect(d, cap_crop);
                if (use != crop_used) {
                    if (crop_used != 0 || use != (d.w << 16 | d.h)) {
                        LogLine("   nvf: the game presents %ux%u of its %ux%u picture - the VIC %s", use >> 16, use & 0xFFFF, d.w, d.h,
                                use == (d.w << 16 | d.h) ? "converts all of it" : "converts only that part");
                    }
                    crop_used = use;
                }
                d.rect_w = use >> 16;
                d.rect_h = use & 0xFFFF;
            }

            bool dyn_vic = true;
            u32 dyn_out = 0;
            bool dyn_center = false;
            u32 center_frames = 0;
            u32 dyn_frames = 0;
            u32 dyn_min = 0, dyn_max = 0;
            static u32 DynamicRect(const SrcDesc &d, u32 crop) {
                const u32 cw = crop >> 16, ch = crop & 0xFFFF;
                const bool ok = crop != 0 && cw >= 320 && ch >= 180
                             && cw <= d.rect_w && ch <= d.rect_h && (cw < d.rect_w || ch < d.rect_h)
                             && !(cw == 1280 && ch == 720)
                             && cw * 10 >= ch * 12 && cw * 10 <= ch * 24;
                return ok ? crop : 0;
            }
            void CropCentered(SrcDesc &d, const OutDesc &out) {
                if (dyn_out != 0 || cap_crop_c == 0 || d.rect_w != d.w || d.rect_h != d.h) { return; }
                const u32 cw = cap_crop_c >> 16, ch = cap_crop_c & 0xFFFF;
                if (cw < 320 || ch < 180 || cw > d.w || ch > d.h) { return; }
                if (dyn_vic) {
                    if (cw >= d.w && ch >= d.h) { return; }
                    if (center_frames++ == 0) {
                        LogLine("   nvf: the game presents a part in the middle of its %ux%u picture (%ux%u) - the VIC scales that part to the stream", d.w, d.h, cw, ch);
                    }
                    d.rect_x = (d.w - cw) / 2;
                    d.rect_y = (d.h - ch) / 2;
                    d.rect_w = cw;
                    d.rect_h = ch;
                    return;
                }
                const u32 ow = static_cast<u32>(static_cast<u64>(cw) * out.w / d.w) & ~1u;
                const u32 oh = static_cast<u32>(static_cast<u64>(ch) * out.h / d.h) & ~1u;
                if (ow == 0 || oh == 0 || (ow >= out.w && oh >= out.h)) { return; }
                if (center_frames++ == 0) {
                    LogLine("   nvf: the game presents a part in the middle of its %ux%u picture (%ux%u) - the viewer shows only that part", d.w, d.h, cw, ch);
                }
                dyn_out = ow << 16 | oh;
                dyn_center = true;
            }

            void CropOffset(SrcDesc &d) {
                if (dyn_out != 0 || cap_crop_ow == 0) { return; }
                const u32 x = cap_crop_o >> 16, y = cap_crop_o & 0xFFFF, w = cap_crop_ow >> 16, h = cap_crop_ow & 0xFFFF;
                if (w < 320 || h < 180 || x + w > d.w || y + h > d.h) { return; }
                if (offset_frames++ == 0) {
                    LogLine("   nvf: the game presents %ux%u at (%u,%u) of its %ux%u picture - the VIC converts that part", w, h, x, y, d.w, d.h);
                }
                d.rect_x = x;
                d.rect_y = y;
                d.rect_w = w;
                d.rect_h = h;
            }

            void CropDynamic(SrcDesc &d, const OutDesc &out) {
                dyn_out = 0;
                const u32 use = DynamicRect(d, cap_crop);
                if (use == 0) { return; }
                if (dyn_frames++ == 0) {
                    LogLine("   nvf: dynamic resolution - the game presents %ux%u of the %ux%u it was sized for; %s",
                            use >> 16, use & 0xFFFF, d.rect_w, d.rect_h,
                            dyn_vic ? "the VIC scales the presented part to the stream" : "the viewer shows only that part (the crop goes in each frame's header)");
                }
                const u32 area = (use >> 16) * (use & 0xFFFF);
                if (dyn_min == 0 || area < (dyn_min >> 16) * (dyn_min & 0xFFFF)) { dyn_min = use; }
                if (area > (dyn_max >> 16) * (dyn_max & 0xFFFF)) { dyn_max = use; }
                if (!dyn_vic) {
                    const u32 ow = static_cast<u32>(static_cast<u64>(use >> 16) * out.w / d.rect_w) & ~1u;
                    const u32 oh = static_cast<u32>(static_cast<u64>(use & 0xFFFF) * out.h / d.rect_h) & ~1u;
                    if (ow != 0 && oh != 0 && (ow < out.w || oh < out.h)) { dyn_out = ow << 16 | oh; }
                    return;
                }
                d.rect_w = use >> 16;
                d.rect_h = use & 0xFFFF;
            }

            bool Vic(const char *stage, u32 cur = 0) {
                g_strip_src = src;
                g_strip_out = Lay().out;
                dyn_out = 0;
                dyn_center = false;
                dyn_vic = g_uvc_mode || !g_viewer_crop.load(std::memory_order_relaxed);
                if (!size_check_ok) { CropToPresent(g_strip_src); }
                else { CropDynamic(g_strip_src, g_strip_out); }
                CropCentered(g_strip_src, g_strip_out);
                CropOffset(g_strip_src);
                g_strip_flip = present_flip;
                BlendSlot ov[MaxOverlaySlots];
                g_ovl_nslots = present_flip == 0 ? OverlaySlotsFor(g_strip_out, ov) : 0;
                g_ovl_slots = g_ovl_nslots != 0 ? ov : nullptr;
                ON_SCOPE_EXIT { g_strip_flip = 0; g_ovl_slots = nullptr; g_ovl_nslots = 0; };
                JobCtx vc{ vfd, cmd_handle, vsyncpt, cfg_addr, arena_vic + (cur != 0 ? cur : Lay().off_cur), slot_vic, 0, 0, 0, 0, SelfSrc };
                vc.out_is_dst_buf = false;
                g_csc_override = src.bgra ? std::addressof(NvfBt709Bgra) : std::addressof(NvfBt709);
                const bool ok = RunOneJob(stage, VicJob::BlitStrip, true, vc);
                g_csc_override = nullptr;
                return ok;
            }

            void SetSetupByte(u32 off, u8 v) {
                x.a[NvfOffSetup + off] = v;
                armDCacheFlush(x.a + NvfOffSetup + (off & ~0x3Fu), 0x40);
            }

            bool Stall(const char *who, const char *what) {
                LogLine("   %s: %s: no status from our job in 1 s.", who, what);
                u32 now = 0;
                if (const u32 cfd = CtrlFd(); cfd != 0) {
                    const u64 w0 = armTicksToNs(armGetSystemTick());
                    for (;;) {
                        struct { u32 id; u32 value; } r = { x.esyncpt, 0 };
                        u32 e = 0;
                        NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e));
                        now = r.value;
                        if (e == 0 && static_cast<s32>(now - g_nvf_last_fence) >= 0) {
                            LogLine("   %s: the job finished %llu ms later (syncpt %u now %u, fence %u) - NVENC was busy, not wedged; the stream restarts",
                                    who, static_cast<unsigned long long>((armTicksToNs(armGetSystemTick()) - w0) / 1000000), x.esyncpt, now, g_nvf_last_fence);
                            return true;
                        }
                        if (armTicksToNs(armGetSystemTick()) - w0 > UINT64_C(3000000000)) { break; }
                        os::SleepThread(TimeSpan::FromMilliSeconds(10));
                    }
                }
                g_engine_wedged = true;
                keep_open = true;
                LogLine("   %s: still not done after 3 s more. Leaving the NVENC channel OPEN (M74).", who);
                LogLine("   %s: syncpt %u now %u, fence %u %s; status picture index %#x %s", who, x.esyncpt, now,
                        g_nvf_last_fence, g_nvf_last_fence_done ? "REACHED" : "not reached",
                        g_nvf_last_pic, g_nvf_last_ours ? "(ours)" : "(not ours - the engine never wrote its status)");
                LogLine("   No further engine work this boot.");
                return false;
            }

            bool EnsureClock(const char *who, u32 *hz) {
                const u32 ensured = ClockEnsure(PcvModule_NVENC, who);
                *hz = 0;
                return ensured != 0 && ClockRateOf(PcvModule_NVENC, hz) && *hz != 0;
            }
        };

        void GameLeft() {
            if (g_ui_session_active.load(std::memory_order_relaxed)) { return; }
            LogLine("   live: the game quit while streaming - giving the donor's memory back for what starts next");
            PrepareForHomeMenu();
        }

        bool HomeDetachKeepsEngines(const char *home_why, bool web_fast) {
            if (!g_ui_stream_armed || web_fast || home_why == nullptr) { return false; }
            return std::strcmp(home_why, "HOME button") == 0 || std::strcmp(home_why, "present stall fallback") == 0;
        }

        const char *SlotReadFailure() {
            return DebugPumpTargetGone() ? "slot read failed: the game exited" : "slot read failed";
        }

        void TryNvencRealFrames(::ams::svc::Handle dbg, u64 slot_base, u32 vfd, u32 nvmap_fd,
                                u32 cmd_handle, u32 vsyncpt, u32 cfg_addr) {
            VicStage("nf:1");
            LogLine("   ---- NVENC ON REAL GAME FRAMES (M83): game slot -> VIC BT.709 NV12, 16-row block-linear -> IDR ----");
            ON_SCOPE_EXIT { ClockWatchStop(); };
            NvfSession s;
            if (!s.Open(vfd, nvmap_fd, cmd_handle, vsyncpt, cfg_addr, "nvframe")) { VicStage("nf:open_FAILED"); return; }

            char path[48];
            auto save = [&](const char *tag, const nvenc_pic_stat_s &st) {
                armDCacheFlush(s.x.a + NvfOffStatus, 0x1000);
                armDCacheFlush(s.x.a + NvfOffBits, NvfBitsLen(st));
                std::snprintf(path, sizeof(path), "sdmc:/nvframe-%s-status.bin", tag);
                WriteEngineOutputToSd(path, s.x.a + NvfOffStatus, 0x1000, nullptr);
                std::snprintf(path, sizeof(path), "sdmc:/nvframe-%s-bits.bin", tag);
                WriteEngineOutputToSd(path, s.x.a + NvfOffBits, NvfBitsLen(st), nullptr);
            };
            auto save_picture = [&](const char *tag) {
                armDCacheFlush(s.x.a + NvfOffCur, NvfLuma + NvfChroma);
                std::snprintf(path, sizeof(path), "sdmc:/nvframe-%s-y.bin", tag);
                WriteEngineOutputToSd(path, s.x.a + NvfOffCur, NvfLuma, nullptr);
                std::snprintf(path, sizeof(path), "sdmc:/nvframe-%s-uv.bin", tag);
                WriteEngineOutputToSd(path, s.x.a + NvfOffCurUV, NvfChroma, nullptr);
            };

            u32 enc_hz = 0;
            if (!s.EnsureClock("nvframe", std::addressof(enc_hz))) {
                LogLine("   nvframe: NOT submitting - NVENC clock not established (%u Hz)", enc_hz);
                VicStage("nf:no_clock");
                return;
            }
            u64 read_ns = 0; u32 sig = 0;
            if (!s.Capture(dbg, slot_base, std::addressof(read_ns), std::addressof(sig))) { LogLine("   nvframe: %s", SlotReadFailure()); VicStage("nf:read_FAILED"); return; }
            s.Decide("nvframe");
            if (s.CornerFast()) {
                WriteSdVerified("sdmc:/nvframe-0-src.bin", g_ind_buf, CornerBlockRowBytes, g_stage_buf, nullptr);
            }
            const u64 v0 = armTicksToNs(armGetSystemTick());
            g_vic_quiet = false;
            const bool vic_ok = s.Vic("nf:vic0");
            const u64 vic_ns = armTicksToNs(armGetSystemTick()) - v0;
            LogLine("   nvframe[0]: slot read %llu us, VIC -> BT.709 NV12 (16-row blocks) %s in %llu us incl. logging (NVENC %u Hz)",
                    static_cast<unsigned long long>(read_ns / 1000), vic_ok ? "done" : "DID NOT COMPLETE",
                    static_cast<unsigned long long>(vic_ns / 1000), enc_hz);
            if (!vic_ok) { VicStage("nf:vic_FAILED"); return; }

            constexpr u8 Qps[] = { 16, 20, 24 };
            u32 pic = NvfPictureIndex;
            for (const u8 qp : Qps) {
                s.SetSetupByte(SetupRcQpI, qp);
                nvenc_pic_stat_s st = {};
                u64 enc_ns = 0;
                const int r = NvfEncode(s.x, pic, std::addressof(st), std::addressof(enc_ns));
                if (r == 1) { LogLine("   nvframe[0] QP %u: submit REJECTED", qp); VicStage("nf:submit_rejected"); return; }
                if (r == 2) { s.Stall("nvframe", "frame 0"); VicStage("nf:STALLED_left_open"); return; }
                LogLine("   nvframe[0] QP %2u: %u B in %llu us (status: error %u ucode %#x pic_type %u avgQP %u intra %u)",
                        qp, st.total_bit_count / 8, static_cast<unsigned long long>(enc_ns / 1000),
                        st.error_status, st.ucode_error_status, st.pic_type, st.avgQP, st.intra_mb_count);
                char tag[8];
                std::snprintf(tag, sizeof(tag), "q%u", qp);
                save(tag, st);
                ++pic;
            }
            save_picture("0");

            const u32 nframes = g_nvframe_n > 600 ? 600 : g_nvframe_n;
            constexpr u8 LoopQp = 20;
            s.SetSetupByte(SetupRcQpI, LoopQp);
            g_vic_quiet = true;
            u64 s_read = 0, s_vic = 0, s_enc = 0, s_work = 0, s_wait = 0, m_read = 0, m_vic = 0, m_enc = 0, m_work = 0;
            u64 s_bytes = 0;
            u32 b_min = ~0u, b_max = 0, done = 0, distinct = 0, last_sig = 0, errs = 0;
            nvenc_pic_stat_s last = {};
            const u32 q0 = g_queue_count.load(std::memory_order_relaxed);
            const u64 loop_t0 = armTicksToNs(armGetSystemTick());
            bool stalled = false;
            for (u32 i = 0; i < nframes; ++i) {
                const u64 t0 = armTicksToNs(armGetSystemTick());
                u64 rn = 0; u32 sg = 0;
                if (!s.Capture(dbg, slot_base, std::addressof(rn), std::addressof(sg))) { LogLine("   nvframe: %s at frame %u", SlotReadFailure(), i); break; }
                const u64 t1 = armTicksToNs(armGetSystemTick());
                if (!s.Vic("nf:vic")) { LogLine("   nvframe: VIC did not complete at frame %u", i); break; }
                const u64 t2 = armTicksToNs(armGetSystemTick());
                nvenc_pic_stat_s st = {};
                u64 en = 0;
                const int r = NvfEncode(s.x, pic++, std::addressof(st), std::addressof(en));
                if (r == 1) { LogLine("   nvframe: submit REJECTED at frame %u", i); break; }
                if (r == 2) { g_vic_quiet = false; s.Stall("nvframe", "loop"); stalled = true; break; }
                const u64 t3 = armTicksToNs(armGetSystemTick());
                if (st.ucode_error_status != 0 || st.total_bit_count == 0) { ++errs; }
                const u32 bytes = st.total_bit_count / 8;
                s_bytes += bytes;
                if (bytes < b_min) { b_min = bytes; }
                if (bytes > b_max) { b_max = bytes; }
                if (sg != last_sig) { ++distinct; last_sig = sg; }
                const u64 dr = rn, dv = t2 - t1, de = t3 - t2, dw = dr + dv + de;
                s_read += dr; s_vic += dv; s_enc += de; s_work += dw; s_wait += (t1 - t0) - rn;
                if (dr > m_read) { m_read = dr; }
                if (dv > m_vic) { m_vic = dv; }
                if (de > m_enc) { m_enc = de; }
                if (dw > m_work) { m_work = dw; }
                last = st;
                ++done;
            }
            const u64 wall = armTicksToNs(armGetSystemTick()) - loop_t0;
            const u32 presented = g_queue_count.load(std::memory_order_relaxed) - q0;
            g_vic_quiet = false;
            if (stalled) { VicStage("nf:STALLED_left_open"); return; }

            u32 after_hz = 0;
            static_cast<void>(ClockRateOf(PcvModule_NVENC, std::addressof(after_hz)));
            const u32 nd = done ? done : 1;
            const u64 fps_x10 = wall ? static_cast<u64>(done) * UINT64_C(10000000000) / wall : 0;
            const u64 game_x10 = wall ? static_cast<u64>(presented) * UINT64_C(10000000000) / wall : 0;
            LogLine("   nvframe: %u of %u frames at QP %u in %llu ms -> %llu.%llu fps (game presented %llu.%llu fps), %u distinct, %u with errors",
                    done, nframes, LoopQp, static_cast<unsigned long long>(wall / 1000000),
                    static_cast<unsigned long long>(fps_x10 / 10), static_cast<unsigned long long>(fps_x10 % 10),
                    static_cast<unsigned long long>(game_x10 / 10), static_cast<unsigned long long>(game_x10 % 10),
                    distinct, errs);
            LogLine("   nvframe: per frame avg/max us: read+flush %llu/%llu  VIC %llu/%llu  NVENC %llu/%llu  work %llu/%llu  (60 fps budget 16667); waiting for presents avg %llu",
                    static_cast<unsigned long long>(s_read / nd / 1000), static_cast<unsigned long long>(m_read / 1000),
                    static_cast<unsigned long long>(s_vic / nd / 1000), static_cast<unsigned long long>(m_vic / 1000),
                    static_cast<unsigned long long>(s_enc / nd / 1000), static_cast<unsigned long long>(m_enc / 1000),
                    static_cast<unsigned long long>(s_work / nd / 1000), static_cast<unsigned long long>(m_work / 1000),
                    static_cast<unsigned long long>(s_wait / nd / 1000));
            LogLine("   nvframe: IDR size avg %llu B (min %u, max %u) -> %llu Mbps at 60 fps; NVENC %u Hz after",
                    static_cast<unsigned long long>(s_bytes / nd), done ? b_min : 0, b_max,
                    static_cast<unsigned long long>(s_bytes / nd * 8 * 60 / 1000000), after_hz);
            DebugPumpLogStats("nvframe");
            if (done != 0) {
                save("last", last);
                save_picture("last");
            }
            VicStage(done == nframes ? "nf:DONE" : "nf:partial");
        }

        constexpr u32 NvpOffSetupP = NvfArenaSize;
        constexpr u32 NvpOffRefB   = NvpOffSetupP + 0x1000;
        constexpr u32 NvpOffMeA    = NvpOffRefB + 0x160000;
        constexpr u32 NvpOffMeB    = NvpOffMeA + 0x40000;
        constexpr u32 NvpArenaSize = NvpOffMeB + 0x40000;
        constexpr size_t NvpArenaBase = FbBlockRowStage + StreamStageSize;
        constexpr size_t NvpAccBase   = NvpArenaBase + NvpArenaSize;
        constexpr u32 SetupFrameNum = __builtin_offsetof(nvenc_h264_drv_pic_setup_s, pic_control)
                                    + __builtin_offsetof(nvenc_h264_pic_control_s, frame_num);
        constexpr u32 SetupRcQpP = SetupRc + __builtin_offsetof(nvenc_h264_rc_s, QP);
        static_assert(SetupFrameNum == 0x17C && nvenc_grc_p::Setup[SetupFrameNum] == 1 && nvenc_grc_p::Setup[SetupFrameNum + 2] == 2,
                      "grc's P setup: frame_num 1 at 0x17C, POC lsb 2 at 0x17E");
        static_assert(nvenc_grc_p::Setup[0x1A8] == 0x90 && nvenc_grc_idr::Setup[0x1A8] == 0x9C,
                      "pic_control word: P (type 0, ref 1) vs IDR (type 3, ref 1)");
        static_assert(sizeof(nvenc_grc_p::Setup) <= 0x1000);
        static_assert((SetupFrameNum & ~0x3Fu) == ((SetupFrameNum + 3) & ~0x3Fu), "frame_num and POC share a cache line");

        const NvfLayout &Layout720() {
            static constexpr NvfLayout L = {
                NvfW, NvfH, NvfOut, NvfLuma, NvfChroma,
                NvfOffSetup, NvfOffStatus, NvfOffRc, NvfOffCmd, NvfOffBits, NvfBitsSize, NvfOffHist,
                NvfOffCur, NvfOffCurUV, NvfOffRefOut, NvfArenaSize,
                NvpOffSetupP, NvpOffRefB, NvpOffMeA, NvpOffMeB, NvpArenaSize,
                (NvpArenaSize + 0xFFFu) & ~0xFFFu, ((NvpArenaSize + 0xFFFu) & ~0xFFFu) + NvfLuma,
                ((((NvpArenaSize + 0xFFFu) & ~0xFFFu) + NvfLuma + NvfChroma) + 0xFFFu) & ~0xFFFu,
                nvenc_grc_idr::Setup, nvenc_grc_p::Setup,
                nvenc_grc_hdrs::SpsPps, static_cast<u32>(sizeof(nvenc_grc_hdrs::SpsPps)),
            };
            return L;
        }

        constexpr u32 LxRc = 0x2000, LxCmd = 0x22000, LxBits = 0x30000, LxHistMax = 0x185000;
        constexpr u32 LxRefSize = 0x300000, LxMeSize = 0x80000;
        static_assert(sizeof(nvenc_1080p::IdrSetup) == 0x1000 && sizeof(nvenc_1080p::PSetup) == 0x1000);
        static_assert(nvenc_1080p::HistSize <= LxHistMax && 1920u * 1088u * 3u / 2u <= LxRefSize);

        NvfLayout MakeLayout(u32 w, u32 h, const u8 *idr, const u8 *p, const u8 *hdrs, u32 hdrs_len) {
            NvfLayout L = {};
            L.w = w; L.h = h;
            L.out = OutDesc{ w, h, w, vic::PIXFMT_Y8_U8V8_N420, vic::BLK_KIND_GENERIC_16Bx2, 1 };
            L.luma = Nv12LumaBytes(L.out);
            L.chroma = Nv12ChromaBytes(L.out);
            L.off_setup = NvfOffSetup; L.off_status = NvfOffStatus; L.off_rc = LxRc; L.off_cmd = LxCmd;
            L.off_bits = LxBits; L.bits_size = nvenc_1080p::BitsSize; L.off_hist = LxBits + nvenc_1080p::BitsSize;
            L.off_cur = L.off_hist + LxHistMax;
            L.off_cur_uv = L.off_cur + L.luma;
            L.off_ref_out = (L.off_cur_uv + L.chroma + 0xFFF) & ~0xFFFu;
            L.arena_size = L.off_ref_out + LxRefSize;
            L.off_setup_p = L.arena_size; L.off_ref_b = L.off_setup_p + 0x1000;
            L.off_me_a = L.off_ref_b + LxRefSize; L.off_me_b = L.off_me_a + LxMeSize;
            L.arena_size_p = L.off_me_b + LxMeSize;
            L.off_cur2 = L.arena_size_p;
            L.off_cur2_uv = L.off_cur2 + L.luma;
            L.arena_size_pipe = (L.off_cur2_uv + L.chroma + 0xFFFu) & ~0xFFFu;
            L.idr_setup = idr; L.p_setup = p; L.hdrs = hdrs; L.hdrs_len = hdrs_len;
            return L;
        }

        const NvfLayout &Layout1080() {
            static const NvfLayout L = MakeLayout(1920, 1080, nvenc_1080p::IdrSetup, nvenc_1080p::PSetup,
                                                  nvenc_1080p::SpsPps, static_cast<u32>(sizeof(nvenc_1080p::SpsPps)));
            return L;
        }

        const NvfLayout &LayoutRaw(u32 w, u32 h) {
            static NvfLayout L;
            L = {};
            L.w = w; L.h = h;
            L.out = OutDesc{ w, h, w, vic::PIXFMT_Y8_U8V8_N420 };
            L.luma = Nv12LumaBytes(L.out);
            L.chroma = Nv12ChromaBytes(L.out);
            L.off_cur = 0x1000;
            L.off_cur_uv = L.off_cur + L.luma;
            L.off_cur2 = (L.off_cur_uv + L.chroma + 0xFFFu) & ~0xFFFu;
            L.off_cur2_uv = L.off_cur2 + L.luma;
            L.arena_size = L.arena_size_p = L.arena_size_pipe = (L.off_cur2_uv + L.chroma + 0xFFFu) & ~0xFFFu;
            return L;
        }

        bool ShotFromSession(const NvfSession &s) {
            if (vic::Is16Bit(s.src.pixfmt)) { LogLine("shot: a 16-bit picture - no screenshot from the stream"); return false; }
            const u32 shown = s.size_check_ok ? (s.src.w << 16 | s.src.h) : NvfSession::PresentRect(s.src, s.cap_crop);
            u32 w = s.corner ? NvfW : shown >> 16, h = s.corner ? NvfH : shown & 0xFFFF;
            if (s.size_check_ok) {
                SrcDesc r = s.src;
                r.rect_w = w; r.rect_h = h;
                if (const u32 dyn = NvfSession::DynamicRect(r, s.cap_crop); dyn != 0) { w = dyn >> 16; h = dyn & 0xFFFF; }
            }
            u32 x0 = 0, y0 = 0;
            if (const u32 ow = s.cap_crop_ow; ow != 0 && (s.cap_crop_o >> 16) + (ow >> 16) <= s.src.w && (s.cap_crop_o & 0xFFFF) + (ow & 0xFFFF) <= s.src.h) {
                x0 = s.cap_crop_o >> 16; y0 = s.cap_crop_o & 0xFFFF; w = ow >> 16; h = ow & 0xFFFF;
            }
            const size_t n = s.CornerFast() ? static_cast<size_t>(CornerBlockRows) * FbBlockRow : s.buf_bytes;
            return WriteShot(g_shot_out, g_ind_buf, n, w, h, s.src.stride_px * 4, s.src.blk_h_log2, s.src.bgra, s.present_flip, x0, y0);
        }

        bool ShotStandalone() {
            if (g_app_excluded.load(std::memory_order_relaxed)) {
                ++g_shot_fail; LogLine("shot: this app is excluded - not attaching"); NotifyShot("Screenshots are off for this app (excluded)"); return false;
            }
            ::ams::os::ProcessId pid{};
            if (g_pmdmnt_rc != 0 || R_FAILED(::ams::pm::dmnt::GetApplicationProcessId(std::addressof(pid)))) {
                ++g_shot_fail; LogLine("shot: no game is running"); return false;
            }
            GameSurface geo;
            for (u32 tries = 0; tries < 10; ++tries) {
                const u32 gen = g_game_surface.generation;
                geo = g_game_surface;
                if (gen == g_game_surface.generation) { break; }
            }
            if (geo.num_slots == 0 || geo.buf_size == 0) { ++g_shot_fail; LogLine("shot: this game's swapchain has not been seen"); return false; }
            const bool took = g_ind_buf == nullptr;
            if (!CaptureAcquire("shot")) { ++g_shot_fail; NotifyShot("Screenshot failed: not enough memory free"); return false; }
            ON_SCOPE_EXIT { if (took) { CaptureRelease("shot"); } };
            ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
            if (!DmAttach(pid.value, std::addressof(dbg))) { ++g_shot_fail; LogLine("shot: could not attach to the game"); return false; }
            u64 slot_base = 0;
            const bool found = LocateSwapchain(dbg, geo, std::addressof(slot_base), "shot");
            u32 shot_nev = 0;
            static_cast<void>(DbgResume(dbg, std::addressof(shot_nev)));
            bool ok = false;
            if (!found) {
                ++g_shot_fail; LogLine("shot: the swapchain was not found in the game's memory");
            } else {
                NvfSession s;
                if (s.SetGeometry(geo, "shot")) {
                    s.PrepareSlots(dbg, slot_base, "shot");
                    s.seen = g_queue_count.load(std::memory_order_acquire) - 1;
                    u64 rn = 0; u32 sg = 0;
                    if (s.Capture(dbg, slot_base, std::addressof(rn), std::addressof(sg))) {
                        DbgDetach(dbg);
                        dbg = ::ams::svc::InvalidHandle;
                        s.Decide("shot");
                        ok = ShotFromSession(s);
                    } else {
                        ++g_shot_fail; LogLine("shot: %s", SlotReadFailure());
                    }
                } else {
                    ++g_shot_fail;
                }
            }
            if (dbg != ::ams::svc::InvalidHandle) { DbgDetach(dbg); }
            return ok;
        }

        StreamEnd TryUvcRawStream(::ams::svc::Handle dbg, u64 slot_base, u32 vfd, u32 nvmap_fd,
                                  u32 cmd_handle, u32 vsyncpt, u32 cfg_addr, const GameSurface *geo) {
            VicStage("uvcraw:1");
            if (g_stream_stage[0] == nullptr || g_stream_stage[1] == nullptr) { LogLine("   uvcraw: no stage buffers"); return StreamEnd::Failed; }
            NvfSession s;
            if (geo != nullptr && !s.SetGeometry(*geo, "uvcraw")) { VicStage("uvcraw:geometry"); return StreamEnd::Failed; }
            if (geo != nullptr) { s.PrepareSlots(dbg, slot_base, "uvcraw"); }
            {
                u64 rn = 0; u32 sg = 0;
                if (!s.Capture(dbg, slot_base, std::addressof(rn), std::addressof(sg))) { LogLine("   uvcraw: %s", SlotReadFailure()); return StreamEnd::GameGone; }
                s.Decide("uvcraw");
            }
            u32 w = 0, h = 0;
            UvcFrameSize(std::addressof(w), std::addressof(h));
            g_uvc_new_consumer.store(false, std::memory_order_relaxed);
            s.lay = std::addressof(LayoutRaw(w, h));
            s.vic_only = true;
            const NvfLayout &L = s.Lay();
            s.arena_size = L.arena_size;
            const size_t frame_bytes = L.luma + L.chroma;
            LogLine("   ---- WEBCAM (any app): NV12 %ux%u, %zu B a frame, no encoder ----", w, h, frame_bytes);
            if (!s.Open(vfd, nvmap_fd, cmd_handle, vsyncpt, cfg_addr, "uvcraw")) { VicStage("uvcraw:open_FAILED"); return g_engine_wedged ? StreamEnd::EngineStall : StreamEnd::Failed; }
            g_live_w.store(w, std::memory_order_relaxed);
            g_live_h.store(h, std::memory_order_relaxed);
            g_live_state.store(LiveState_Streaming, std::memory_order_relaxed);
            g_streaming_now.store(true, std::memory_order_relaxed);
            ON_SCOPE_EXIT { g_streaming_now.store(false, std::memory_order_relaxed); };
            SetViewerReason(ViewerReason_None);
            g_vic_quiet = true;
            ON_SCOPE_EXIT { g_vic_quiet = false; };

            const char *why = "all frames sent";
            StreamEnd end = StreamEnd::Done;
            u32 parity = 0, sent = 0, frame_no = 0, dropped = 0;
            bool pending = false;
            SizeCheck size_check(s.corner);
            u64 s_read = 0, s_vic = 0, s_pack = 0, s_wait = 0, w_n = 0;
            const u64 loop_t0 = armTicksToNs(armGetSystemTick());
            u64 o_t0 = loop_t0, w_t0 = loop_t0;
            u32 o_sent0 = 0, w_sent0 = 0, o_q0 = g_queue_count.load(std::memory_order_relaxed);
            for (u32 i = 0; ; ++i) {
                const u64 t0 = armTicksToNs(armGetSystemTick());
                if (!g_stream_enabled.load(std::memory_order_relaxed)) { why = "turned off from the overlay"; end = StreamEnd::Disabled; break; }
                if (g_reconfig_request.exchange(false, std::memory_order_relaxed)) { why = "new settings from the manager app"; end = StreamEnd::Reconfigure; break; }
                if (g_app_excluded.load(std::memory_order_relaxed)) { why = "this app was excluded"; end = StreamEnd::Disabled; break; }
                if (g_ui_session_active.load(std::memory_order_relaxed) && g_game_presented_in_ui.load(std::memory_order_relaxed)) { why = "a game is on screen"; end = StreamEnd::GameGone; break; }
                if (g_detach_on_home && !g_ui_session_active.load(std::memory_order_relaxed)) {
                    const u64 qt = g_queue_tick.load(std::memory_order_relaxed);
                    if (qt != 0) {
                        const u64 age_ms = armTicksToNs(armGetSystemTick() - qt) / UINT64_C(1000000);
                        const char *home_why = nullptr;
                        const bool web_fast = age_ms >= 500 && WebMenuUp();
                        if (web_fast || HomeDetachShouldFire(age_ms, g_home_detach_ms, g_home_fallback_ms, std::addressof(home_why))) {
                            why = "HOME / game stopped presenting";
                            if (web_fast && home_why == nullptr) { home_why = "web menu over game"; }
                            LogLine("home: detach (%s) after present stall %llu ms (probe %u, fallback %u)",
                                    home_why != nullptr ? home_why : "?",
                                    static_cast<unsigned long long>(age_ms), g_home_detach_ms, g_home_fallback_ms);
                            if (HomeDetachKeepsEngines(home_why, web_fast)) {
                                LogLine("home: the HOME menu is streamed - the engines stay for its session");
                            } else {
                                PrepareForHomeMenu();
                            }
                            end = StreamEnd::GameGone;
                            break;
                        }
                    }
                }
                if (geo != nullptr && g_session_surface->generation != geo->generation) { why = "the game made new frame buffers"; end = StreamEnd::Reconfigure; break; }
                if (g_uvc_new_consumer.exchange(false, std::memory_order_relaxed)) {
                    u32 nw = 0, nh = 0;
                    UvcFrameSize(std::addressof(nw), std::addressof(nh));
                    if (nw != w) { why = "the camera app asked for another size"; end = StreamEnd::Reconfigure; break; }
                }
                if (UvcHostStalled()) { why = "the camera app stopped reading"; end = StreamEnd::ViewerGone; break; }
                if (!g_uvc_streaming.load(std::memory_order_relaxed)) { why = "the camera app closed (host detached)"; end = StreamEnd::ViewerGone; break; }
                if (t0 - o_t0 >= UINT64_C(1000000000)) {
                    const u32 qn = g_queue_count.load(std::memory_order_relaxed);
                    g_live_fps_x10.store(static_cast<u32>(static_cast<u64>(sent - o_sent0) * UINT64_C(10000000000) / (t0 - o_t0)), std::memory_order_relaxed);
                    g_live_game_fps_x10.store(static_cast<u32>(static_cast<u64>(qn - o_q0) * UINT64_C(10000000000) / (t0 - o_t0)), std::memory_order_relaxed);
                    o_t0 = t0; o_sent0 = sent; o_q0 = qn;
                }
                u64 rn = 0; u32 sg = 0;
                if (!s.Capture(dbg, slot_base, std::addressof(rn), std::addressof(sg), std::addressof(dropped))) { why = SlotReadFailure(); GameLeft(); end = StreamEnd::GameGone; break; }
                const u64 t1 = armTicksToNs(armGetSystemTick());
                if (!s.Vic("uvcraw:vic", L.off_cur)) { why = "VIC did not complete"; end = StreamEnd::Failed; break; }
                if (TakeShotRequest()) { static_cast<void>(ShotFromSession(s)); }
                const u64 t2 = armTicksToNs(armGetSystemTick());
                armDCacheFlush(s.x.a + L.off_cur, frame_bytes);
                if (pending) { UvcWaitSent(); ++sent; pending = false; }
                const u64 t3 = armTicksToNs(armGetSystemTick());
                const size_t total = UvcRawPack(g_stream_stage[parity], s.x.a + L.off_cur, frame_bytes, frame_no++);
                UvcQueueFrame(g_stream_stage[parity], total);
                pending = true;
                parity ^= 1;
                const u64 t4 = armTicksToNs(armGetSystemTick());
                s_read += t1 - t0; s_vic += t2 - t1; s_wait += t3 - t2; s_pack += t4 - t3; ++w_n;

                if (s.size_check_ok) {
                    const s32 sl = g_queue_slot.load(std::memory_order_relaxed);
                    if (const char *r = size_check.Step(dbg, s.SlotAddr(slot_base, sl), sl, s.src.stride_px, s.src.blk_h_log2)) {
                        why = r; end = StreamEnd::Reconfigure; break;
                    }
                }
                if ((i + 1) % 1200 == 0) {
                    const u64 wn = armTicksToNs(armGetSystemTick());
                    const u64 n = w_n ? w_n : 1;
                    const u64 fps = wn > w_t0 ? static_cast<u64>(sent - w_sent0) * UINT64_C(10000000000) / (wn - w_t0) : 0;
                    LogLine("   uvcraw window: %llu.%llu fps sent; per frame avg us: read %llu  VIC %llu  waiting for USB %llu  pack %llu",
                            static_cast<unsigned long long>(fps / 10), static_cast<unsigned long long>(fps % 10),
                            static_cast<unsigned long long>(s_read / n / 1000), static_cast<unsigned long long>(s_vic / n / 1000),
                            static_cast<unsigned long long>(s_wait / n / 1000), static_cast<unsigned long long>(s_pack / n / 1000));
                    s_read = s_vic = s_wait = s_pack = 0; w_n = 0; w_t0 = wn; w_sent0 = sent;
                    DebugPumpLogStats("uvcraw");
                }
                os::SleepThread(TimeSpan::FromMicroSeconds(500));
            }
            if (pending) { UvcWaitSent(); ++sent; }
            const u64 ms = (armTicksToNs(armGetSystemTick()) - loop_t0) / 1000000;
            LogLine("   uvcraw: stopped: %s", why);
            LogLine("   uvcraw: %u frames sent in %llu ms (%llu.%llu fps), %u presents skipped", sent, static_cast<unsigned long long>(ms),
                    static_cast<unsigned long long>(ms ? sent * 1000ull / ms : 0), static_cast<unsigned long long>(ms ? (sent * 10000ull / ms) % 10 : 0), dropped);
            UvcLogStats("uvcraw");
            return end;
        }

        StreamEnd TryNvencStream(::ams::svc::Handle dbg, u64 slot_base, u32 vfd, u32 nvmap_fd,
                                 u32 cmd_handle, u32 vsyncpt, u32 cfg_addr, bool live,
                                 const GameSurface *geo) {
            VicStage("ns:1");
            const u32 nframes = live ? ~0u : (g_nvstream_n > 36000 ? 36000 : g_nvstream_n);
            const u32 progress_every = live ? 1200 : 600;
            const u8 qp = static_cast<u8>(g_nvstream_qp < 10 ? 10 : (g_nvstream_qp > 40 ? 40 : g_nvstream_qp));
            u32 gop = g_nvstream_gop > 250 ? 250 : g_nvstream_gop;
            if (gop == 1) { gop = 0; }
            if (gop != 0 && (g_ind_buf == nullptr || g_ind_size < NvfArenaBase + NvpArenaSize)) {
                LogLine("   nvstream: nvgop=%u needs %zu KB of capture region, have %zu KB - streaming IDR-only",
                        gop, (NvfArenaBase + NvpArenaSize) / 1024, g_ind_size / 1024);
                gop = 0;
            }
            if (live) {
                LogLine("   ---- LIVE H.264 STREAM (M86): until the viewer or the game goes, %s%u, QP %u ----",
                        gop != 0 ? "an IDR every " : "IDR-only", gop, qp);
            } else if (gop != 0) {
                LogLine("   ---- H.264 STREAM OVER USB (M85): %u frames, an IDR every %u then P frames, QP %u, BT.709 ----", nframes, gop, qp);
            } else {
                LogLine("   ---- H.264 STREAM OVER USB (M83): %u frames, IDR-only, QP %u, BT.709 ----", nframes, qp);
            }
            if (g_stream_stage[0] == nullptr || g_stream_stage[1] == nullptr) { LogLine("   nvstream: no stage buffers"); return StreamEnd::Failed; }
            g_tx_net = live && !g_uvc_mode && NetClientPresent();
            ON_SCOPE_EXIT { if (g_tx_net.exchange(false)) { NetLogStats("nvstream"); } };
            if (g_tx_net) {
                char ips[20];
                const u32 ip = NetIp();
                std::snprintf(ips, sizeof(ips), "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
                LogLine("   nvstream: to the network viewer (the console is %s)", ips);
            }
            for (u32 t = 0; t < 50 && !g_tx_net && !UsbReady(); ++t) { os::SleepThread(TimeSpan::FromMilliSeconds(100)); }
            if (!g_tx_net && !UsbReady()) { LogLine("   nvstream: USB not Configured (no host, or \"usb\" absent from the arm file) - not streaming"); VicStage("ns:no_usb"); return StreamEnd::ViewerGone; }
            ON_SCOPE_EXIT { ClockWatchStop(); };
            NvfSession s;
            if (geo != nullptr && !s.SetGeometry(*geo, "nvstream")) { VicStage("ns:geometry"); return StreamEnd::Failed; }
            if (geo != nullptr) { s.PrepareSlots(dbg, slot_base, "nvstream"); }
            {
                u64 rn = 0; u32 sg = 0;
                if (!s.Capture(dbg, slot_base, std::addressof(rn), std::addressof(sg))) { LogLine("   nvstream: %s", SlotReadFailure()); return StreamEnd::GameGone; }
                s.Decide("nvstream");
            }
            const bool uvc = g_uvc_mode;
            s.dyn_vic = uvc;
            u32 uvc_w = 0, uvc_h = 0;
            if (uvc) { UvcFrameSize(std::addressof(uvc_w), std::addressof(uvc_h)); g_uvc_new_consumer.store(false, std::memory_order_relaxed); }
            const bool fits1080 = g_ind_size >= NvfArenaBase + Layout1080().arena_size_pipe;
            const bool up1080 = !uvc && g_scale1080.load(std::memory_order_relaxed) && !s.corner &&
                                s.src.w > 1280 && s.src.h > 720 && s.src.w <= 1920 && s.src.h <= 1080 && !(s.src.w == 1920 && s.src.h == 1080);
            const bool src1080 = !s.corner && ((s.src.w == 1920 && s.src.h == 1080) || up1080) && !g_cap720_armed;
            bool full1080 = uvc ? (uvc_w == 1920 && fits1080) : (src1080 && fits1080);
            u32 lock = (live && !uvc) ? g_size_lock.load(std::memory_order_relaxed) : 0;
            if (lock == ~0u) {
                lock = full1080 ? (1920u << 16 | 1080u) : (1280u << 16 | 720u);
                g_size_lock.store(lock, std::memory_order_relaxed);
            }
            const bool lock1080 = lock != 0 && (lock >> 16) == 1920;
            {
                const bool want1080 = uvc ? uvc_w == 1920
                                          : (lock != 0 ? lock1080 : src1080);
                if (live && want1080 && !fits1080 && g_donor_mode && DonorCanGrow()) {
                    g_donor_regrow.store(true, std::memory_order_relaxed);
                    return StreamEnd::Regrow;
                }
            }
            if (lock != 0) {
                if (lock1080 && !fits1080) { LogLine("   nvstream: the recording is 1080p but the capture region holds 720p only - 720p"); }
                if (full1080 != (lock1080 && fits1080)) {
                    LogLine("   nvstream: the viewer records at %ux%u - the picture is scaled to it", lock >> 16, lock & 0xFFFF);
                }
                full1080 = lock1080 && fits1080;
            }
            if (uvc && uvc_w == 1920 && !fits1080) { LogLine("   nvstream: webcam asked for 1080p but the capture region is too small - sending 720p"); }
            u32 uvc_drops = uvc ? UvcDroppedCount() : 0;
            s.lay = full1080 ? std::addressof(Layout1080()) : nullptr;
            const NvfLayout &L = s.Lay();
            const bool big720 = !uvc && !full1080 && !s.corner && (s.src.w > 1280 || s.src.h > 720) &&
                                g_ind_size >= NvfArenaBase + Layout720().arena_size_pipe;
            const bool pipe = full1080 || big720;
            s.arena_size = pipe ? L.arena_size_pipe : (gop != 0 ? L.arena_size_p : L.arena_size);
            LogLine("   nvstream: encoding %ux%u%s%s%s", L.w, L.h, uvc ? " for the webcam" : "",
                    full1080 ? (up1080 ? " (1920x1088 coded, SPS-cropped; the VIC scales the picture up - scale_to_1080)" : " (1920x1088 coded, SPS-cropped)")
                             : (s.corner ? " (the handheld picture, 1:1)" : " (the VIC scales the picture)"),
                    pipe ? "; pipelined: the next frame is read while this one encodes" : "");
            if (!s.Open(vfd, nvmap_fd, cmd_handle, vsyncpt, cfg_addr, "nvstream")) { VicStage("ns:open_FAILED"); return g_engine_wedged ? StreamEnd::EngineStall : StreamEnd::Failed; }
            if (live) {
                g_live_w.store(L.w, std::memory_order_relaxed);
                g_live_h.store(L.h, std::memory_order_relaxed);
                g_live_state.store(LiveState_Streaming, std::memory_order_relaxed);
                g_streaming_now.store(true, std::memory_order_relaxed);
                SetViewerReason(ViewerReason_None);
                if (g_memory_boost.load(std::memory_order_relaxed)) { MemoryBoost(true); }
            }
            ON_SCOPE_EXIT { g_streaming_now.store(false, std::memory_order_relaxed); UsbByeStop(); MemoryBoost(false); };
            static const char *const AbName[4] = { "A grc's (baseline)", "B P QP+2", "C temporal hint", "D P QP+2 + temporal hint" };
            constexpr u32 ProductionVariant = 3;
            u32 ab_variant = 0;
            u8 cur_qp = qp;
            auto prep_p = [&](u32 v) {
                u8 *ps = s.x.a + L.off_setup_p;
                std::memcpy(ps, L.p_setup, 0x1000);
                const u8 pq = static_cast<u8>((v & 1) ? (cur_qp + 2 > 51 ? 51 : cur_qp + 2) : cur_qp);
                ps[SetupRcQpP] = pq;
                ps[SetupRcQpI] = pq;
                if (v & 2) {
                    const auto *pic = reinterpret_cast<const nvenc_h264_drv_pic_setup_s *>(ps);
                    const u32 mo = pic->pic_control.me_control_offset;
                    if (mo != 0 && mo + sizeof(nvenc_h264_me_control_s) <= 0x1000) {
                        auto *me = reinterpret_cast<nvenc_h264_me_control_s *>(ps + mo);
                        me->predsrc.self_temporal_enable = 1;
                        me->predsrc.self_temporal_search = 1;
                        me->predsrc.self_temporal_refine = 1;
                        me->predsrc.self_temporal_stamp_l0 = 1;
                    }
                }
                armDCacheFlush(ps, 0x1000);
            };
            if (gop != 0) { prep_p(g_nvab_armed && live ? 0 : ProductionVariant); }
            u32 enc_hz = 0;
            if (!s.EnsureClock("nvstream", std::addressof(enc_hz))) {
                LogLine("   nvstream: NOT submitting - NVENC clock not established (%u Hz)", enc_hz);
                VicStage("ns:no_clock");
                return StreamEnd::Failed;
            }
            s.SetSetupByte(SetupRcQpI, qp);

            const size_t HdrsLen = L.hdrs_len;
            const u32 max_frame = static_cast<u32>(StreamStageSize - HdrsLen);
            std::memcpy(g_stream_stage[0], L.hdrs, HdrsLen);
            std::memcpy(g_stream_stage[1], L.hdrs, HdrsLen);

            const u32 refs[2] = { L.off_ref_out, L.off_ref_b };
            const u32 mes[2]  = { L.off_me_a, L.off_me_b };
            u32 size_changes = 0;
            g_vic_quiet = true;
            u32 parity = 0, urb = 0, sent = 0, errs = 0, dropped = 0, done = 0, clock_fixes = 0;
            u32 audio_seq = 0;
            u64 audio_bytes = 0;
            if (live && !uvc) { AudioAttachRing(g_audio_ring); g_audio_active.store(g_audio_enabled.load(std::memory_order_relaxed), std::memory_order_relaxed); }
            ON_SCOPE_EXIT { g_audio_active.store(false, std::memory_order_relaxed); AudioDetachRing(); };
            u32 idrs = 0, pos = 0, n_i = 0, n_p = 0, last_sent = ~0u, last_vic = ~0u, last_pos = 0;
            bool pending = false, stalled = false, need_idr = true;
            bool said_long_gop = false;
            u32 key_requests = 0;
            if (live) { g_viewer_key_request.store(false, std::memory_order_relaxed); UsbByeStop(); }
            SizeCheck size_check(s.corner);
            u64 ab_bytes_p = 0, ab_bytes_i = 0, ab_intra = 0, ab_inter = 0, ab_qp = 0, ab_t0 = armTicksToNs(armGetSystemTick());
            u32 ab_np = 0, ab_ni = 0, ab_frames = 0, ab_window = 0;
            u32 rc_frames = 0, rc_busy_sum = 0, rc_busy_n = 0;
            u8 rc_qp_min = 51, rc_qp_max = 0;
            const bool ab = live && g_nvab_armed && gop != 0;
            if (ab) { LogLine("   nvab: bitrate experiment - P-frame variants A/B/C/D, 600 frames each, repeating"); }
            const char *why = "all frames sent";
            StreamEnd end = StreamEnd::Done;
            u64 s_read = 0, s_vic = 0, s_enc = 0, s_copy = 0, s_usb = 0, s_bytes = 0, m_work = 0, m_bytes = 0;
            u64 s_bytes_i = 0, s_bytes_p = 0, s_age = 0, m_age = 0;
            u64 m_read = 0, m_vic = 0, m_enc = 0, m_copy = 0, m_usbw = 0;
            struct Stall { u32 frame; u32 read_us, vic_us, enc_us, usbw_us, copy_us; u64 at_ms; };
            Stall stalls[8] = {};
            u32 n_stalls = 0, n_stalls_logged = 0;
            u32 viewer_gaps = 0;
            u32 pic = NvfPictureIndex + 0x100;
            const u32 q0 = g_queue_count.load(std::memory_order_relaxed);
            const u64 loop_t0 = armTicksToNs(armGetSystemTick());
            u32 q_window = q0;
            u64 t_window = loop_t0;
            struct InFlight { bool active; u32 i; u32 pic; u32 fence; u64 t_submit; bool is_idr; u32 pos; u64 present_ns, rn, vic_ns, t0, t1, flush_ns; u32 crop = 0; bool center = false; };
            u64 w_read = 0, w_flush = 0, w_vic = 0, w_enc = 0, w_fence0 = 0;
            u64 w_u_wait = 0, w_u_audio = 0, w_u_hdr = 0, w_u_post = 0;
            u32 w_u_audio_n = 0, w_u_n = 0;
            u64 lp_cap = 0, lp_vic = 0, lp_col = 0, lp_sub = 0, lp_slp = 0, lp_poll0 = 0;
            u32 lp_n = 0;
            u64 lt_cap = 0, lt_vic = 0, lt_col = 0, lt_sub = 0, lt_slp = 0, lt_n = 0;
            const u64 lt_t0 = armTicksToNs(armGetSystemTick());
            u32 w_n = 0, w_sent0 = 0, w_fwait0 = 0, w_ready0 = 0, w_waited0 = 0, w_drop0 = 0;
            u64 w_t0 = armTicksToNs(armGetSystemTick());
            InFlight fl = {};
            const u32 curs[2]    = { L.off_cur, L.off_cur2 };
            const u32 curs_uv[2] = { L.off_cur_uv, L.off_cur2_uv };
            auto log_stalls = [&]() {
                const u32 upto = n_stalls < 8 ? n_stalls : 8;
                for (; n_stalls_logged < upto; ++n_stalls_logged) {
                    const Stall &k = stalls[n_stalls_logged];
                    LogLine("   nvstream: STALL at frame %u (+%llu ms): read %u us, VIC %u us, NVENC %u us, waiting for the previous USB transfer %u us, copy %u us",
                            k.frame, static_cast<unsigned long long>(k.at_ms), k.read_us, k.vic_us, k.enc_us, k.usbw_us, k.copy_us);
                }
            };
            u64 ab_age_sum = 0, ab_age_max = 0, ab_cp_sum = 0, ab_enc_sum = 0, ab_read_sum = 0;
            u32 ab_age_n = 0, early_collects = 0;
            struct AbTotal { u64 age, cp, enc, read, ns; u32 n, halves, mids; };
            AbTotal ab_tot[2] = {};
            u64 ec_t0 = armTicksToNs(armGetSystemTick());
            u64 sp_copied = 0, sp_submit = 0, sp_enc = 0, sp_sent = 0;
            u32 sp_n = 0;
            auto collect = [&](InFlight &f) -> bool {
                f.active = false;
                const u64 w0 = armTicksToNs(armGetSystemTick());
                nvenc_pic_stat_s st = {};
                u64 en = 0;
                const int r = NvfWaitDone(s.x, f.pic, f.fence, f.t_submit, std::addressof(st), std::addressof(en));
                if (r == 2) {
                    g_vic_quiet = false;
                    if (s.Stall("nvstream", f.is_idr ? "stream (IDR)" : "stream (P)")) { why = "NVENC was late - restarting"; end = StreamEnd::Failed; return true; }
                    stalled = true; why = "NVENC stalled"; end = StreamEnd::EngineStall; return true;
                }
                const u64 t3 = armTicksToNs(armGetSystemTick());
                const u64 enc_wait = t3 - w0;
                const u32 bytes = st.total_bit_count / 8;
                if (st.ucode_error_status != 0 || bytes == 0 || st.bitstream_start_pos != 0 || bytes > max_frame) {
                    ++errs;
                    need_idr = true;
                    pos = 0;
                    return false;
                }
                if (f.is_idr) { ++idrs; need_idr = false; }
                if (ab) {
                    ++ab_frames;
                    if (f.is_idr) { ab_bytes_i += bytes; ++ab_ni; }
                    else { ab_bytes_p += bytes; ++ab_np; ab_intra += st.intra_mb_count; ab_inter += st.inter_mb_count; ab_qp += st.avgQP; }
                }
                last_pos = f.pos;
                pos = f.pos + 1;

                if (uvc) {
                    const u64 t4u = armTicksToNs(armGetSystemTick());
                    if (pending) { UvcWaitSent(); ++sent; pending = false; }
                    if (!g_uvc_streaming.load(std::memory_order_relaxed)) { why = "the camera app closed (host detached)"; end = StreamEnd::ViewerGone; return true; }
                    armDCacheFlush(s.x.a + L.off_bits, bytes);
                    std::memcpy(g_stream_stage[parity] + HdrsLen, s.x.a + L.off_bits, bytes);
                    UvcQueueFrame(g_stream_stage[parity], HdrsLen + bytes);
                    pending = true;
                    parity ^= 1;
                    last_sent = f.i;
                    const u64 t6u = armTicksToNs(armGetSystemTick());
                    const u64 age = (f.present_ns != 0 && t6u > f.present_ns) ? t6u - f.present_ns : 0;
                    if (t4u - t3 > UINT64_C(500000000)) { ++viewer_gaps; }
                    ++done;
                    ++w_n; w_read += f.rn; w_flush += f.flush_ns; w_vic += f.vic_ns; w_enc += enc_wait; LiveFrameSent(f.rn, f.vic_ns);
                    s_read += f.rn; s_vic += f.vic_ns; s_enc += enc_wait; s_usb += t6u - t3;
                    s_bytes += bytes;
                    if (f.is_idr) { s_bytes_i += bytes; ++n_i; } else { s_bytes_p += bytes; ++n_p; }
                    s_age += age;
                    if (age > m_age) { m_age = age; }
                    if (bytes > m_bytes) { m_bytes = bytes; }
                    return false;
                }
                if (pending) {
                    size_t got = 0;
                    if (!UsbWaitAsync(urb, std::addressof(got))) { pending = false; why = "USB transfer did not complete (viewer gone?)"; end = StreamEnd::ViewerGone; return true; }
                    ++sent;
                    pending = false;
                }
                if (live && !SendConsoleInfoIfDue()) { why = "USB info send failed (viewer gone?)"; end = StreamEnd::ViewerGone; return true; }
                const u64 t3w = armTicksToNs(armGetSystemTick());
                w_u_wait += t3w - t3;
                if (live && g_audio_active.load(std::memory_order_relaxed)) {
                    const size_t an = g_audio_stage != nullptr ? AudioTake(g_audio_stage, AudioStageSize) : 0;
                    if (an != 0) {
                        SftHdrWire ah = {};
                        ah.magic = 0x52544653u;
                        ah.version = 2;
                        ah.flags = 8;
                        ah.width = 48000; ah.height = 2;
                        ah.length = static_cast<u32>(an);
                        ah.kind = audio_seq++;
                        std::memcpy(g_usb_hdr, std::addressof(ah), sizeof(ah));
                        size_t as = 0;
                        if (!UsbSendBuffer(g_usb_hdr, sizeof(SftHdrWire), std::addressof(as)) ||
                            !UsbSendBuffer(g_audio_stage, an, std::addressof(as))) {
                            why = "USB audio send failed (viewer gone?)"; end = StreamEnd::ViewerGone; return true;
                        }
                        audio_bytes += an;
                        w_u_audio += armTicksToNs(armGetSystemTick()) - t3w; ++w_u_audio_n;
                    }
                }
                const u64 t4 = armTicksToNs(armGetSystemTick());
                armDCacheFlush(s.x.a + L.off_bits, bytes);
                std::memcpy(g_stream_stage[parity] + HdrsLen, s.x.a + L.off_bits, bytes);
                const u32 payload = static_cast<u32>(HdrsLen + bytes);
                const u64 t5 = armTicksToNs(armGetSystemTick());
                const u64 age = (f.present_ns != 0 && t5 > f.present_ns) ? t5 - f.present_ns : 0;
                if (age != 0) { ab_age_sum += age; ++ab_age_n; ab_read_sum += f.rn; if (age > ab_age_max) { ab_age_max = age; } }
                if (age != 0 && f.t1 > f.present_ns && f.t_submit >= f.t1 && w0 >= f.t_submit && t5 >= w0) {
                    sp_copied += f.t1 - f.present_ns; sp_submit += f.t_submit - f.t1; sp_enc += w0 - f.t_submit; sp_sent += t5 - w0; ++sp_n;
                    ab_cp_sum += f.t1 - f.present_ns; ab_enc_sum += w0 - f.t_submit;
                }
                SftHdrWire h = {};
                h.magic = 0x52544653u;
                h.version = 2;
                h.flags = static_cast<u16>(2 | (f.is_idr ? 4 : 0) | (f.crop != 0 ? 32 : 0) | (f.crop != 0 && f.center ? 64 : 0) | VPadHdrFlag());
                h.width = L.w; h.height = L.h;
                h.stride = static_cast<u32>(age / 1000);
                h.length = payload;
                h.block_h_log2 = f.crop;
                h.kind = f.i;
                std::memcpy(g_usb_hdr, std::addressof(h), sizeof(h));
                size_t hs = 0;
                if (!UsbSendBuffer(g_usb_hdr, sizeof(SftHdrWire), std::addressof(hs))) { why = "USB header send failed (viewer gone?)"; end = StreamEnd::ViewerGone; return true; }
                const u64 t5h = armTicksToNs(armGetSystemTick());
                if (!UsbPostAsync(g_stream_stage[parity], payload, std::addressof(urb))) { why = "USB post failed"; end = StreamEnd::ViewerGone; return true; }
                w_u_hdr += t5h - t5; w_u_post += armTicksToNs(armGetSystemTick()) - t5h; ++w_u_n;
                pending = true;
                parity ^= 1;
                last_sent = f.i;
                const u64 t6 = armTicksToNs(armGetSystemTick());
                if ((t4 - t3) > UINT64_C(500000000) || (t6 - t5) > UINT64_C(500000000)) { need_idr = true; ++viewer_gaps; }

                ++done;
                if (done == 1) { LogDefer(false); }
                ++w_n; w_read += f.rn; w_flush += f.flush_ns; w_vic += f.vic_ns; w_enc += enc_wait; LiveFrameSent(f.rn, f.vic_ns);
                s_read += f.rn; s_vic += f.vic_ns; s_enc += enc_wait; s_copy += t5 - t4; s_usb += (t4 - t3) + (t6 - t5);
                s_bytes += bytes;
                if (f.is_idr) { s_bytes_i += bytes; ++n_i; } else { s_bytes_p += bytes; ++n_p; }
                s_age += age;
                if (age > m_age) { m_age = age; }
                if (bytes > m_bytes) { m_bytes = bytes; }
                const u64 work = f.rn + f.vic_ns + enc_wait + (t6 - t3);
                if (work > m_work) { m_work = work; }
                if (f.rn > m_read) { m_read = f.rn; }
                if (f.vic_ns > m_vic) { m_vic = f.vic_ns; }
                if (enc_wait > m_enc) { m_enc = enc_wait; }
                if (t4 - t3 > m_usbw) { m_usbw = t4 - t3; }
                if (t5 - t4 > m_copy) { m_copy = t5 - t4; }
                if (work > UINT64_C(50000000)) {
                    if (n_stalls < 8) {
                        stalls[n_stalls] = { f.i, static_cast<u32>(f.rn / 1000), static_cast<u32>(f.vic_ns / 1000),
                                             static_cast<u32>(enc_wait / 1000), static_cast<u32>((t4 - t3) / 1000),
                                             static_cast<u32>((t5 - t4) / 1000), (f.t0 - loop_t0) / 1000000 };
                    }
                    ++n_stalls;
                }
                return false;
            };

            bool hook_stop = false;
            auto hook = [&]() -> bool {
                if (!fl.active || !NvfPollDone(s.x, fl.pic, fl.fence)) { return false; }
                if (collect(fl)) { hook_stop = true; }
                ++early_collects;
                return true;
            };
            u64 o_t0 = loop_t0;
            u32 o_sent0 = 0, o_q0 = q0;
            const u32 pace_fps = g_max_fps.load(std::memory_order_relaxed);
            const bool pace_on = live && src1080 && pace_fps > 0;
            u32 pace_div = pace::MinDivisor(pace_fps);
            u64 pace_last_t0 = 0, pace_work = 0;
            u32 pace_n = 0;
            if (pace_on) { LogLine("   nvstream: pacing on, at most %u fps", pace_fps); }
            for (u32 i = 0; i < nframes; ++i) {
                u64 rn = 0; u32 sg = 0;
                if (pace_on && pace_last_t0 != 0) {
                    const u64 pnow = armTicksToNs(armGetSystemTick());
                    const u64 ptarget = pace_last_t0 + pace::IntervalNs(pace_div) - UINT64_C(1500000);
                    if (pnow < ptarget) {
                        if (fl.active && collect(fl)) { break; }
                        const u64 ps0 = armTicksToNs(armGetSystemTick());
                        if (ps0 < ptarget) { os::SleepThread(TimeSpan::FromNanoSeconds(static_cast<s64>(ptarget - ps0))); }
                        const u64 psl = armTicksToNs(armGetSystemTick()) - ps0;
                        lp_slp += psl; lt_slp += psl;
                    }
                }
                const u64 t0 = armTicksToNs(armGetSystemTick());
                if (live && !g_stream_enabled.load(std::memory_order_relaxed)) { why = "turned off from the overlay"; end = StreamEnd::Disabled; break; }
                if (live && g_applet_needs_memory.load(std::memory_order_relaxed)) { why = "an applet needs the memory"; end = StreamEnd::Disabled; break; }
                if (live && g_reconfig_request.exchange(false, std::memory_order_relaxed)) { why = "new settings from the manager app"; end = StreamEnd::Reconfigure; break; }
                if (live && g_app_excluded.load(std::memory_order_relaxed)) { why = "this app was excluded"; end = StreamEnd::Disabled; break; }
                if (live && (i & 7) == 0 && UsbViewerSaidBye()) { why = "the viewer closed"; end = StreamEnd::ViewerGone; break; }
                if (live && g_ui_session_active.load(std::memory_order_relaxed) && g_game_presented_in_ui.load(std::memory_order_relaxed)) {
                    why = "a game is on screen"; end = StreamEnd::GameGone; break;
                }
                if (live && g_detach_on_home && !g_ui_session_active.load(std::memory_order_relaxed)) {
                    const u64 qt = g_queue_tick.load(std::memory_order_relaxed);
                    if (qt != 0) {
                        const u64 age_ms = armTicksToNs(armGetSystemTick() - qt) / UINT64_C(1000000);
                        const char *home_why = nullptr;
                        const bool web_fast = age_ms >= 500 && WebMenuUp();
                        if (web_fast || HomeDetachShouldFire(age_ms, g_home_detach_ms, g_home_fallback_ms, std::addressof(home_why))) {
                            why = "HOME / game stopped presenting";
                            if (web_fast && home_why == nullptr) { home_why = "web menu over game"; }
                            LogLine("home: detach (%s) after present stall %llu ms (probe %u, fallback %u)",
                                    home_why != nullptr ? home_why : "?",
                                    static_cast<unsigned long long>(age_ms), g_home_detach_ms, g_home_fallback_ms);
                            if (HomeDetachKeepsEngines(home_why, web_fast)) {
                                LogLine("home: the HOME menu is streamed - the engines stay for its session");
                            } else {
                                PrepareForHomeMenu();
                            }
                            end = StreamEnd::GameGone;
                            break;
                        }
                    }
                }
                if (live && geo != nullptr && g_session_surface->generation != geo->generation) { why = "the game made new frame buffers"; end = StreamEnd::Reconfigure; break; }
                if (uvc) {
                    if (g_uvc_new_consumer.exchange(false, std::memory_order_relaxed)) {
                        u32 nw = 0, nh = 0;
                        UvcFrameSize(std::addressof(nw), std::addressof(nh));
                        if (nw != uvc_w) { why = "the camera app asked for another size"; end = StreamEnd::Reconfigure; break; }
                        need_idr = true;
                    }
                    const u32 nd = UvcDroppedCount();
                    if (nd != uvc_drops) { uvc_drops = nd; need_idr = true; }
                    if (UvcHostStalled()) { why = "the camera app stopped reading"; end = StreamEnd::ViewerGone; break; }
                }
                if (live && t0 - o_t0 >= UINT64_C(1000000000)) {
                    const u32 qn = g_queue_count.load(std::memory_order_relaxed);
                    g_live_fps_x10.store(static_cast<u32>(static_cast<u64>(sent - o_sent0) * UINT64_C(10000000000) / (t0 - o_t0)), std::memory_order_relaxed);
                    g_live_game_fps_x10.store(static_cast<u32>(static_cast<u64>(qn - o_q0) * UINT64_C(10000000000) / (t0 - o_t0)), std::memory_order_relaxed);
                    o_t0 = t0; o_sent0 = sent; o_q0 = qn;
                }
                const bool early = pipe && (g_early_collect == 1 || (g_early_collect == 2 && ((i / 600) & 1) == 0));
                const u32 mid_mode = g_mid_collect.load(std::memory_order_relaxed);
                const bool mid = early && (mid_mode == 1 || (mid_mode == 2 && ((i / 600) & 1) == 0));
                if (pipe && (g_early_collect == 2 || mid_mode == 2) && i % 600 == 599) {
                    const u64 ab_dt = t0 > ec_t0 ? t0 - ec_t0 : 1;
                    const u64 ab_fps_x10 = static_cast<u64>(ab_age_n) * UINT64_C(10000000000) / ab_dt;
                    const u32 abn = ab_age_n ? ab_age_n : 1;
                    LogLine("   A/B early-collect %s, mid-read collect %s: console age avg %llu us (copied %llu, encoded %llu), max %llu us; read avg %llu us; %u frames sent, %llu.%llu fps; collected while idle %u, inside a read %u",
                            early ? "ON " : "OFF", mid ? "ON " : "OFF", static_cast<unsigned long long>(ab_age_sum / abn / 1000),
                            static_cast<unsigned long long>(ab_cp_sum / abn / 1000), static_cast<unsigned long long>(ab_enc_sum / abn / 1000),
                            static_cast<unsigned long long>(ab_age_max / 1000), static_cast<unsigned long long>(ab_read_sum / abn / 1000), ab_age_n,
                            static_cast<unsigned long long>(ab_fps_x10 / 10), static_cast<unsigned long long>(ab_fps_x10 % 10), early_collects, s.mid_collects);
                    {
                        AbTotal &tt = ab_tot[(early && (mid_mode != 2 || mid)) ? 0 : 1];
                        tt.age += ab_age_sum; tt.cp += ab_cp_sum; tt.enc += ab_enc_sum; tt.read += ab_read_sum; tt.ns += ab_dt;
                        tt.n += ab_age_n; ++tt.halves; tt.mids += s.mid_collects;
                    }
                    ab_age_sum = ab_age_max = ab_cp_sum = ab_enc_sum = ab_read_sum = 0; ab_age_n = 0; early_collects = 0; s.mid_collects = 0; ec_t0 = t0;
                }
                s.early_poll = early;
                s.mid_poll = mid;
                if (pipe && fl.active) {
                    s.idle_work = { [](void *p) -> bool { return (*static_cast<decltype(hook) *>(p))(); }, std::addressof(hook) };
                }
                const bool got = s.Capture(dbg, slot_base, std::addressof(rn), std::addressof(sg), std::addressof(dropped));
                if (got && early) { s.idle_work.Poll(); }
                s.idle_work = {};
                s.early_poll = false;
                s.mid_poll = false;
                if (hook_stop) { break; }
                if (!got) { why = SlotReadFailure(); if (live) { GameLeft(); } end = StreamEnd::GameGone; break; }
                const u64 present_ns = armTicksToNs(s.present_tick);
                const u64 t1 = armTicksToNs(armGetSystemTick());
                const u32 k = pipe ? (i & 1) : 0;
                if (!s.Vic("ns:vic", curs[k])) { why = "VIC did not complete"; end = StreamEnd::Failed; break; }
                last_vic = i;
                if (live && TakeShotRequest()) { static_cast<void>(ShotFromSession(s)); }
                const u64 t2 = armTicksToNs(armGetSystemTick());

                if (fl.active && collect(fl)) { break; }
                const u64 t2c = armTicksToNs(armGetSystemTick());

                if (g_tx_net && ++rc_frames >= 12) {
                    rc_frames = 0;
                    const u32 busy = NetBusyPercent();
                    u8 nq = cur_qp;
                    if (busy > 97 && nq < 44) { nq = static_cast<u8>(nq + 3 > 44 ? 44 : nq + 3); }
                    else if (busy > 85 && nq < 44) { nq = static_cast<u8>(nq + 1); }
                    else if (busy < 55 && nq > qp) { nq = static_cast<u8>(nq - 1); }
                    rc_busy_sum += busy; ++rc_busy_n;
                    if (nq < rc_qp_min) { rc_qp_min = nq; }
                    if (nq > rc_qp_max) { rc_qp_max = nq; }
                    if (nq != cur_qp) {
                        cur_qp = nq;
                        s.SetSetupByte(SetupRcQpI, cur_qp);
                        if (gop != 0) { prep_p(g_nvab_armed ? ab_variant : ProductionVariant); }
                    }
                }
                if (ab && ab_frames >= 600) {
                    const u64 now_ab = armTicksToNs(armGetSystemTick());
                    const u64 mbs = ab_intra + ab_inter;
                    const u64 fps_x10 = now_ab > ab_t0 ? static_cast<u64>(ab_frames) * UINT64_C(10000000000) / (now_ab - ab_t0) : 0;
                    LogLine("   nvab #%u %s: %u P avg %llu KB, %u I avg %llu KB, P intra %llu.%llu%%, P avgQP %llu, %llu.%llu fps -> %llu Mbps at 60",
                            ab_window, AbName[ab_variant],
                            ab_np, static_cast<unsigned long long>(ab_np ? ab_bytes_p / ab_np / 1024 : 0),
                            ab_ni, static_cast<unsigned long long>(ab_ni ? ab_bytes_i / ab_ni / 1024 : 0),
                            static_cast<unsigned long long>(mbs ? ab_intra * 1000 / mbs / 10 : 0), static_cast<unsigned long long>(mbs ? ab_intra * 1000 / mbs % 10 : 0),
                            static_cast<unsigned long long>(ab_np ? ab_qp / ab_np : 0),
                            static_cast<unsigned long long>(fps_x10 / 10), static_cast<unsigned long long>(fps_x10 % 10),
                            static_cast<unsigned long long>((ab_bytes_p + ab_bytes_i) * 8 * 60 / ab_frames / 1000000));
                    ab_bytes_p = ab_bytes_i = ab_intra = ab_inter = ab_qp = 0;
                    ab_np = ab_ni = ab_frames = 0;
                    ab_t0 = now_ab;
                    ++ab_window;
                    ab_variant = (ab_variant + 1) & 3;
                    prep_p(ab_variant);
                    need_idr = true;
                }
                if (live && g_viewer_key_request.exchange(false, std::memory_order_relaxed)) { need_idr = true; ++key_requests; }
                const bool wired = g_net_wired.load(std::memory_order_relaxed);
                const u32 gop_now = keyframe::Choose(g_tx_net, live, uvc, gop, g_viewer_can_ask.load(std::memory_order_relaxed), wired,
                                                     g_usb_gop.load(std::memory_order_relaxed));
                if (gop_now != gop && !said_long_gop) {
                    said_long_gop = true;
                    if (g_tx_net) {
                        LogLine("   nvstream: network keyframes on demand + every %u frames (%s; setting: %u)",
                                gop_now, wired ? "wired" : "wifi", gop);
                    } else {
                        LogLine("   nvstream: USB keyframes on demand + every %u frames (setting: %u; usb_keyframe_interval = 0 turns this off)",
                                gop_now, gop);
                    }
                }
                if (gop == 0 || need_idr || pos >= gop_now) { pos = 0; }
                NvfJob j;
                j.setup = L.off_setup;
                j.ref_out = refs[pos & 1];
                j.cur = curs[k];
                j.cur_uv = curs_uv[k];
                if (pos == 0) {
                    s.SetSetupByte(SetupIdrPicId, static_cast<u8>(idrs & 1));
                } else {
                    const u16 fn = static_cast<u16>(pos % nvenc_grc_p::MaxFrameNum);
                    const u16 poc = static_cast<u16>((2 * pos) % nvenc_grc_p::MaxPocLsb);
                    std::memcpy(s.x.a + L.off_setup_p + SetupFrameNum, std::addressof(fn), 2);
                    std::memcpy(s.x.a + L.off_setup_p + SetupFrameNum + 2, std::addressof(poc), 2);
                    armDCacheFlush(s.x.a + L.off_setup_p + (SetupFrameNum & ~0x3Fu), 0x40);
                    j.setup  = L.off_setup_p;
                    j.ref_in = refs[(pos - 1) & 1];
                    j.me_in  = mes[(pos - 1) & 1];
                    j.me_out = mes[pos & 1];
                }
                fl = InFlight{ true, i, pic++, 0, 0, pos == 0, pos, present_ns, rn, t2 - t1, t0, t1, s.last_flush_ns, s.dyn_out, s.dyn_center };
                if (NvfSubmit(s.x, fl.pic, j, std::addressof(fl.fence), std::addressof(fl.t_submit)) != 0) {
                    fl.active = false; why = "NVENC submit rejected"; end = StreamEnd::Failed; break;
                }
                if (!pipe && collect(fl)) { break; }
                const u64 t3s = armTicksToNs(armGetSystemTick());
                lp_cap += t1 - t0; lp_vic += t2 - t1; lp_col += t2c - t2; lp_sub += t3s - t2c; ++lp_n;
                lt_cap += t1 - t0; lt_vic += t2 - t1; lt_col += t2c - t2; lt_sub += t3s - t2c; ++lt_n;
                if (pace_on) {
                    pace_last_t0 = t0;
                    pace_work += rn + (t3s - t1);
                    if (++pace_n >= 60) {
                        const u64 pavg = pace_work / pace_n;
                        const u32 pnd = pace::Divisor(pace_fps, pavg, pace_div);
                        if (pnd != pace_div) {
                            LogLine("   nvstream: paced to %u fps (work avg %llu us)", 60 / pnd, static_cast<unsigned long long>(pavg / 1000));
                        }
                        pace_div = pnd;
                        pace_work = 0; pace_n = 0;
                    }
                }

                if (live && s.size_check_ok) {
                    const s32 sl = g_queue_slot.load(std::memory_order_relaxed);
                    if (const char *r = size_check.Step(dbg, s.SlotAddr(slot_base, sl), sl, s.src.stride_px, s.src.blk_h_log2)) {
                        ++size_changes;
                        why = r;
                        end = StreamEnd::Reconfigure;
                        break;
                    }
                }
                if ((i + 1) % progress_every == 0) {
                    log_stalls();
                    {
                        const u64 wn = armTicksToNs(armGetSystemTick());
                        const u32 wd = w_n ? w_n : 1;
                        const u32 fw = s.fence_waits - w_fwait0;
                        const u64 wfps = wn > w_t0 ? static_cast<u64>(sent - w_sent0) * UINT64_C(10000000000) / (wn - w_t0) : 0;
                        {
                            u32 cpu_hz = 0, emc_hz = 0;
                            static_cast<void>(ClockRateOf(0x40000001u , std::addressof(cpu_hz)));
                            static_cast<void>(ClockRateOf(0x40000039u , std::addressof(emc_hz)));
                            const u32 sn = sp_n ? sp_n : 1;
                            LogLine("   nvstream age split avg us: present->copied %llu, ->submitted %llu, ->encoded %llu, ->sent %llu (%u frames); CPU %u MHz, memory %u MHz",
                                    static_cast<unsigned long long>(sp_copied / sn / 1000), static_cast<unsigned long long>(sp_submit / sn / 1000),
                                    static_cast<unsigned long long>(sp_enc / sn / 1000), static_cast<unsigned long long>(sp_sent / sn / 1000), sp_n,
                                    cpu_hz / 1000000, emc_hz / 1000000);
                            sp_copied = sp_submit = sp_enc = sp_sent = 0; sp_n = 0;
                        }
                        if (w_u_n != 0) {
                            LogLine("   nvstream USB per frame avg us: wait for the previous transfer %llu, audio %llu (%u sends), header %llu, post %llu",
                                    static_cast<unsigned long long>(w_u_wait / w_u_n / 1000), static_cast<unsigned long long>(w_u_audio / w_u_n / 1000), w_u_audio_n,
                                    static_cast<unsigned long long>(w_u_hdr / w_u_n / 1000), static_cast<unsigned long long>(w_u_post / w_u_n / 1000));
                            w_u_wait = w_u_audio = w_u_hdr = w_u_post = 0; w_u_audio_n = w_u_n = 0;
                        }
                        if (lp_n != 0) {
                            const u64 per = wn > w_t0 ? (wn - w_t0) / lp_n : 0;
                            const u64 polls = s.mid_poll_sum - lp_poll0;
                            LogLine("   nvstream loop split per trip avg us (%u trips, %llu in all): get the picture %llu (mid-read polls %llu of it), VIC %llu, collect the last frame %llu, build+submit %llu, sleep %llu; the rest (logging, checks) %lld",
                                    lp_n, static_cast<unsigned long long>(per / 1000), static_cast<unsigned long long>(lp_cap / lp_n / 1000),
                                    static_cast<unsigned long long>(polls / lp_n / 1000), static_cast<unsigned long long>(lp_vic / lp_n / 1000),
                                    static_cast<unsigned long long>(lp_col / lp_n / 1000), static_cast<unsigned long long>(lp_sub / lp_n / 1000),
                                    static_cast<unsigned long long>(lp_slp / lp_n / 1000),
                                    static_cast<long long>((static_cast<long long>(per) - static_cast<long long>((lp_cap + lp_vic + lp_col + lp_sub + lp_slp) / lp_n)) / 1000));
                            lp_cap = lp_vic = lp_col = lp_sub = lp_slp = 0; lp_n = 0; lp_poll0 = s.mid_poll_sum;
                        }
                        LogLine("   nvstream window: %llu.%llu fps sent; per frame avg us: read %llu (flush %llu of it)  VIC %llu  NVENC wait %llu  GPU fence %llu; "
                                "presents: %u already drawn, %u waited for, %u skipped",
                                static_cast<unsigned long long>(wfps / 10), static_cast<unsigned long long>(wfps % 10),
                                static_cast<unsigned long long>(w_read / wd / 1000), static_cast<unsigned long long>(w_flush / wd / 1000),
                                static_cast<unsigned long long>(w_vic / wd / 1000), static_cast<unsigned long long>(w_enc / wd / 1000),
                                static_cast<unsigned long long>(fw ? (s.fence_wait_sum - w_fence0) / fw / 1000 : 0),
                                s.pick_ready - w_ready0, s.pick_waited - w_waited0, dropped - w_drop0);
#if SFT_FILTER_AB_PROBE
                        if (g_enlarged_window.exchange(false, std::memory_order_relaxed)) {
                            const bool two = g_enlarge_2tap.load(std::memory_order_relaxed);
                            LogLine("   filter A/B: that window enlarged with the %s filter - the next one uses the %s", two ? "2-tap" : "1-tap", two ? "1-tap" : "2-tap");
                            g_enlarge_2tap.store(!two, std::memory_order_relaxed);
                        }
#endif
                        w_n = 0; w_read = w_flush = w_vic = w_enc = 0; w_t0 = wn; w_sent0 = sent;
                        w_fwait0 = s.fence_waits; w_fence0 = s.fence_wait_sum;
                        w_ready0 = s.pick_ready; w_waited0 = s.pick_waited; w_drop0 = dropped;
                    }
                    u32 hz = 0;
                    if (!ClockRateOf(PcvModule_NVENC, std::addressof(hz)) || hz < 400000000u) {
                        ++clock_fixes;
                        static_cast<void>(s.EnsureClock("nvstream", std::addressof(hz)));
                    }
                    const u64 now = armTicksToNs(armGetSystemTick());
                    const u32 qn = g_queue_count.load(std::memory_order_relaxed);
                    const u64 gw_x10 = now > t_window ? static_cast<u64>(qn - q_window) * UINT64_C(10000000000) / (now - t_window) : 0;
                    q_window = qn; t_window = now;
                    LogLine("   nvstream: %u frames in %llu ms, %u sent, avg %llu B/frame (IDR %llu, P %llu), NVENC %u Hz; game presented %llu.%llu fps this window",
                            i + 1, static_cast<unsigned long long>((now - loop_t0) / 1000000), sent,
                            static_cast<unsigned long long>(done ? s_bytes / done : 0),
                            static_cast<unsigned long long>(n_i ? s_bytes_i / n_i : 0),
                            static_cast<unsigned long long>(n_p ? s_bytes_p / n_p : 0), hz,
                            static_cast<unsigned long long>(gw_x10 / 10), static_cast<unsigned long long>(gw_x10 % 10));
                    DebugPumpLogStats("nvstream");
                    if (g_tx_net && rc_busy_n) {
                        LogLine("   nvstream: network rate control - QP %u now, %u-%u this window (quality setting %u), sender busy %u%% on average",
                                cur_qp, rc_qp_min, rc_qp_max, qp, rc_busy_sum / rc_busy_n);
                        rc_busy_sum = rc_busy_n = 0; rc_qp_min = 51; rc_qp_max = 0;
                    }
                }
                const u64 tsl0 = armTicksToNs(armGetSystemTick());
                os::SleepThread(TimeSpan::FromMicroSeconds(500));
                {
                    const u64 sl = armTicksToNs(armGetSystemTick()) - tsl0;
                    lp_slp += sl; lt_slp += sl;
                }
            }
            if (fl.active) {
                if (end == StreamEnd::Done || end == StreamEnd::Reconfigure || end == StreamEnd::GameGone || end == StreamEnd::Disabled) {
                    static_cast<void>(collect(fl));
                } else {
                    nvenc_pic_stat_s st = {};
                    u64 en = 0;
                    fl.active = false;
                    if (NvfWaitDone(s.x, fl.pic, fl.fence, fl.t_submit, std::addressof(st), std::addressof(en)) == 2) {
                        if (!s.Stall("nvstream", "the last frame in flight")) { stalled = true; }
                    }
                }
            }
            if (uvc) {
                if (pending) { UvcWaitSent(); ++sent; pending = false; }
                UvcLogStats("nvstream");
            }
            if (pending && end != StreamEnd::ViewerGone) {
                size_t got = 0;
                if (UsbWaitAsync(urb, std::addressof(got))) { ++sent; }
            }
            g_vic_quiet = false;
            const u64 wall = armTicksToNs(armGetSystemTick()) - loop_t0;
            const u32 presented = g_queue_count.load(std::memory_order_relaxed) - q0;
            const u32 nd = done ? done : 1;
            const u64 fps_x10 = wall ? static_cast<u64>(sent) * UINT64_C(10000000000) / wall : 0;
            const u64 game_x10 = wall ? static_cast<u64>(presented) * UINT64_C(10000000000) / wall : 0;
            g_streaming_now.store(false, std::memory_order_relaxed);
            if (end == StreamEnd::GameGone || end == StreamEnd::Reconfigure) { LogDefer(true); }
            LogLine("   nvstream: stopped: %s", why);
            if (end == StreamEnd::ViewerGone) { g_viewer_crop.store(false, std::memory_order_relaxed); g_size_lock.store(0, std::memory_order_relaxed); }
            if (s.center_frames != 0) {
                LogLine("   nvstream: a centred crop in %u frames (the viewer showed only the middle)", s.center_frames);
            }
            if (s.dyn_frames != 0) {
                LogLine("   nvstream: dynamic resolution in %u frames, %ux%u to %ux%u", s.dyn_frames,
                        s.dyn_min >> 16, s.dyn_min & 0xFFFF, s.dyn_max >> 16, s.dyn_max & 0xFFFF);
            }
            if (audio_bytes != 0) {
                LogLine("   nvstream: game audio %llu KB in %u packets (%llu ms of sound)", static_cast<unsigned long long>(audio_bytes / 1024), audio_seq,
                        static_cast<unsigned long long>(audio_bytes / 192));
            }
            if (lt_n != 0) {
                const u64 per = (armTicksToNs(armGetSystemTick()) - lt_t0) / lt_n;
                LogLine("   nvstream loop split, session avg us per trip (%llu trips): get the picture %llu (mid-read polls %llu), VIC %llu, collect %llu, build+submit %llu, sleep %llu; trip %llu",
                        static_cast<unsigned long long>(lt_n), static_cast<unsigned long long>(lt_cap / lt_n / 1000),
                        static_cast<unsigned long long>(s.mid_poll_sum / lt_n / 1000), static_cast<unsigned long long>(lt_vic / lt_n / 1000),
                        static_cast<unsigned long long>(lt_col / lt_n / 1000), static_cast<unsigned long long>(lt_sub / lt_n / 1000),
                        static_cast<unsigned long long>(lt_slp / lt_n / 1000), static_cast<unsigned long long>(per / 1000));
            }
            for (u32 h = 0; h < 2; ++h) {
                const AbTotal &tt = ab_tot[h];
                if (tt.halves == 0) { continue; }
                const u32 tn = tt.n ? tt.n : 1;
                const u64 tfps = tt.ns ? static_cast<u64>(tt.n) * UINT64_C(10000000000) / tt.ns : 0;
                LogLine("   A/B session total, %s halves (%u): console age avg %llu us (copied %llu, encoded %llu); read avg %llu us; %u frames, %llu.%llu fps; collected inside a read %u",
                        h == 0 ? "ON " : "OFF", tt.halves, static_cast<unsigned long long>(tt.age / tn / 1000),
                        static_cast<unsigned long long>(tt.cp / tn / 1000), static_cast<unsigned long long>(tt.enc / tn / 1000),
                        static_cast<unsigned long long>(tt.read / tn / 1000), tt.n,
                        static_cast<unsigned long long>(tfps / 10), static_cast<unsigned long long>(tfps % 10), tt.mids);
            }
            LogLine("   nvstream: %u frames sent in %llu ms -> %llu.%llu fps (game presented %llu.%llu fps, %u presents skipped), %u encodes with errors",
                    sent, static_cast<unsigned long long>(wall / 1000000),
                    static_cast<unsigned long long>(fps_x10 / 10), static_cast<unsigned long long>(fps_x10 % 10),
                    static_cast<unsigned long long>(game_x10 / 10), static_cast<unsigned long long>(game_x10 % 10),
                    dropped, errs);
            LogLine("   nvstream: per frame avg us: read+flush %llu  VIC %llu  NVENC %llu  copy %llu  usb %llu; worst work %llu us",
                    static_cast<unsigned long long>(s_read / nd / 1000), static_cast<unsigned long long>(s_vic / nd / 1000),
                    static_cast<unsigned long long>(s_enc / nd / 1000), static_cast<unsigned long long>(s_copy / nd / 1000),
                    static_cast<unsigned long long>(s_usb / nd / 1000), static_cast<unsigned long long>(m_work / 1000));
            LogLine("   nvstream: avg %llu B/frame (max %llu) -> %llu Mbps at the achieved rate; %u IDR avg %llu B, %u P avg %llu B; clock re-ensured %u time(s)",
                    static_cast<unsigned long long>(s_bytes / nd), static_cast<unsigned long long>(m_bytes),
                    static_cast<unsigned long long>(wall ? s_bytes * 8 * 1000 / wall : 0),
                    n_i, static_cast<unsigned long long>(n_i ? s_bytes_i / n_i : 0),
                    n_p, static_cast<unsigned long long>(n_p ? s_bytes_p / n_p : 0), clock_fixes);
            LogLine("   nvstream: console latency, the game's present -> header sent: avg %llu us, max %llu us",
                    static_cast<unsigned long long>(s_age / nd / 1000), static_cast<unsigned long long>(m_age / 1000));
            LogLine("   nvstream: present fences: %u waited, avg %llu us, max %llu us, %u timed out (250 ms), %u presents without a readable fence; %u read(s) redone (the slot may have been reused while it was read)",
                    s.fence_waits, static_cast<unsigned long long>(s.fence_waits ? s.fence_wait_sum / s.fence_waits / 1000 : 0),
                    static_cast<unsigned long long>(s.fence_wait_max / 1000), s.fence_timeouts, s.fence_unknown, s.reread);
            LogLine("   nvstream: presents taken (M96): %u already drawn, %u waited for (the oldest not yet drawn), %u by the old latest-present rule, %u by seed content-sync (%u re-read, %u torn dropped)",
                    s.pick_ready, s.pick_waited, s.pick_legacy, s.pick_seeded, s.seed_reread, s.seed_dropped);
            LogLine("   nvstream: per-stage max us: read %llu  VIC %llu  NVENC %llu  USB wait %llu  copy %llu; %u frame(s) over 50 ms of work; %u USB wait(s) over 500 ms (IDR forced after each)",
                    static_cast<unsigned long long>(m_read / 1000), static_cast<unsigned long long>(m_vic / 1000),
                    static_cast<unsigned long long>(m_enc / 1000), static_cast<unsigned long long>(m_usbw / 1000),
                    static_cast<unsigned long long>(m_copy / 1000), n_stalls, viewer_gaps);
            for (u32 k = n_stalls_logged; k < (n_stalls < 8 ? n_stalls : 8); ++k) {
                LogLine("   nvstream: STALL at frame %u (+%llu ms): read %u us, VIC %u us, NVENC %u us, waiting for the previous USB transfer %u us, copy %u us",
                        stalls[k].frame, static_cast<unsigned long long>(stalls[k].at_ms), stalls[k].read_us, stalls[k].vic_us,
                        stalls[k].enc_us, stalls[k].usbw_us, stalls[k].copy_us);
            }
            DebugPumpLogStats("nvstream");
            if (size_changes != 0) { LogLine("   nvstream: ended to restart at the game's new picture size"); }
            if (key_requests != 0) { LogLine("   nvstream: %u keyframe(s) the viewer asked for (a new decoder, a recording)", key_requests); }
            if (!live && !stalled && last_sent != ~0u && last_sent == last_vic) {
                LogLine("   nvstream: last frame sent #%u, %s, %u frame(s) after its IDR", last_sent, last_pos == 0 ? "IDR" : "P", last_pos);
                const u32 kk = pipe ? (last_sent & 1) : 0;
                armDCacheFlush(s.x.a + curs[kk], L.luma);
                armDCacheFlush(s.x.a + curs_uv[kk], L.chroma);
                WriteEngineOutputToSd("sdmc:/nvstream-last-y.bin", s.x.a + curs[kk], L.luma, nullptr);
                WriteEngineOutputToSd("sdmc:/nvstream-last-uv.bin", s.x.a + curs_uv[kk], L.chroma, nullptr);
            }
            VicStage(stalled ? "ns:STALLED_left_open" : (sent == nframes ? "ns:DONE" : "ns:stopped"));
            return end;
        }

        void TryNvencPFrames(::ams::svc::Handle dbg, u64 slot_base, u32 vfd, u32 nvmap_fd,
                             u32 cmd_handle, u32 vsyncpt, u32 cfg_addr) {
            VicStage("np:1");
            const u32 n = g_nvp_n < 2 ? 2 : (g_nvp_n > 60 ? 60 : g_nvp_n);
            constexpr u8 Qp = 20;
            LogLine("   ---- P FRAMES (M83 nvp): 1 IDR + %u P from real frames, QP %u, grc's P setup and P job ----", n - 1, Qp);
            if (g_ind_buf == nullptr || g_ind_size < NvpAccBase + 0x100000) {
                LogLine("   nvp: capture region %zu KB, need %zu KB - not running", g_ind_size / 1024, (NvpAccBase + 0x100000) / 1024);
                return;
            }
            ON_SCOPE_EXIT { ClockWatchStop(); };
            NvfSession s;
            s.arena_base = NvpArenaBase;
            s.arena_size = NvpArenaSize;
            if (!s.Open(vfd, nvmap_fd, cmd_handle, vsyncpt, cfg_addr, "nvp")) { VicStage("np:open_FAILED"); return; }
            std::memcpy(s.x.a + NvpOffSetupP, nvenc_grc_p::Setup, sizeof(nvenc_grc_p::Setup));
            s.x.a[NvpOffSetupP + SetupRcQpP] = Qp;
            s.x.a[NvpOffSetupP + SetupRcQpI] = Qp;
            armDCacheFlush(s.x.a + NvpOffSetupP, 0x1000);
            s.SetSetupByte(SetupRcQpI, Qp);
            u32 hz = 0;
            if (!s.EnsureClock("nvp", std::addressof(hz))) { LogLine("   nvp: NOT submitting - NVENC clock not established"); VicStage("np:no_clock"); return; }
            {
                u64 rn = 0; u32 sg = 0;
                if (!s.Capture(dbg, slot_base, std::addressof(rn), std::addressof(sg))) { LogLine("   nvp: %s", SlotReadFailure()); return; }
                s.Decide("nvp");
            }

            u8 *const acc = g_ind_buf + NvpAccBase;
            const size_t acc_cap = g_ind_size - NvpAccBase;
            u32 lens[64] = {};
            size_t acc_len = 0;
            u32 done = 0, errs = 0;
            u64 s_enc_i = 0, s_enc_p = 0;
            const u32 refs[2] = { NvfOffRefOut, NvpOffRefB };
            const u32 mes[2]  = { NvpOffMeA, NvpOffMeB };
            u32 pic = NvfPictureIndex + 0x200;
            g_vic_quiet = true;
            for (u32 i = 0; i < n; ++i) {
                if (acc_cap - acc_len < 0x80000) { LogLine("   nvp: accumulator full after %u frames (%zu KB)", i, acc_len / 1024); break; }
                u64 rn = 0; u32 sg = 0;
                if (!s.Capture(dbg, slot_base, std::addressof(rn), std::addressof(sg))) { LogLine("   nvp: %s at frame %u", SlotReadFailure(), i); break; }
                if (!s.Vic("np:vic")) { LogLine("   nvp: VIC did not complete at frame %u", i); break; }
                NvfJob j;
                j.ref_out = refs[i & 1];
                if (i != 0) {
                    const u16 fn = static_cast<u16>(i % nvenc_grc_p::MaxFrameNum);
                    const u16 poc = static_cast<u16>((2 * i) % nvenc_grc_p::MaxPocLsb);
                    std::memcpy(s.x.a + NvpOffSetupP + SetupFrameNum, std::addressof(fn), 2);
                    std::memcpy(s.x.a + NvpOffSetupP + SetupFrameNum + 2, std::addressof(poc), 2);
                    armDCacheFlush(s.x.a + NvpOffSetupP + (SetupFrameNum & ~0x3Fu), 0x40);
                    j.setup  = NvpOffSetupP;
                    j.ref_in = refs[(i - 1) & 1];
                    j.me_in  = mes[(i - 1) & 1];
                    j.me_out = mes[i & 1];
                }
                nvenc_pic_stat_s st = {};
                u64 en = 0;
                const int r = NvfEncode(s.x, pic++, std::addressof(st), std::addressof(en), j);
                if (r == 1) { LogLine("   nvp: submit REJECTED at frame %u", i); break; }
                if (r == 2) { g_vic_quiet = false; s.Stall("nvp", i == 0 ? "the IDR" : "a P frame"); VicStage("np:STALLED_left_open"); break; }
                const u32 bytes = st.total_bit_count / 8;
                const bool bad = st.ucode_error_status != 0 || bytes == 0 || st.bitstream_start_pos != 0;
                if (bad) { ++errs; }
                LogLine("   nvp[%2u]: %s pic_type %u  %6u B  intra/inter MBs %4u/%4u  avgQP %u  ucode %#x  %llu us",
                        i, i == 0 ? "IDR" : "P  ", st.pic_type, bytes, st.intra_mb_count, st.inter_mb_count, st.avgQP,
                        st.ucode_error_status, static_cast<unsigned long long>(en / 1000));
                if (i == 0) { s_enc_i += en; } else { s_enc_p += en; }
                if (bad || acc_len + bytes > acc_cap) { break; }
                armDCacheFlush(s.x.a + NvfOffBits, bytes);
                std::memcpy(acc + acc_len, s.x.a + NvfOffBits, bytes);
                acc_len += bytes;
                lens[i] = bytes;
                ++done;
            }
            g_vic_quiet = false;
            if (s.keep_open) { return; }
            LogLine("   nvp: %u of %u frames encoded, %u with errors; IDR %u B, P avg %llu B; NVENC IDR %llu us, P avg %llu us",
                    done, n, errs, lens[0], static_cast<unsigned long long>(done > 1 ? (acc_len - lens[0]) / (done - 1) : 0),
                    static_cast<unsigned long long>(s_enc_i / 1000),
                    static_cast<unsigned long long>(done > 1 ? s_enc_p / (done - 1) / 1000 : 0));
            if (done == 0) { VicStage("np:nothing"); return; }

            u32 index[1 + 64] = { done };
            for (u32 i = 0; i < done; ++i) { index[1 + i] = lens[i]; }
            std::memcpy(g_stage_buf + 0x10000, index, sizeof(index));
            WriteSdVerified("sdmc:/nvp-index.bin", g_stage_buf + 0x10000, 4 * (1 + done), g_stage_buf + 0x20000, nullptr);
            char path[48];
            for (size_t off = 0, k = 0; off < acc_len; off += 0x100000, ++k) {
                const size_t len = (acc_len - off < 0x100000) ? acc_len - off : 0x100000;
                std::snprintf(path, sizeof(path), "sdmc:/nvp-stream-%u.bin", static_cast<u32>(k));
                std::memcpy(g_stage_buf, acc + off, len);
                WriteSdVerified(path, g_stage_buf, len, g_stage_buf + ((len + 0xFFF) & ~static_cast<size_t>(0xFFF)), nullptr);
            }
            armDCacheFlush(s.x.a + NvfOffCur, NvfLuma + NvfChroma);
            WriteEngineOutputToSd("sdmc:/nvp-last-y.bin", s.x.a + NvfOffCur, NvfLuma, nullptr);
            WriteEngineOutputToSd("sdmc:/nvp-last-uv.bin", s.x.a + NvfOffCurUV, NvfChroma, nullptr);
            VicStage(done == n ? "np:DONE" : "np:partial");
        }

        void TryNvenc1080(::ams::svc::Handle dbg, u64 slot_base, u32 vfd, u32 nvmap_fd,
                          u32 cmd_handle, u32 vsyncpt, u32 cfg_addr) {
            VicStage("n1:1");
            const NvfLayout &L = Layout1080();
            const u32 nframes = g_nv1080_n < 10 ? 10 : (g_nv1080_n > 1200 ? 1200 : g_nv1080_n);
            LogLine("   ---- 1080p NVENC (M90): grc's setups resized to 1920x1080 on real frames; GOP of 10; %u timed frames ----", nframes);
            ON_SCOPE_EXIT { ClockWatchStop(); };
            NvfSession s;
            s.lay = std::addressof(L);
            s.arena_size = L.arena_size_p;
            if (!s.Open(vfd, nvmap_fd, cmd_handle, vsyncpt, cfg_addr, "nv1080")) { VicStage("n1:open_FAILED"); return; }
            std::memcpy(s.x.a + L.off_setup_p, L.p_setup, 0x1000);
            constexpr u8 Qp = 20;
            s.x.a[L.off_setup_p + SetupRcQpP] = Qp;
            s.x.a[L.off_setup_p + SetupRcQpI] = Qp;
            armDCacheFlush(s.x.a + L.off_setup_p, 0x1000);
            u32 hz = 0;
            if (!s.EnsureClock("nv1080", std::addressof(hz))) { LogLine("   nv1080: NOT submitting - NVENC clock not established"); VicStage("n1:no_clock"); return; }

            auto save = [&](const char *path, const u8 *src, size_t len) {
                armDCacheFlush(const_cast<u8 *>(src), len);
                WriteSdVerified(path, src, len, g_ind_buf, nullptr);
            };
            auto bits_len = [&](const nvenc_pic_stat_s &st) -> u32 {
                u32 len = st.total_bit_count / 8 + st.bitstream_start_pos + 16;
                return len > L.bits_size ? L.bits_size : len;
            };

            u64 rn = 0; u32 sg = 0;
            if (!s.Capture(dbg, slot_base, std::addressof(rn), std::addressof(sg))) { LogLine("   nv1080: %s", SlotReadFailure()); VicStage("n1:read_FAILED"); return; }
            s.Decide("nv1080");
            const bool upscaled = s.corner;
            if (upscaled) {
                LogLine("   nv1080: HANDHELD content - the VIC scales the 1280x720 picture UP to 1920x1080: a real 1080p encode, not a native 1080p picture (dock the console for that)");
            } else {
                LogLine("   nv1080: DOCKED content - the VIC converts the game's 1920x1080 picture 1:1");
                WriteSdVerified("sdmc:/nv1080-0-src.bin", g_ind_buf, FbBlockRow, g_stage_buf, nullptr);
            }
            u32 pic = NvfPictureIndex + 0x300;
            char path[48];

            g_vic_quiet = false;
            const u64 v0 = armTicksToNs(armGetSystemTick());
            if (!s.Vic("n1:vic0")) { LogLine("   nv1080: VIC did not complete"); VicStage("n1:vic_FAILED"); return; }
            LogLine("   nv1080[0]: slot read %llu us, VIC -> 1920x1080 BT.709 NV12 in %llu us incl. logging (NVENC %u Hz)",
                    static_cast<unsigned long long>(rn / 1000), static_cast<unsigned long long>((armTicksToNs(armGetSystemTick()) - v0) / 1000), hz);
            for (const u8 q : { static_cast<u8>(16), static_cast<u8>(20), static_cast<u8>(24) }) {
                s.SetSetupByte(SetupRcQpI, q);
                NvfJob j;
                j.setup = L.off_setup;
                j.ref_out = L.off_ref_out;
                nvenc_pic_stat_s st = {};
                u64 en = 0;
                const int r = NvfEncode(s.x, pic++, std::addressof(st), std::addressof(en), j);
                if (r == 1) { LogLine("   nv1080[0] QP %u: submit REJECTED", q); VicStage("n1:rejected"); return; }
                if (r == 2) { s.Stall("nv1080", "frame 0"); VicStage("n1:STALLED_left_open"); return; }
                LogLine("   nv1080[0] QP %2u: %u B in %llu us (status: error %u ucode %#x pic_type %u avgQP %u intra %u)",
                        q, st.total_bit_count / 8, static_cast<unsigned long long>(en / 1000),
                        st.error_status, st.ucode_error_status, st.pic_type, st.avgQP, st.intra_mb_count);
                std::snprintf(path, sizeof(path), "sdmc:/nv1080-q%u-status.bin", q);
                save(path, s.x.a + L.off_status, 0x1000);
                std::snprintf(path, sizeof(path), "sdmc:/nv1080-q%u-bits.bin", q);
                save(path, s.x.a + L.off_bits, bits_len(st));
            }
            save("sdmc:/nv1080-0-y.bin",  s.x.a + L.off_cur,    L.luma);
            save("sdmc:/nv1080-0-uv.bin", s.x.a + L.off_cur_uv, L.chroma);

            const u32 refs[2] = { L.off_ref_out, L.off_ref_b };
            const u32 mes[2]  = { L.off_me_a, L.off_me_b };
            s.SetSetupByte(SetupRcQpI, Qp);
            u32 idrs = 0;
            auto encode_at = [&](u32 pos, nvenc_pic_stat_s *st, u64 *en) -> int {
                NvfJob j;
                j.setup = L.off_setup;
                j.ref_out = refs[pos & 1];
                if (pos == 0) {
                    s.SetSetupByte(SetupIdrPicId, static_cast<u8>(idrs & 1));
                } else {
                    const u16 fn = static_cast<u16>(pos % nvenc_grc_p::MaxFrameNum);
                    const u16 poc = static_cast<u16>((2 * pos) % nvenc_grc_p::MaxPocLsb);
                    std::memcpy(s.x.a + L.off_setup_p + SetupFrameNum, std::addressof(fn), 2);
                    std::memcpy(s.x.a + L.off_setup_p + SetupFrameNum + 2, std::addressof(poc), 2);
                    armDCacheFlush(s.x.a + L.off_setup_p + (SetupFrameNum & ~0x3Fu), 0x40);
                    j.setup  = L.off_setup_p;
                    j.ref_in = refs[(pos - 1) & 1];
                    j.me_in  = mes[(pos - 1) & 1];
                    j.me_out = mes[pos & 1];
                }
                const int r = NvfEncode(s.x, pic++, st, en, j);
                if (r == 0 && pos == 0) { ++idrs; }
                return r;
            };

            constexpr u32 GopN = 10;
            u8 *const acc = g_stage_buf;
            const size_t acc_cap = 3 * StreamStageSize;
            u32 lens[GopN] = {};
            size_t acc_len = 0;
            u32 gop_done = 0;
            g_vic_quiet = true;
            for (u32 i = 0; i < GopN; ++i) {
                if (!s.Capture(dbg, slot_base, std::addressof(rn), std::addressof(sg))) { LogLine("   nv1080 GOP: %s at frame %u", SlotReadFailure(), i); break; }
                if (!s.Vic("n1:gop")) { LogLine("   nv1080 GOP: VIC did not complete at frame %u", i); break; }
                nvenc_pic_stat_s st = {};
                u64 en = 0;
                const int r = encode_at(i, std::addressof(st), std::addressof(en));
                if (r == 1) { LogLine("   nv1080 GOP: submit REJECTED at frame %u", i); break; }
                if (r == 2) { g_vic_quiet = false; s.Stall("nv1080", i == 0 ? "the GOP's IDR" : "a GOP P frame"); VicStage("n1:STALLED_left_open"); return; }
                const u32 bytes = st.total_bit_count / 8;
                LogLine("   nv1080 GOP[%u]: %s pic_type %u  %7u B  intra/inter MBs %4u/%4u  avgQP %u  ucode %#x  %llu us",
                        i, i == 0 ? "IDR" : "P  ", st.pic_type, bytes, st.intra_mb_count, st.inter_mb_count, st.avgQP,
                        st.ucode_error_status, static_cast<unsigned long long>(en / 1000));
                if (st.ucode_error_status != 0 || bytes == 0 || st.bitstream_start_pos != 0 || acc_len + bytes > acc_cap) { break; }
                armDCacheFlush(s.x.a + L.off_bits, bytes);
                std::memcpy(acc + acc_len, s.x.a + L.off_bits, bytes);
                acc_len += bytes;
                lens[i] = bytes;
                ++gop_done;
            }
            g_vic_quiet = false;
            if (gop_done != 0) {
                u32 index[1 + GopN] = { gop_done };
                for (u32 i = 0; i < gop_done; ++i) { index[1 + i] = lens[i]; }
                std::memcpy(g_ind_buf + FbSlotSize - 0x1000, index, sizeof(index));
                WriteSdVerified("sdmc:/nv1080-gop-index.bin", g_ind_buf + FbSlotSize - 0x1000, 4 * (1 + gop_done), g_ind_buf, nullptr);
                WriteSdVerified("sdmc:/nv1080-gop.bin", acc, acc_len, g_ind_buf, nullptr);
                if (gop_done == GopN) {
                    save("sdmc:/nv1080-gop-last-y.bin",  s.x.a + L.off_cur,    L.luma);
                    save("sdmc:/nv1080-gop-last-uv.bin", s.x.a + L.off_cur_uv, L.chroma);
                }
            }
            LogLine("   nv1080 GOP: %u of %u frames kept (%zu B)", gop_done, GopN, acc_len);

            u64 s_read = 0, s_vic = 0, s_enc = 0, m_read = 0, m_vic = 0, m_enc = 0, m_work = 0, s_work = 0, s_wait = 0;
            u64 b_i = 0, b_p = 0;
            u32 n_i = 0, n_p = 0, done = 0, errs = 0, pos = 0;
            const u32 q0 = g_queue_count.load(std::memory_order_relaxed);
            const u64 loop_t0 = armTicksToNs(armGetSystemTick());
            g_vic_quiet = true;
            for (u32 i = 0; i < nframes; ++i) {
                const u64 t0 = armTicksToNs(armGetSystemTick());
                if (!s.Capture(dbg, slot_base, std::addressof(rn), std::addressof(sg))) { LogLine("   nv1080: %s at timed frame %u", SlotReadFailure(), i); break; }
                const u64 t1 = armTicksToNs(armGetSystemTick());
                if (!s.Vic("n1:vic")) { LogLine("   nv1080: VIC did not complete at timed frame %u", i); break; }
                const u64 t2 = armTicksToNs(armGetSystemTick());
                if (pos >= 60) { pos = 0; }
                nvenc_pic_stat_s st = {};
                u64 en = 0;
                const int r = encode_at(pos, std::addressof(st), std::addressof(en));
                if (r == 1) { LogLine("   nv1080: submit REJECTED at timed frame %u", i); break; }
                if (r == 2) { g_vic_quiet = false; s.Stall("nv1080", "the timed loop"); VicStage("n1:STALLED_left_open"); return; }
                const u64 t3 = armTicksToNs(armGetSystemTick());
                const u32 bytes = st.total_bit_count / 8;
                if (st.ucode_error_status != 0 || bytes == 0) { ++errs; pos = 0; continue; }
                if (pos == 0) { b_i += bytes; ++n_i; } else { b_p += bytes; ++n_p; }
                ++pos;
                const u64 dr = rn, dv = t2 - t1, de = t3 - t2, dw = dr + dv + de;
                s_read += dr; s_vic += dv; s_enc += de; s_work += dw; s_wait += (t1 - t0) - dr;
                if (dr > m_read) { m_read = dr; }
                if (dv > m_vic) { m_vic = dv; }
                if (de > m_enc) { m_enc = de; }
                if (dw > m_work) { m_work = dw; }
                ++done;
                os::SleepThread(TimeSpan::FromMicroSeconds(500));
            }
            g_vic_quiet = false;
            const u64 wall = armTicksToNs(armGetSystemTick()) - loop_t0;
            const u32 presented = g_queue_count.load(std::memory_order_relaxed) - q0;
            const u32 nd = done ? done : 1;
            const u64 fps_x10 = wall ? static_cast<u64>(done) * UINT64_C(10000000000) / wall : 0;
            const u64 game_x10 = wall ? static_cast<u64>(presented) * UINT64_C(10000000000) / wall : 0;
            LogLine("   nv1080: %u of %u timed frames in %llu ms -> %llu.%llu fps (game presented %llu.%llu fps), %u with errors, content %s",
                    done, nframes, static_cast<unsigned long long>(wall / 1000000),
                    static_cast<unsigned long long>(fps_x10 / 10), static_cast<unsigned long long>(fps_x10 % 10),
                    static_cast<unsigned long long>(game_x10 / 10), static_cast<unsigned long long>(game_x10 % 10),
                    errs, upscaled ? "UPSCALED from handheld 720p" : "native 1080p (docked)");
            LogLine("   nv1080: per frame avg/max us: read+flush %llu/%llu  VIC %llu/%llu  NVENC %llu/%llu  work %llu/%llu  (60 fps budget 16667); waiting for presents + fences avg %llu",
                    static_cast<unsigned long long>(s_read / nd / 1000), static_cast<unsigned long long>(m_read / 1000),
                    static_cast<unsigned long long>(s_vic / nd / 1000), static_cast<unsigned long long>(m_vic / 1000),
                    static_cast<unsigned long long>(s_enc / nd / 1000), static_cast<unsigned long long>(m_enc / 1000),
                    static_cast<unsigned long long>(s_work / nd / 1000), static_cast<unsigned long long>(m_work / 1000),
                    static_cast<unsigned long long>(s_wait / nd / 1000));
            LogLine("   nv1080: %u IDR avg %llu B, %u P avg %llu B -> %llu Mbps at 60 fps",
                    n_i, static_cast<unsigned long long>(n_i ? b_i / n_i : 0), n_p, static_cast<unsigned long long>(n_p ? b_p / n_p : 0),
                    static_cast<unsigned long long>(done ? (b_i + b_p) / nd * 8 * 60 / 1000000 : 0));
            DebugPumpLogStats("nv1080");
            VicStage(done == nframes ? "n1:DONE" : "n1:partial");
        }

        constexpr u64 GrcProgramIdValue = 0x0100000000000035ull;

        struct ScanHit { u64 addr; u32 kind; u32 word0, word1, word2, word3; };

        void TryGrcScan() {
            VicStage("grcscan:1");
            LogLine("   ---- GRC OBSERVER (read-only; no channel, no submit) ----");

            os::ProcessId pid = {};
            {
                const Result r = ::ams::pm::dmnt::GetProcessId(std::addressof(pid), ncm::ProgramId{GrcProgramIdValue});
                LogLine("   pm:dmnt GetProcessId(%016llx) rc=0x%x -> pid=%llu",
                        static_cast<unsigned long long>(GrcProgramIdValue), r.GetValue(),
                        static_cast<unsigned long long>(pid.value));
                if (R_FAILED(r) || pid.value == 0) {
                    LogLine("   grc is not running - nothing to observe");
                    VicStage("grcscan:no_proc");
                    return;
                }
            }

            ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
            const Result r_attach = ::ams::svc::DebugActiveProcess(std::addressof(dbg), pid.value);
            LogLine("   DebugActiveProcess rc=0x%x", r_attach.GetValue());
            if (R_FAILED(r_attach)) { VicStage("grcscan:no_attach"); return; }

            u32 nev = 0;
            {
                ::ams::svc::DebugEventInfo ev;
                while (nev < 256 && R_SUCCEEDED(::ams::svc::GetDebugEvent(std::addressof(ev), dbg))) { ++nev; }
                const Result rc = ::ams::svc::ContinueDebugEvent(dbg,
                                      ::ams::svc::ContinueFlag_ExceptionHandled | ::ams::svc::ContinueFlag_ContinueAll,
                                      nullptr, 0);
                LogLine("   drained %u events; ContinueDebugEvent rc=0x%x -> grc %s",
                        nev, rc.GetValue(), R_SUCCEEDED(rc) ? "RESUMED" : "STILL HALTED");
            }

            static constexpr u32 NvencMagics[4] = { 0xd0b70006u, 0xc1b70006u, 0xc0b70006u, 0xa0b70006u };
            const u32 setcl_nvenc = vic::Host1xOpcodeSetClass(0, 0x21, 0);
            const u32 setcl_nvjpg = vic::Host1xOpcodeSetClass(0, 0xC0, 0);
            constexpr u32 MethodDrvPicSetup = 0x710u >> 2;

            if (g_ind_buf == nullptr || g_ind_size < 64 * 1024) {
                LogLine("   no scratch buffer (arm \"vic\" too) - skipping the scan");
                static_cast<void>(::ams::svc::CloseHandle(dbg));
                VicStage("grcscan:no_scratch");
                return;
            }

            enum : u32 { Hit_SetclEnc = 5, Hit_SetclJpg = 6, Hit_Method = 7, Hit_SetclVic = 8, Hit_Status = 9, NumKinds = 10 };
            const u32 setcl_vic = vic::Host1xOpcodeSetClass(0, 0x5D, 0);
            u32 status_err_hist[4] = {}, status_ucode_nonzero = 0;
            u32 nvic_dumps = 0;
            static constexpr u32 MaxVicDumps = 4;
            static constexpr u32 MaxLogged = 32;
            ScanHit hits[MaxLogged] = {};
            u32 nlogged = 0, nkind[NumKinds] = {};
            u32 nregions = 0, nskipped = 0;
            u64 scanned = 0, skipped_bytes = 0;
            constexpr u64 MaxRegion = 8ull * 1024 * 1024;
            constexpr u64 ScanCap   = 256ull * 1024 * 1024;
            constexpr u64 ChunkSize = 64 * 1024;

            constexpr size_t RecCap = 0x40000;
            u8 *const rec = g_stage_buf;
            size_t rec_len = 0;
            const bool can_dump = rec != nullptr && 2 * RecCap <= StreamStageSize && g_ind_size >= FbBlockRowStage + 2 * RecCap;
            auto rec_put = [&](u16 kind, u32 rq, u32 a, u32 b, const void *payload, u32 len) -> u8 * {
                if (!can_dump || rec_len + 32 + len > RecCap) { return nullptr; }
                const u32 ms = static_cast<u32>(armTicksToNs(armGetSystemTick()) / UINT64_C(1000000));
                const u32 hdr[8] = { 0x4352564Eu, static_cast<u32>(kind) | (32u << 16), 0, rq, a, b, len, ms };
                std::memcpy(rec + rec_len, hdr, sizeof(hdr));
                u8 *body = rec + rec_len + 32;
                if (payload != nullptr) { std::memcpy(body, payload, len); }
                rec_len += 32 + len;
                return body;
            };
            char note[160];
            auto rec_note = [&](int n) {
                if (n > 0) { rec_put(9, 0, 0, 0, note, static_cast<u32>(n < static_cast<int>(sizeof(note)) ? n : sizeof(note) - 1)); }
            };
            u32 ndumped = 0;
            auto dump = [&](u16 kind, u32 rq, u64 va, u32 len) -> u8 * {
                u8 *body = rec_put(kind, rq, static_cast<u32>(va >> 32), static_cast<u32>(va), nullptr, len);
                if (body == nullptr) { return nullptr; }
                if (!DbgRead(dbg, body, va, len)) {
                    std::memset(body, 0, len);
                    rec_note(std::snprintf(note, sizeof(note), "dump @ %#llx: DbgRead failed - payload zeroed",
                                           static_cast<unsigned long long>(va)));
                    return nullptr;
                }
                ++ndumped;
                return body;
            };

            static constexpr u32 MaxSetups = 6;
            u64 setup_va[MaxSetups] = {};
            u32 nsetups = 0;
            auto plausible = [](const u8 *p) {
                nvenc_h264_drv_pic_setup_s st;
                std::memcpy(std::addressof(st), p, sizeof(st));
                const u32 w = st.input_cfg.frame_width_minus1 + 1u, h = st.input_cfg.frame_height_minus1 + 1u;
                return st.sps_data.profile_idc != 0 && w >= 16 && w <= 4096 && h >= 16 && h <= 4096;
            };

            struct Win { u64 va; u32 len; u32 hit_off; };
            static constexpr u32 MaxWins = 12;
            Win wins[MaxWins] = {};
            u32 nwins = 0;
            u32 nmagic_dumps = 0, nsetcl_dumps = 0;
            static constexpr u32 MaxMagicDumps = 8, MaxSetclDumps = 4;

            u64 addr = 0;
            for (u32 steps = 0; steps < 8000; ++steps) {
                ::ams::svc::MemoryInfo mi = {};
                ::ams::svc::PageInfo   pi = {};
                if (!DbgQuery(dbg, std::addressof(mi), std::addressof(pi), addr)) { break; }
                if (mi.size == 0) { break; }

                const u32 perm  = static_cast<u32>(mi.permission);
                const u32 state = static_cast<u32>(mi.state);
                const bool readable = (perm & ::ams::svc::MemoryPermission_Read) != 0;
                const bool interesting = readable && mi.size >= 0x1000 &&
                                         state != static_cast<u32>(::ams::svc::MemoryState_Free) &&
                                         state != static_cast<u32>(::ams::svc::MemoryState_Code) &&
                                         state != static_cast<u32>(::ams::svc::MemoryState_AliasCode);

                if (interesting && mi.size > MaxRegion) {
                    ++nskipped;
                    skipped_bytes += mi.size;
                    rec_note(std::snprintf(note, sizeof(note), "skipped region %#llx+%#llx state %#x perm %#x (over 8 MB)",
                                           static_cast<unsigned long long>(mi.base_address),
                                           static_cast<unsigned long long>(mi.size), state, perm));
                } else if (interesting && scanned < ScanCap) {
                    ++nregions;
                    u32 region_wins = 0;
                    const u64 region_end = mi.base_address + mi.size;
                    for (u64 off = 0; off < mi.size && scanned < ScanCap; off += ChunkSize) {
                        const u64 n = (mi.size - off < ChunkSize) ? (mi.size - off) : ChunkSize;
                        if (!DbgRead(dbg, g_ind_buf, mi.base_address + off, n)) { break; }
                        scanned += n;
                        const u32 *w = reinterpret_cast<const u32 *>(g_ind_buf);
                        const u64 nw = n / 4;
                        for (u64 i = 0; i + 4 < nw; ++i) {
                            u32 kind = 0;
                            for (u32 m = 0; m < 4; ++m) { if (w[i] == NvencMagics[m]) { kind = 1 + m; } }
                            const u32 nx = w[i + 1] >> 16;
                            const bool method_next = nx == 0x1010 || nx == 0x2010 || nx == 0x3010 || nx == 0x100b;
                            if (kind == 0 && w[i] == setcl_nvenc && method_next) { kind = Hit_SetclEnc; }
                            if (kind == 0 && w[i] == setcl_nvjpg && method_next) { kind = Hit_SetclJpg; }
                            if (kind == 0 && w[i] == setcl_vic   && method_next) { kind = Hit_SetclVic; }
                            if (kind == 0 && i + 11 < nw) {
                                const u32 mbs = (w[i + 10] & 0xFFFF) + (w[i + 10] >> 16);
                                const u32 pt = w[i + 4] & 0xFFFF, ns = w[i + 4] >> 16;
                                if (mbs == 3600 && pt <= 3 && ns >= 1 && ns <= 64 &&
                                    w[i + 2] != 0 && w[i + 2] <= 1382400u * 8 && w[i + 9] <= 1382400u) {
                                    kind = Hit_Status;
                                    ++status_err_hist[w[i + 1] & 3];
                                    if ((w[i + 1] >> 2) != 0) { ++status_ucode_nonzero; }
                                    if (nkind[Hit_Status] < 32) rec_note(std::snprintf(note, sizeof(note),
                                             "grc status @ %#llx: picture_index %#x error_status %u ucode %#x bits %u pic_type %u slices %u avgQP %u intra/inter %u/%u",
                                             static_cast<unsigned long long>(mi.base_address + off + i * 4),
                                             w[i], w[i + 1] & 3, w[i + 1] >> 2, w[i + 2], pt, ns, w[i + 5] >> 16,
                                             w[i + 10] & 0xFFFF, w[i + 10] >> 16));
                                }
                            }
                            const u32 op = w[i] >> 16;
                            if (kind == 0 && (((op == 0x1010 || op == 0x2010 || op == 0x3010) && w[i + 1] == MethodDrvPicSetup) ||
                                              w[i] == (0x40100000u | MethodDrvPicSetup))) {
                                kind = Hit_Method;
                            }
                            if (kind == 0) { continue; }
                            ++nkind[kind];
                            const u64 hit_addr = mi.base_address + off + i * 4;
                            const bool list_it = kind != Hit_Method || nkind[Hit_Method] <= 8;
                            if (list_it && nlogged < MaxLogged) { hits[nlogged++] = { hit_addr, kind, w[i], w[i+1], w[i+2], w[i+3] }; }

                            if (kind <= 4 && nmagic_dumps < MaxMagicDumps) {
                                ++nmagic_dumps;
                                u32 want = 0x1000u;
                                if (hit_addr + want > region_end) { want = static_cast<u32>(region_end - hit_addr) & ~3u; }
                                rec_note(std::snprintf(note, sizeof(note), "setup magic @ %#llx in region %#llx+%#llx state %#x perm %#x",
                                                       static_cast<unsigned long long>(hit_addr),
                                                       static_cast<unsigned long long>(mi.base_address),
                                                       static_cast<unsigned long long>(mi.size), state, perm));
                                const u8 *body = dump(8, 0, hit_addr, want);
                                if (body != nullptr && want >= sizeof(nvenc_h264_drv_pic_setup_s) && plausible(body) && nsetups < MaxSetups) {
                                    setup_va[nsetups++] = hit_addr;
                                }
                            } else if (kind == Hit_SetclEnc && nsetcl_dumps < MaxSetclDumps) {
                                ++nsetcl_dumps;
                                u32 want = 0x400u;
                                if (hit_addr + want > region_end) { want = static_cast<u32>(region_end - hit_addr) & ~3u; }
                                rec_note(std::snprintf(note, sizeof(note), "NVENC SETCL @ %#llx", static_cast<unsigned long long>(hit_addr)));
                                dump(7, 0, hit_addr, want);
                            } else if (kind == Hit_SetclVic && nvic_dumps < MaxVicDumps) {
                                ++nvic_dumps;
                                u32 want = 0x400u;
                                if (hit_addr + want > region_end) { want = static_cast<u32>(region_end - hit_addr) & ~3u; }
                                rec_note(std::snprintf(note, sizeof(note), "VIC SETCL @ %#llx in region %#llx+%#llx state %#x",
                                                       static_cast<unsigned long long>(hit_addr),
                                                       static_cast<unsigned long long>(mi.base_address),
                                                       static_cast<unsigned long long>(mi.size), state));
                                dump(7, 0, hit_addr, want);
                            } else if (kind == Hit_Method && region_wins < 2 && nwins < MaxWins) {
                                bool covered = false;
                                for (u32 k = 0; k < nwins; ++k) {
                                    if (hit_addr >= wins[k].va && hit_addr < wins[k].va + wins[k].len) { covered = true; }
                                }
                                if (!covered) {
                                    const u64 ws = (hit_addr - mi.base_address > 0x200) ? hit_addr - 0x200 : mi.base_address;
                                    u32 wl = 0x800u;
                                    if (ws + wl > region_end) { wl = static_cast<u32>(region_end - ws) & ~3u; }
                                    wins[nwins] = { ws, wl, static_cast<u32>(hit_addr - ws) };
                                    rec_note(std::snprintf(note, sizeof(note), "SET_IN_DRV_PIC_SETUP write @ %#llx (+%#x in the dump) in region %#llx+%#llx state %#x",
                                                           static_cast<unsigned long long>(hit_addr), wins[nwins].hit_off,
                                                           static_cast<unsigned long long>(mi.base_address),
                                                           static_cast<unsigned long long>(mi.size), state));
                                    dump(7, wins[nwins].hit_off, ws, wl);
                                    ++nwins;
                                    ++region_wins;
                                }
                            }
                        }
                    }
                }

                const u64 next = mi.base_address + mi.size;
                if (next <= addr) { break; }
                addr = next;
            }

            u32 polls = 0, extra_setups = 0, extra_nonintra = 0, nev_watch = 0;
            bool intra_seen = false, cmd_redumped = false;
            u32 seen_n = 0;
            struct Seen { u32 slot; u32 frame_num; u32 pic_type; };
            Seen seen[48] = {};
            const u64 w0 = armTicksToNs(armGetSystemTick());
            if (nsetups > 0) {
                alignas(8) u8 hdr_buf[sizeof(nvenc_h264_drv_pic_setup_s)];
                while (armTicksToNs(armGetSystemTick()) - w0 < UINT64_C(3000000000)) {
                    ++polls;
                    {
                        ::ams::svc::DebugEventInfo ev;
                        u32 n = 0;
                        while (n < 64 && R_SUCCEEDED(::ams::svc::GetDebugEvent(std::addressof(ev), dbg))) { ++n; }
                        if (n != 0) {
                            nev_watch += n;
                            static_cast<void>(::ams::svc::ContinueDebugEvent(dbg,
                                ::ams::svc::ContinueFlag_ExceptionHandled | ::ams::svc::ContinueFlag_ContinueAll, nullptr, 0));
                        }
                    }
                    for (u32 k = 0; k < nsetups; ++k) {
                        if (!DbgRead(dbg, hdr_buf, setup_va[k], sizeof(hdr_buf))) { continue; }
                        nvenc_h264_drv_pic_setup_s st;
                        std::memcpy(std::addressof(st), hdr_buf, sizeof(st));
                        const u32 fn = st.pic_control.frame_num, pt = st.pic_control.pic_type;
                        bool known = false;
                        for (u32 j = 0; j < seen_n; ++j) {
                            if (seen[j].slot == k && seen[j].frame_num == fn && seen[j].pic_type == pt) { known = true; }
                        }
                        if (known) { continue; }
                        if (seen_n < 48) { seen[seen_n++] = { k, fn, pt }; }
                        const bool intra = pt >= 2;
                        if (extra_setups >= 8 || (!intra && extra_nonintra >= 3)) { continue; }
                        rec_note(std::snprintf(note, sizeof(note), "watch +%llu ms: slot %u @ %#llx frame_num %u pic_type %u%s",
                                               static_cast<unsigned long long>((armTicksToNs(armGetSystemTick()) - w0) / 1000000),
                                               k, static_cast<unsigned long long>(setup_va[k]), fn, pt, intra ? " (INTRA)" : ""));
                        if (dump(8, 0, setup_va[k], 0x1000) != nullptr) {
                            ++extra_setups;
                            if (!intra) { ++extra_nonintra; }
                        }
                        if (intra && !cmd_redumped) {
                            rec_note(std::snprintf(note, sizeof(note), "intra frame seen: command-buffer windows dumped again now"));
                            for (u32 j = 0; j < nwins; ++j) { dump(7, wins[j].hit_off, wins[j].va, wins[j].len); }
                            cmd_redumped = true;
                        }
                        intra_seen = intra_seen || intra;
                    }
                    if (intra_seen && extra_nonintra >= 2) { break; }
                    os::SleepThread(TimeSpan::FromMilliSeconds(5));
                }
            }
            const u64 watch_ms = (armTicksToNs(armGetSystemTick()) - w0) / 1000000;

            static_cast<void>(::ams::svc::CloseHandle(dbg));
            LogLine("   scanned %llu KB across %u regions; skipped %u region(s) over 8 MB (%llu KB)%s",
                    static_cast<unsigned long long>(scanned / 1024), nregions, nskipped,
                    static_cast<unsigned long long>(skipped_bytes / 1024),
                    scanned >= ScanCap ? "  (hit the 256 MB cap)" : "");
            LogLine("   hits: setup magic %u, NVENC SETCL %u, NVJPG SETCL %u, VIC SETCL %u, SET_IN_DRV_PIC_SETUP writes %u, NVENC status blocks %u",
                    nkind[1] + nkind[2] + nkind[3] + nkind[4], nkind[Hit_SetclEnc], nkind[Hit_SetclJpg],
                    nkind[Hit_SetclVic], nkind[Hit_Method], nkind[Hit_Status]);
            LogLine("   grc's own NVENC status blocks: error_status 0:%u 1:%u 2:%u 3:%u, ucode_error_status non-zero in %u",
                    status_err_hist[0], status_err_hist[1], status_err_hist[2], status_err_hist[3], status_ucode_nonzero);

            static const char *KindName[NumKinds] = { "?", "NVENC magic 5.0", "NVENC magic 6.0",
                                                      "NVENC magic 1.0", "MSENC magic 2.0",
                                                      "SETCL class 0x21 (NVENC)", "SETCL class 0xC0 (NVJPG)",
                                                      "SET_IN_DRV_PIC_SETUP write", "SETCL class 0x5D (VIC)",
                                                      "NVENC status block" };
            for (u32 i = 0; i < nlogged; ++i) {
                LogLine("   hit %2u @ %#llx  %-26s  %08x %08x %08x %08x", i,
                        static_cast<unsigned long long>(hits[i].addr),
                        KindName[hits[i].kind < NumKinds ? hits[i].kind : 0],
                        hits[i].word0, hits[i].word1, hits[i].word2, hits[i].word3);
            }
            LogLine("   plausible setups %u, command-buffer windows %u; watch: %u polls in %llu ms, %u new setups dumped, intra %s, %u debug events",
                    nsetups, nwins, polls, static_cast<unsigned long long>(watch_ms), extra_setups,
                    intra_seen ? "CAPTURED" : "not seen", nev_watch);
            if (nkind[1] + nkind[2] + nkind[3] + nkind[4] + nkind[Hit_Method] == 0) {
                LogLine("   nothing found. Either grc is idle and has not allocated its encoder");
                LogLine("   buffers, or they are in memory this process cannot read.");
            }
            if (!can_dump) {
                LogLine("   no cached stage buffer - hits logged above, nothing dumped");
            } else if (rec_len != 0) {
                LogLine("   dumped %u block(s) into %zu B of records -> sdmc:/grc-scan.bin (decode: tools/nvrec.py)", ndumped, rec_len);
                WriteSdVerified("sdmc:/grc-scan.bin", rec, rec_len, rec + RecCap, nullptr);
            }
            VicStage("grcscan:done");
        }

        void TryNvencPhaseA(u32 nvmap_fd, u32 cmd_handle) {
            if (g_engine_wedged) { LogLine("   nvenc-phaseA: skipped - an engine job is stuck this boot"); return; }
            if (g_clk_armed) { ClocksHoldForEngines("nvenc-phaseA"); }
            VicStage("nv:1_open_msenc");
            u32 efd = 0, nverr = 0;
            if (R_FAILED(NvOpen("/dev/nvhost-msenc", std::addressof(efd), std::addressof(nverr))) || nverr != 0) {
                LogLine("   NVENC: /dev/nvhost-msenc open FAILED nverr=%u", nverr);
                VicStage("nv:1_FAILED");
                return;
            }
            LogLine("   ---- NVENC phase A ----");
            LogLine("   /dev/nvhost-msenc open fd=%u", efd);

            u32 esyncpt = 0;
            {
                struct { u32 module_id; u32 syncpt; } gs = { 0, 0 };
                nverr = 0;
                const auto rc = NvIoctl(efd, NvHostIocChannelGetSyncpoint, std::addressof(gs), sizeof(gs), std::addressof(nverr));
                esyncpt = gs.syncpt;
                LogLine("   GET_SYNCPOINT rc=0x%x nverr=%u -> syncpt=%u  (VIC uses 12)", rc, nverr, esyncpt);
                if (R_FAILED(rc) || nverr != 0) { NvClose(efd); VicStage("nv:2_FAILED"); return; }
            }
            {
                struct { u32 fd; } sn = { nvmap_fd };
                nverr = 0;
                const auto rc = NvIoctl(efd, NvHostIocChannelSetNvmapFd, std::addressof(sn), sizeof(sn), std::addressof(nverr));
                LogLine("   SET_NVMAP_FD(%u) rc=0x%x nverr=%u", nvmap_fd, rc, nverr);
            }

            VicStage("nv:3_submit");
            auto *w = reinterpret_cast<u32 *>(g_vic_cmd_buf);
            u32 words = AppendIncrSyncpt(w, 0, esyncpt, false);
            armDCacheFlush(g_vic_cmd_buf, VicCmdSize);

            alignas(8) u8 sb[16 + 12 + 20 + 4] = {};
            u32 off = 0;
            auto put = [&](u32 v) { std::memcpy(sb + off, std::addressof(v), 4); off += 4; };
            put(1); put(0); put(1); put(1);
            put(cmd_handle); put(0); put(words);
            put(esyncpt); put(1); put(0); put(0); put(0);
            const u32 fence_off = off; put(0);
            const u32 sz = off;

            const u32 req = (UINT32_C(3) << 30) | (sz << 16) | (0x00u << 8) | 0x01u;
            u32 fence_val = 0;
            nverr = 0;
            const auto rc = NvIoctl(efd, req, sb, sz, std::addressof(nverr));
            std::memcpy(std::addressof(fence_val), sb + fence_off, 4);
            LogLine("   CHANNEL_SUBMIT req=0x%08x sz=%u words=%u rc=0x%x nverr=%u -> fence=%u",
                    req, sz, words, rc, nverr, fence_val);

            if (R_SUCCEEDED(rc) && nverr == 0) {
                u32 cfd = 0, ce = 0;
                if (R_SUCCEEDED(NvOpen("/dev/nvhost-ctrl", std::addressof(cfd), std::addressof(ce))) && ce == 0) {
                    struct { u32 id; u32 thresh; u32 timeout; } a = { esyncpt, fence_val, 100 };
                    u32 we = 0;
                    NvIoctl(cfd, NvHostIocCtrlSyncptWait, std::addressof(a), sizeof(a), std::addressof(we));
                    struct { u32 id; u32 value; } r = { esyncpt, 0 };
                    u32 re = 0;
                    NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(re));
                    const bool ok = r.value >= fence_val;
                    LogLine("   WAIT nverr=%u syncpt=%u (want >= %u)  %s", we, r.value, fence_val,
                            ok ? "*** NVENC CHANNEL USABLE ***" : "fence did not advance");
                    NvClose(cfd);
                    VicStage(ok ? "nv:PHASE_A_OK" : "nv:fence_stuck");
                }
            } else {
                LogLine("   submit rejected - the msenc channel does not take this ABI");
                VicStage("nv:submit_rejected");
            }
            NvClose(efd);
        }

        void VicWorkerThread(void *) {
            bool ui_started = false;
            for (;;) {
                os::SleepThread(TimeSpan::FromMilliSeconds(200));
                UsbFollowStreamSwitch();
                if (!ui_started && g_ui_stream_armed && g_live_armed && g_vic_pending.load(std::memory_order_acquire) < 0 &&
                    armTicksToNs(armGetSystemTick()) >= static_cast<u64>(g_probe_delay_s) * UINT64_C(1000000000)) {
                    ui_started = true;
                    LogLine("live: home_menu - starting without waiting for a game");
                    g_last_slot = 0;
                    RunVicBlit(0);
                    while (g_rearm.exchange(false)) { RunVicBlit(g_last_slot); }
                }
                const s32 slot = g_vic_pending.exchange(-1, std::memory_order_acq_rel);
                if (slot >= 0) {
                    g_last_slot = slot;
                    RunVicBlit(slot);
                }
                while (g_rearm.exchange(false)) { RunVicBlit(g_last_slot); }
            }
        }

    }

    void StartVicWorker() {
        if (!g_vic_armed) { return; }
        const s32 worker_prio = ThreadBasePriority() + 4;
        SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_vic_thread), VicWorkerThread, nullptr,
                                        g_vic_stack, sizeof(g_vic_stack), worker_prio));
        os::SetThreadNamePointer(std::addressof(g_vic_thread), "applet-mitm.VIC");
        os::StartThread(std::addressof(g_vic_thread));
        NoteThread(std::addressof(g_vic_thread));
        g_vic_stage.store("waiting", std::memory_order_relaxed);
        LogLine("VIC worker started (execute=%s)", g_vic_execute ? "PhaseB-full-blit" : "PhaseA-noop-cmdbuf");
    }

    void RequestVicBlit(s32 slot) {
        g_vic_pending.store(slot, std::memory_order_release);
    }

}
