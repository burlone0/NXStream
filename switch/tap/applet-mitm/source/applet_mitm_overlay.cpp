#include <stratosphere.hpp>
#include <cstdio>
#include <cstring>
#include "applet_mitm_log.hpp"
#include "applet_mitm_dmdbg.hpp"
#include "applet_mitm_uibinder.hpp"
#include "applet_mitm_ovlprobe.hpp"
#include "applet_mitm_overlay.hpp"
#include "vic40_config.hpp"

namespace ams::mitm::applet {

    constinit std::atomic<bool> g_ovl_vi_mitm{false};
    constinit std::atomic<u32> g_tesla_txns{0}, g_tesla_presents{0};

    namespace {

        constexpr const char *OverlaysFile = "sdmc:/config/nxstream/tap/overlays.txt";
        constexpr const char *ProbeFile    = "sdmc:/config/nxstream/tap/probe-ovl.txt";
        constexpr u64 OverlayDispProgramId = UINT64_C(0x010000000000100C);

        constinit bool g_armed = false;
        constinit std::atomic<bool> g_calibrated{false};

        constinit std::atomic<u64> g_tesla_present_tick{0};
        constinit std::atomic<u32> g_tesla_slot{0};
        constinit std::atomic<u32> g_tesla_buffers_gen{0};
        constinit u32 g_tesla_logged[16] = {};

        constinit os::SdkMutex g_area_lock;
        constinit u8 *g_area = nullptr;
        constinit u8 *g_cached = nullptr;
        constinit std::atomic<u32> g_area_dev{0};
        constinit std::atomic<bool> g_area_up{false};

        constexpr size_t BlockBytes = 0x2000;
        constexpr u32 BarW = 448, BarH = 128;
        constexpr size_t BarBytes = 28 * BlockBytes;
        constexpr u32 TeslaW = 448, TeslaH = 720;
        constexpr size_t TeslaSrcRow = 14 * BlockBytes;
        constexpr size_t TeslaSrcBuf = 6 * TeslaSrcRow;
        constexpr size_t TeslaRegion = 2 * TeslaSrcBuf;
        constexpr size_t TeslaDstRow = 28 * BlockBytes;
        constexpr u32 QsX = 816, QsW = 464, QsH = 720, QsCol0 = 51, QsCols = 29;
        constexpr size_t QsRow = QsCols * BlockBytes;
        constexpr size_t BigBuf = 6 * QsRow;
        constexpr size_t OffBar[2]   = { 0, BarBytes };
        constexpr size_t OffBig[2]   = { 2 * BarBytes, 2 * BarBytes + BigBuf };
        constexpr size_t AreaNeeded  = 2 * BarBytes + 2 * BigBuf;
        constexpr u32 FsW = 640, FsH = 360, FsCols = 40;
        constexpr size_t FsBuf = 3 * FsCols * BlockBytes;
        constexpr size_t OdRow = 80 * BlockBytes;
        static_assert(FsBuf <= BigBuf);
        static_assert(AreaNeeded <= 0x330000, "the overlays must fit the VIC output buffer past the self-tests' 64 KB");
        constexpr size_t CachedBar      = 0;
        constexpr size_t CachedTeslaIn  = BarBytes;
        constexpr size_t CachedTeslaOut = CachedTeslaIn + TeslaSrcBuf;
        constexpr size_t CachedLutRg    = CachedTeslaOut + TeslaDstRow;
        constexpr size_t CachedLutB     = CachedLutRg + 16 * 256 * 2;
        constexpr size_t CachedNeeded   = CachedLutB + 256;
        static_assert(CachedNeeded <= 0x140000 && QsRow <= TeslaSrcBuf && OdRow <= TeslaSrcBuf && BlockBytes <= TeslaDstRow);

        constexpr u64 OdBufBytes = UINT64_C(1280) * 768 * 4;
        constexpr u64 OdRegionBytes = 2 * OdBufBytes;

        constinit std::atomic<u32> g_bar_pub{0};
        constinit std::atomic<u64> g_bar_change_tick{0};
        constinit std::atomic<u32> g_tesla_pub{0};
        constinit std::atomic<u32> g_qs_pub{0};
        constinit std::atomic<bool> g_qs_active{false};
        constinit std::atomic<u32> g_fs_pub{0};
        constinit std::atomic<bool> g_fs_active{false};
        constexpr u64 BarHoldNs   = UINT64_C(2500000000);
        constexpr u64 TeslaHoldNs = UINT64_C(300000000);

        alignas(os::ThreadStackAlignment) constinit u8 g_ovl_stack[8_KB];
        constinit os::ThreadType g_ovl_thread;

        constinit u32 g_inv[256] = {};
        constinit u16 g_src_off[8][32] = {}, g_dst_off[8][16] = {};

        u32 GobOffset(u32 xb, u32 y) { return (xb / 32) * 256 + (y / 2) * 64 + ((xb % 32) / 16) * 32 + (y % 2) * 16 + (xb % 16); }
        u64 AgoNs(u64 tick) { return tick == 0 ? UINT64_MAX : armTicksToNs(armGetSystemTick() - tick); }
        u64 NowNs() { return armTicksToNs(armGetSystemTick()); }
        bool FileThere(const char *path) {
            fs::DirectoryEntryType t;
            return R_SUCCEEDED(fs::GetEntryType(std::addressof(t), path));
        }
        u64 PidOf(u64 program_id) {
            os::ProcessId pid{0};
            if (R_FAILED(::ams::pm::dmnt::GetProcessId(std::addressof(pid), ncm::ProgramId{program_id}))) { return 0; }
            return pid.value;
        }
        u32 Fnv(const u8 *p, size_t n) {
            u32 h = 2166136261u;
            for (size_t i = 0; i < n; i += 4) { h = (h ^ (p[i] | (p[i + 1] << 8) | (p[i + 2] << 16) | (static_cast<u32>(p[i + 3]) << 24))) * 16777619u; }
            return h;
        }

        struct Proc {
            svc::Handle h = svc::InvalidHandle;
            u64 base = 0;
            u64 retry_at = 0;
            const char *name = "";
        };
        constinit Proc g_od, g_tesla;

        void Detach(Proc &p, const char *why) {
            if (p.h != svc::InvalidHandle) {
                svc::CloseHandle(p.h);
                LogLine("overlays: %s detached (%s)", p.name, why);
            }
            p.h = svc::InvalidHandle;
            p.base = 0;
        }
        void Pump(Proc &p) {
            if (p.h == svc::InvalidHandle) { return; }
            s32 idx = 0;
            if (R_SUCCEEDED(svc::WaitSynchronization(std::addressof(idx), std::addressof(p.h), 1, 0))) {
                u32 n = 0;
                static_cast<void>(DbgResume(p.h, std::addressof(n)));
            }
        }
        u64 FindRegion(svc::Handle h, u64 size, const char *log = nullptr) {
            u64 addr = 0;
            for (u32 guard = 0; guard < 4096; ++guard) {
                svc::MemoryInfo mi = {};
                svc::PageInfo pi = {};
                if (!DbgQuery(h, std::addressof(mi), std::addressof(pi), addr) || mi.size == 0) { break; }
                const bool dev = (static_cast<u32>(mi.attribute) & svc::MemoryAttribute_DeviceShared) != 0 || mi.device_count != 0;
                if (dev && log != nullptr) {
                    LogLine("overlays: %s device-shared %010llx %llu KB state 0x%x perm %u dev %u", log, static_cast<unsigned long long>(mi.base_address),
                            static_cast<unsigned long long>(mi.size / 1024), static_cast<u32>(mi.state), static_cast<u32>(mi.permission), mi.device_count);
                }
                if (dev && mi.size == size && (mi.permission & svc::MemoryPermission_Read) != 0 && log == nullptr) { return mi.base_address; }
                const u64 next = mi.base_address + mi.size;
                if (next <= addr) { break; }
                addr = next;
            }
            return 0;
        }
        bool Attach(Proc &p, u64 program, u64 region, bool keep = false) {
            if (p.h != svc::InvalidHandle) { return true; }
            if (NowNs() < p.retry_at) { return false; }
            p.retry_at = NowNs() + UINT64_C(5000000000);
            const u64 pid = PidOf(program);
            if (pid == 0) { return false; }
            svc::Handle h = svc::InvalidHandle;
            if (const Result rc = svc::DebugActiveProcess(std::addressof(h), pid); R_FAILED(rc)) {
                LogLine("overlays: %s (pid %llu) - DebugActiveProcess rc=0x%x", p.name, static_cast<unsigned long long>(pid), rc.GetValue());
                return false;
            }
            u32 nev = 0;
            static_cast<void>(DbgResume(h, std::addressof(nev)));
            p.h = h;
            p.base = FindRegion(h, region);
            if (p.base == 0) {
                LogLine("overlays: %s (pid %llu) - no %llu KB region yet", p.name, static_cast<unsigned long long>(pid), static_cast<unsigned long long>(region / 1024));
                if (keep) { return true; }
                Detach(p, "no frame buffers");
                return false;
            }
            LogLine("overlays: %s attached (pid %llu), frame buffers at %010llx", p.name, static_cast<unsigned long long>(pid), static_cast<unsigned long long>(p.base));
            return true;
        }

        bool Unpremultiply(u8 *p, size_t n) {
            bool any = false;
            for (size_t i = 0; i < n; i += 4) {
                const u32 a = p[i + 3];
                if (a == 0) { p[i] = p[i + 1] = p[i + 2] = 0; continue; }
                any = true;
                if (a == 255) { continue; }
                for (u32 c = 0; c < 3; ++c) {
                    const u32 v = (p[i + c] * g_inv[a]) >> 16;
                    p[i + c] = static_cast<u8>(v > 255 ? 255 : v);
                }
            }
            return any;
        }

        void ConvertTeslaRow(const u8 *src, u8 *dst) {
            const u16 *lut_rg = reinterpret_cast<const u16 *>(g_cached + CachedLutRg);
            const u8 *lut_b = g_cached + CachedLutB;
            for (u32 gx = 0; gx < 14; ++gx) {
                for (u32 gy = 0; gy < 16; ++gy) {
                    const u8 *sg = src + gx * BlockBytes + gy * 512;
                    u8 *dg0 = dst + (2 * gx) * BlockBytes + gy * 512;
                    u8 *dg1 = dg0 + BlockBytes;
                    for (u32 y = 0; y < 8; ++y) {
                        for (u32 x = 0; x < 32; ++x) {
                            const u8 *s = sg + g_src_off[y][x];
                            const u32 a4 = s[1] >> 4;
                            const u32 px = a4 == 0 ? 0u : (lut_rg[a4 * 256 + s[0]] | (static_cast<u32>(lut_b[a4 * 16 + (s[1] & 15)]) << 16) | ((a4 * 17) << 24));
                            *reinterpret_cast<u32 *>((x < 16 ? dg0 : dg1) + g_dst_off[y][x & 15]) = px;
                        }
                    }
                }
            }
        }

        constinit u32 g_bar_copies = 0, g_tesla_frames = 0;
        constinit u64 g_work_ns = 0;

        constexpr u64 SettleMs = 5;

        constinit u32 g_bar_hash[2] = {}, g_bar_turn = 0;
        constinit bool g_bar_seen[2] = {};
        void StepBar() {
            if (g_od.h == svc::InvalidHandle) {
                if (!Attach(g_od, OverlayDispProgramId, OdRegionBytes)) { return; }
                g_bar_seen[0] = g_bar_seen[1] = false;
            }
            const u32 b = (g_bar_turn++) & 1;
            u8 *r0 = g_cached + CachedBar;
            const u64 va = g_od.base + b * OdBufBytes;
            if (!DbgRead(g_od.h, r0, va, BarBytes)) { Detach(g_od, "read failed"); return; }
            const u32 h = Fnv(r0, BarBytes);
            if (!g_bar_seen[b]) { g_bar_seen[b] = true; g_bar_hash[b] = h; return; }
            if (h == g_bar_hash[b]) { return; }
            const u32 pub = g_bar_pub.load(std::memory_order_relaxed);
            Pump(g_od);
            os::SleepThread(TimeSpan::FromMilliSeconds(SettleMs));
            if (!DbgRead(g_od.h, r0, va, BarBytes)) { Detach(g_od, "read failed"); return; }
            if (Fnv(r0, BarBytes) != h) {
                if (pub & 2) { g_bar_change_tick.store(armGetSystemTick(), std::memory_order_relaxed); }
                return;
            }
            g_bar_hash[b] = h;
            const bool any = Unpremultiply(r0, BarBytes);
            const u32 di = (pub & 2) ? ((pub & 1) ^ 1) : 0;
            std::memcpy(g_area + OffBar[di], r0, BarBytes);
            armDCacheFlush(g_area + OffBar[di], BarBytes);
            g_bar_change_tick.store(any ? armGetSystemTick() : 0, std::memory_order_relaxed);
            g_bar_pub.store(di | (any ? 2u : 0u), std::memory_order_release);
            ++g_bar_copies;
        }

        bool BlockShows(u64 buf, u32 brow, u32 col) {
            u8 *p = g_cached + CachedTeslaOut;
            if (!DbgRead(g_od.h, p, buf + (brow * 80 + col) * BlockBytes, BlockBytes)) { return false; }
            for (u32 i = 3; i < BlockBytes; i += 4) { if (p[i] != 0) { return true; } }
            return false;
        }

        void HalveRow(const u8 *in, u32 R, u8 *dst) {
            for (u32 oy = 0; oy < 64; ++oy) {
                const u32 Y = R * 64 + oy;
                if (Y >= FsH) { break; }
                const u32 iy = 2 * oy;
                const u8 *srow = in + (iy / 8) * 512;
                const u32 yy = iy % 8;
                u8 *drow = dst + (Y / 128) * FsCols * BlockBytes + ((Y % 128) / 8) * 512;
                const u32 dyy = Y % 8;
                for (u32 ox = 0; ox < FsW; ++ox) {
                    const u32 ix = 2 * ox;
                    const u8 *g = srow + (ix / 16) * BlockBytes;
                    const u8 *p00 = g + g_dst_off[yy][ix % 16], *p01 = g + g_dst_off[yy][(ix % 16) + 1];
                    const u8 *p10 = g + g_dst_off[yy + 1][ix % 16], *p11 = g + g_dst_off[yy + 1][(ix % 16) + 1];
                    const u32 a = (p00[3] + p01[3] + p10[3] + p11[3] + 2) / 4;
                    u32 px = 0;
                    if (a != 0) {
                        const u32 inv = g_inv[a];
                        u32 c[3];
                        for (u32 k = 0; k < 3; ++k) {
                            const u32 v = (((p00[k] + p01[k] + p10[k] + p11[k] + 2) / 4) * inv) >> 16;
                            c[k] = v > 255 ? 255 : v;
                        }
                        px = c[0] | (c[1] << 8) | (c[2] << 16) | (a << 24);
                    }
                    *reinterpret_cast<u32 *>(drow + (ox / 16) * BlockBytes + g_dst_off[dyy][ox % 16]) = px;
                }
            }
        }

        u64 Hash64(const u8 *p, size_t n) {
            const u64 *w = reinterpret_cast<const u64 *>(p);
            constexpr u64 K = UINT64_C(0x9E3779B97F4A7C15);
            u64 a = 1, b = 2, c = 3, d = 4;
            for (size_t i = 0; i + 4 <= n / 8; i += 4) {
                a = (a ^ w[i]) * K;
                b = (b ^ w[i + 1]) * K;
                c = (c ^ w[i + 2]) * K;
                d = (d ^ w[i + 3]) * K;
            }
            return a ^ ((b << 16) | (b >> 48)) ^ ((c << 32) | (c >> 32)) ^ ((d << 48) | (d >> 16));
        }

        struct Panel {
            const char *name;
            u32 col0, cols;
            bool half;
            std::atomic<u32> *pub;
            std::atomic<bool> *active;
            u32 turn;
            u64 held[2][6];
            bool held_valid[2];
            u32 copies, torn;
        };
        constinit Panel g_qs = { "the quick settings", QsCol0, QsCols, false, &g_qs_pub, &g_qs_active, 0, {}, {}, 0, 0 };
        constinit Panel g_fs = { "a full-screen one (the power menu?)", 0, 80, true, &g_fs_pub, &g_fs_active, 0, {}, {}, 0, 0 };

        void PanelReset(Panel &p) {
            p.pub->store(0, std::memory_order_relaxed);
            p.held_valid[0] = p.held_valid[1] = false;
        }

        enum class Look { Same, Copied, Moving, Failed };

        Look PanelTry(Panel &p, u32 b) {
            const u64 buf = g_od.base + b * OdBufBytes;
            u8 *in = g_cached + CachedTeslaIn;
            const size_t row_bytes = p.cols * BlockBytes;
            const u32 pub = p.pub->load(std::memory_order_relaxed);
            const bool shown = (pub & 2) != 0;
            const u32 si = pub & 1;
            u64 h1[6];
            bool same = shown;
            for (u32 r = 0; r < 6; ++r) {
                if (!DbgRead(g_od.h, in, buf + (r * 80 + p.col0) * BlockBytes, row_bytes)) { return Look::Failed; }
                h1[r] = Hash64(in, row_bytes);
                if (p.held[si][r] != h1[r]) { same = false; }
            }
            if (same) { return Look::Same; }
            Pump(g_od);
            os::SleepThread(TimeSpan::FromMilliSeconds(SettleMs));
            Pump(g_od);
            const u32 di = shown ? (si ^ 1) : 0;
            u8 *dst = g_area + OffBig[di];
            for (u32 r = 0; r < 6; ++r) {
                if (!DbgRead(g_od.h, in, buf + (r * 80 + p.col0) * BlockBytes, row_bytes)) { return Look::Failed; }
                const u64 h = Hash64(in, row_bytes);
                if (h != h1[r]) { ++p.torn; return Look::Moving; }
                if (p.held_valid[di] && p.held[di][r] == h) { continue; }
                if (p.half) { HalveRow(in, r, dst); }
                else {
                    static_cast<void>(Unpremultiply(in, row_bytes));
                    std::memcpy(dst + r * row_bytes, in, row_bytes);
                }
                p.held[di][r] = h;
            }
            p.held_valid[di] = true;
            armDCacheFlush(dst, BigBuf);
            p.pub->store(di | 2u, std::memory_order_release);
            ++p.copies;
            return Look::Copied;
        }

        void PanelStep(Panel &p) {
            const u32 b = (p.turn++) & 1;
            if (PanelTry(p, b) == Look::Moving) { static_cast<void>(PanelTry(p, b ^ 1)); }
        }

        void PanelWatch(Panel &p, bool shows) {
            if (!shows) {
                if (p.active->load(std::memory_order_relaxed)) {
                    p.active->store(false, std::memory_order_relaxed);
                    p.pub->store(0, std::memory_order_relaxed);
                    LogLine("overlays: %s closed", p.name);
                }
                return;
            }
            if (!p.active->load(std::memory_order_relaxed)) {
                PanelReset(p);
                p.active->store(true, std::memory_order_relaxed);
                g_tesla_pub.store(0, std::memory_order_relaxed);
                LogLine("overlays: %s opened", p.name);
            }
            PanelStep(p);
        }

        void StepPanels() {
            const u64 buf = g_od.base + (g_fs.turn & 1) * OdBufBytes;
            const bool fs = BlockShows(buf, 2, 10) && BlockShows(buf, 2, 40);
            PanelWatch(g_fs, fs);
            if (fs) {
                if (g_qs_active.load(std::memory_order_relaxed)) { PanelWatch(g_qs, false); }
                return;
            }
            const u64 qbuf = g_od.base + (g_qs.turn & 1) * OdBufBytes;
            PanelWatch(g_qs, BlockShows(qbuf, 2, 65));
        }

        constinit u32 g_tesla_gen_seen = 0, g_tesla_hash = 0;
        constinit u64 g_tesla_next_ns = 0, g_tesla_busy_until = 0;
        constinit u32 g_tesla_misses = 0;
        constinit u32 g_tesla_held[2][6] = {};
        constinit bool g_tesla_held_valid[2] = {};
        constinit u32 g_tesla_rows = 0;
        void StepTesla() {
            const u64 age = AgoNs(g_tesla_present_tick.load(std::memory_order_relaxed));
            if (age > UINT64_C(2000000000)) {
                if (g_tesla.h != svc::InvalidHandle) { Detach(g_tesla, "it stopped presenting"); }
                g_tesla_pub.store(0, std::memory_order_relaxed);
                return;
            }
            if (age > TeslaHoldNs) { return; }
            if (g_qs_active.load(std::memory_order_relaxed) || g_fs_active.load(std::memory_order_relaxed)) { g_tesla_held_valid[0] = g_tesla_held_valid[1] = false; return; }
            const u64 now = NowNs();
            if (now < g_tesla_next_ns) { return; }
            g_tesla_next_ns = now + (now < g_tesla_busy_until ? UINT64_C(50000000) : UINT64_C(100000000));
            const u32 gen = g_tesla_buffers_gen.load(std::memory_order_relaxed);
            if (g_tesla.h != svc::InvalidHandle && gen != g_tesla_gen_seen) { g_tesla.base = 0; g_tesla_held_valid[0] = g_tesla_held_valid[1] = false; }
            g_tesla_gen_seen = gen;
            if (g_tesla.h == svc::InvalidHandle && !Attach(g_tesla, TeslaProgramId, TeslaRegion, true)) { return; }
            if (g_tesla.base == 0) {
                g_tesla.base = FindRegion(g_tesla.h, TeslaRegion);
                if (g_tesla.base == 0) {
                    if (++g_tesla_misses == 10) { static_cast<void>(FindRegion(g_tesla.h, TeslaRegion, "Tesla (no 1344 KB region after 10 looks)")); }
                    return;
                }
                if (g_tesla_misses != 0) { LogLine("overlays: Tesla frame buffers at %010llx after %u look(s)", static_cast<unsigned long long>(g_tesla.base), g_tesla_misses); }
                g_tesla_misses = 0;
            }
            const u32 slot = g_tesla_slot.load(std::memory_order_relaxed) & 1;
            const u64 src = g_tesla.base + slot * TeslaSrcBuf;
            u8 *in = g_cached + CachedTeslaIn, *out = g_cached + CachedTeslaOut;
            if (!DbgRead(g_tesla.h, in, src, TeslaSrcBuf)) { g_tesla.base = 0; return; }
            Pump(g_tesla);
            u32 rh[6], h = 2166136261u;
            for (u32 r = 0; r < 6; ++r) { rh[r] = Fnv(in + r * TeslaSrcRow, TeslaSrcRow); h = (h ^ rh[r]) * 16777619u; }
            const u32 pub = g_tesla_pub.load(std::memory_order_relaxed);
            if ((pub & 2) && h == g_tesla_hash) { return; }
            g_tesla_busy_until = now + UINT64_C(1000000000);
            const u32 di = (pub & 2) ? ((pub & 1) ^ 1) : 0;
            u8 *dst = g_area + OffBig[di];
            for (u32 r = 0; r < 6; ++r) {
                if (g_tesla_held_valid[di] && g_tesla_held[di][r] == rh[r]) { continue; }
                ConvertTeslaRow(in + r * TeslaSrcRow, out);
                std::memcpy(dst + r * TeslaDstRow, out, TeslaDstRow);
                armDCacheFlush(dst + r * TeslaDstRow, TeslaDstRow);
                g_tesla_held[di][r] = rh[r];
                ++g_tesla_rows;
            }
            g_tesla_held_valid[di] = true;
            g_tesla_hash = h;
            g_tesla_pub.store(di | 2u, std::memory_order_release);
            ++g_tesla_frames;
        }

        void OverlayThread(void *) {
            for (u32 a = 1; a < 256; ++a) { g_inv[a] = (255u << 16) / a; }
            bool tables = false;
            for (u32 y = 0; y < 8; ++y) {
                for (u32 x = 0; x < 32; ++x) { g_src_off[y][x] = static_cast<u16>(GobOffset(x * 2, y)); }
                for (u32 x = 0; x < 16; ++x) { g_dst_off[y][x] = static_cast<u16>(GobOffset(x * 4, y)); }
            }
            g_od.name = "overlayDisp";
            g_tesla.name = "Tesla";
            u64 minute = NowNs() + UINT64_C(60000000000);
            for (u32 tick = 0;; ++tick) {
                os::SleepThread(TimeSpan::FromMilliSeconds(10));
                Pump(g_od);
                Pump(g_tesla);
                {
                    std::scoped_lock lk(g_area_lock);
                    if (!g_area_up.load(std::memory_order_relaxed) || !g_calibrated.load(std::memory_order_relaxed)) {
                        if (g_od.h != svc::InvalidHandle) { Detach(g_od, "not streaming"); }
                        if (g_tesla.h != svc::InvalidHandle) { Detach(g_tesla, "not streaming"); }
                        tables = false;
                        continue;
                    }
                    if (!tables) {
                        u16 *rg = reinterpret_cast<u16 *>(g_cached + CachedLutRg);
                        u8 *bl = g_cached + CachedLutB;
                        for (u32 a4 = 1; a4 < 16; ++a4) {
                            auto un = [a4](u32 c4) -> u32 { const u32 v = c4 * 255 / a4; return v > 255 ? 255 : v; };
                            for (u32 v = 0; v < 256; ++v) { rg[a4 * 256 + v] = static_cast<u16>(un(v & 15) | (un(v >> 4) << 8)); }
                            for (u32 v = 0; v < 16; ++v) { bl[a4 * 16 + v] = static_cast<u8>(un(v)); }
                        }
                        tables = true;
                    }
                    const u64 t0 = NowNs();
                    if ((tick % 10) == 0) {
                        StepBar();
                    }
                    if ((tick % 5) == 0 && g_od.h != svc::InvalidHandle) { StepPanels(); }
                    StepTesla();
                    g_work_ns += NowNs() - t0;
                }
                if (NowNs() >= minute) {
                    minute = NowNs() + UINT64_C(60000000000);
                    if (g_bar_copies != 0 || g_tesla_frames != 0 || g_qs.copies != 0 || g_fs.copies != 0) {
                        LogLine("overlays: last minute - volume bar copied %u times, quick settings %u (%u looks caught it being drawn), full-screen %u (%u), Tesla %u frames (%u of their 128-row strips converted), %llu ms of work",
                                g_bar_copies, g_qs.copies, g_qs.torn, g_fs.copies, g_fs.torn, g_tesla_frames, g_tesla_rows, static_cast<unsigned long long>(g_work_ns / 1000000));
                    }
                    g_bar_copies = g_tesla_frames = g_tesla_rows = 0;
                    g_qs.copies = g_qs.torn = g_fs.copies = g_fs.torn = 0;
                    g_work_ns = 0;
                }
            }
        }

    }

    void OverlayArm(bool on) {
        g_armed = on || FileThere(OverlaysFile);
        const bool probe = SFT_OVL_PROBE && FileThere(ProbeFile);
        if (g_armed || probe) { g_ovl_vi_mitm.store(true, std::memory_order_relaxed); }
        if (g_armed) {
            LogLine("overlays: on (%s) - the volume bar, quick settings, power menu and Tesla/Ultrahand in the stream; Tesla's new vi:m sessions are wrapped (never captured)", on ? "config" : "overlays.txt");
            SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_ovl_thread), OverlayThread, nullptr, g_ovl_stack, sizeof(g_ovl_stack), ThreadBasePriority()));
            os::SetThreadNamePointer(std::addressof(g_ovl_thread), "applet-mitm.Overlays");
            os::StartThread(std::addressof(g_ovl_thread));
            NoteThread(std::addressof(g_ovl_thread));
        } else if (probe) {
            LogLine("ovl2: Tesla's new vi:m sessions will be wrapped (counted, never captured)");
        }
    }

    bool OverlaysArmed() { return g_armed; }

    void OverlaySetCalibrated(bool ok) {
        g_calibrated.store(ok, std::memory_order_relaxed);
        if (!ok) { LogLine("overlays: OFF for this boot - the blend check failed"); }
    }

    void OverlayAreaUp(u8 *cpu, u32 dev, size_t size, u8 *cached, size_t cached_size) {
        if (!g_armed || cpu == nullptr || dev == 0 || size < AreaNeeded || cached == nullptr || cached_size < CachedNeeded) { return; }
        std::scoped_lock lk(g_area_lock);
        g_area = cpu;
        g_cached = cached;
        g_area_dev.store(dev, std::memory_order_relaxed);
        g_bar_pub.store(0, std::memory_order_relaxed);
        g_tesla_pub.store(0, std::memory_order_relaxed);
        g_bar_change_tick.store(0, std::memory_order_relaxed);
        g_tesla_held_valid[0] = g_tesla_held_valid[1] = false;
        g_qs_pub.store(0, std::memory_order_relaxed);
        g_qs_active.store(false, std::memory_order_relaxed);
        g_fs_pub.store(0, std::memory_order_relaxed);
        g_fs_active.store(false, std::memory_order_relaxed);
        g_area_up.store(true, std::memory_order_release);
    }

    void OverlayAreaDown() {
        std::scoped_lock lk(g_area_lock);
        g_area_up.store(false, std::memory_order_relaxed);
        g_area = nullptr;
        g_cached = nullptr;
        g_area_dev.store(0, std::memory_order_relaxed);
        g_bar_pub.store(0, std::memory_order_relaxed);
        g_tesla_pub.store(0, std::memory_order_relaxed);
        g_qs_pub.store(0, std::memory_order_relaxed);
        g_qs_active.store(false, std::memory_order_relaxed);
        g_fs_pub.store(0, std::memory_order_relaxed);
        g_fs_active.store(false, std::memory_order_relaxed);
    }

    u32 OverlayPlanes(OverlayPlane *out, u32 max) {
        if (!g_calibrated.load(std::memory_order_relaxed) || !g_area_up.load(std::memory_order_acquire)) { return 0; }
        const u32 dev = g_area_dev.load(std::memory_order_relaxed);
        if (dev == 0) { return 0; }
        u32 n = 0;
        const u32 fp = g_fs_pub.load(std::memory_order_acquire);
        const u32 qp = g_qs_pub.load(std::memory_order_acquire);
        const u32 tp = g_tesla_pub.load(std::memory_order_acquire);
        if (n < max && g_fs_active.load(std::memory_order_relaxed) && (fp & 2)) {
            out[n++] = { dev + static_cast<u32>(OffBig[fp & 1]), FsW, FsH, vic::PIXFMT_A8R8G8B8, 0, 0, 1280, 720 };
            static bool said = false;
            if (!said) { said = true; LogLine("overlays: a full-screen one (the power menu) is in the stream (first frame)"); }
        } else if (n < max && g_qs_active.load(std::memory_order_relaxed) && (qp & 2)) {
            out[n++] = { dev + static_cast<u32>(OffBig[qp & 1]), QsW, QsH, vic::PIXFMT_A8R8G8B8, QsX, 0, QsW, QsH };
            static bool said = false;
            if (!said) { said = true; LogLine("overlays: the quick settings are in the stream (first frame)"); }
        } else if (n < max && (tp & 2) && AgoNs(g_tesla_present_tick.load(std::memory_order_relaxed)) < TeslaHoldNs) {
            out[n++] = { dev + static_cast<u32>(OffBig[tp & 1]), TeslaW, TeslaH, vic::PIXFMT_A8R8G8B8, 0, 0, TeslaW, TeslaH };
            static bool said = false;
            if (!said) { said = true; LogLine("overlays: Tesla is in the stream (first frame)"); }
        }
        const u32 bp = g_bar_pub.load(std::memory_order_acquire);
        if (n < max && (bp & 2) && AgoNs(g_bar_change_tick.load(std::memory_order_relaxed)) < BarHoldNs) {
            out[n++] = { dev + static_cast<u32>(OffBar[bp & 1]), BarW, BarH, vic::PIXFMT_A8R8G8B8, 0, 0, BarW, BarH };
            static bool said = false;
            if (!said) { said = true; LogLine("overlays: the volume bar is in the stream (first frame)"); }
        }
        return n;
    }

    void OvlNoteTeslaTransaction(u32 code, const void *in, size_t in_size) {
        g_tesla_txns.fetch_add(1, std::memory_order_relaxed);
        if (code == 7) {
            const s32 slot = ParseParcelSlot(static_cast<const u8 *>(in), in_size);
            if (slot >= 0) { g_tesla_slot.store(static_cast<u32>(slot), std::memory_order_relaxed); }
            g_tesla_present_tick.store(armGetSystemTick(), std::memory_order_relaxed);
            g_tesla_presents.fetch_add(1, std::memory_order_relaxed);
        } else if (code == 14) {
            g_tesla_buffers_gen.fetch_add(1, std::memory_order_relaxed);
        }
        if (code < 16 && g_tesla_logged[code] < 3) {
            ++g_tesla_logged[code];
            LogLine("overlays: Tesla binder code %u, %zu B in", code, in_size);
        }
    }

}
