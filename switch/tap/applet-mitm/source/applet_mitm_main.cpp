#include <stratosphere.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "applet_mitm_service.hpp"
#include "applet_mitm_sm.hpp"
#include "applet_mitm_donor.hpp"
#include "applet_mitm_am.hpp"
#include "applet_mitm_audout.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_nv.hpp"
#include "applet_mitm_nvdrv.hpp"
#include "applet_mitm_nvjpg.hpp"
#include "applet_mitm_armfile.hpp"
#include "applet_mitm_gop.hpp"
#include "applet_mitm_clk.hpp"
#include "applet_mitm_control.hpp"
#include "applet_mitm_shot.hpp"
#include "applet_mitm_audio.hpp"
#include "applet_mitm_uvc.hpp"
#include "applet_mitm_net.hpp"
#include "applet_mitm_uibinder.hpp"
#include "applet_mitm_uisource.hpp"
#include "applet_mitm_soundprobe.hpp"
#include "applet_mitm_ovlprobe.hpp"
#include "applet_mitm_overlay.hpp"
#include "applet_mitm_featmem.hpp"
#include "applet_mitm_vpad.hpp"

extern "C" {
    NvServiceType __nx_nv_service_type    = NvServiceType_System;
    u32           __nx_nv_transfermem_size = 0x40000;
}

namespace ams {

    namespace {

        constexpr size_t MallocBufferSize = 96_KB;
        alignas(os::MemoryPageSize) constinit u8 g_malloc_buffer[MallocBufferSize];

        enum PortIndex {
            PortIndex_AppletMitm,
            PortIndex_ViManagerMitm,
            PortIndex_Count,
        };

        constexpr sm::ServiceName AppletMitmServiceName = sm::ServiceName::Encode("vi:u");
        constexpr sm::ServiceName ViManagerMitmServiceName = sm::ServiceName::Encode("vi:m");

        struct ServerOptions {
            static constexpr size_t PointerBufferSize   = 0x1000;
            static constexpr size_t MaxDomains          = 0;
            static constexpr size_t MaxDomainObjects    = 0;
            static constexpr bool CanDeferInvokeRequest = false;
            static constexpr bool CanManageMitmServers  = true;
        };

        constexpr size_t MaxSessions = 16;

        class ServerManager final : public sf::hipc::ServerManager<PortIndex_Count, ServerOptions, MaxSessions> {
            private:
                virtual Result OnNeedsToAccept(int port_index, Server *server) override;
        };

        ServerManager g_server_manager;

        constexpr size_t ViThreads = 4;
        alignas(os::ThreadStackAlignment) constinit u8 g_vi_stacks[ViThreads - 1][16_KB];
        constinit os::ThreadType g_vi_threads[ViThreads - 1];
        void ViLoopThread(void *) { mitm::applet::LoopProcessSafely(g_server_manager, "vi"); }

        enum UiPortIndex {
            UiPortIndex_Dispdrv,
            UiPortIndex_ViSystem,
            UiPortIndex_Count,
        };
        constexpr sm::ServiceName DispdrvServiceName = sm::ServiceName::Encode("dispdrv");
        constexpr sm::ServiceName ViSystemServiceName = sm::ServiceName::Encode("vi:s");
        struct UiServerOptions {
            static constexpr size_t PointerBufferSize   = 0x3000;
            static constexpr size_t MaxDomains          = 4;
            static constexpr size_t MaxDomainObjects    = 16;
            static constexpr bool CanDeferInvokeRequest = false;
            static constexpr bool CanManageMitmServers  = true;
        };
#if SFT_UI_PROBE
        constexpr size_t UiMaxSessions = 12;
        constexpr size_t UiThreads = 3;
#else
        constexpr size_t UiMaxSessions = 4;
        constexpr size_t UiThreads = 2;
#endif
        class UiServerManager final : public sf::hipc::ServerManager<UiPortIndex_Count, UiServerOptions, UiMaxSessions> {
            private:
                virtual Result OnNeedsToAccept(int port_index, Server *server) override;
        };
        constinit UiServerManager *g_ui_server_manager = nullptr;
        UiServerManager *UiServer() {
            static_assert(alignof(UiServerManager) <= os::MemoryPageSize);
            if (g_ui_server_manager == nullptr) {
                if (void *mem = mitm::applet::FeatureMemory(sizeof(UiServerManager), "home menu: the dispdrv server"); mem != nullptr) {
                    g_ui_server_manager = std::construct_at(static_cast<UiServerManager *>(mem));
                }
            }
            return g_ui_server_manager;
        }
        alignas(os::ThreadStackAlignment) constinit u8 g_ui_stacks[UiThreads][12_KB];
        constinit os::ThreadType g_ui_threads[UiThreads];
        void UiLoopThread(void *) { mitm::applet::LoopProcessSafely(*g_ui_server_manager, "dispdrv"); }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
        void StartUiBinderMitm() {
            if (!mitm::applet::UiProbeOn() && !mitm::applet::g_ui_stream_armed) { return; }
            ::Service dd = {};
            ::Result rc = 0;
            {
                mitm::applet::SmUse sm_dd;
                rc = ::smGetService(std::addressof(dd), "dispdrv");
            }
            if (R_FAILED(rc)) { mitm::applet::LogLine("home menu: dispdrv not opened rc=0x%x - the Album is not streamed", rc); return; }
            mitm::applet::LogLine("home menu: dispdrv's pointer buffer %#x (ours %#zx)", static_cast<unsigned>(dd.pointer_buffer_size), UiServerOptions::PointerBufferSize);
            mitm::applet::UiProbeSetDispdrv(dd);
            bool any = false;
            const bool probe_dd = mitm::applet::UiProbeWants("dispdrv");
            if (!probe_dd && !mitm::applet::g_ui_stream_armed) {
                mitm::applet::LogLine("uiprobe: dispdrv mitm off (nodispdrv)");
            } else if (dd.pointer_buffer_size > UiServerOptions::PointerBufferSize) {
                mitm::applet::LogLine("home menu: dispdrv's pointer buffer is larger than ours - the Album is not streamed");
            } else if (UiServer() == nullptr) {
                mitm::applet::LogLine("home menu: the Album is not streamed (no memory for its server)");
            } else {
                mitm::applet::g_ui_dispdrv_probe.store(probe_dd);
                SFT_ABORT_UNLESS(UiServer()->RegisterMitmServer<mitm::applet::UiBinderMitm>(UiPortIndex_Dispdrv, DispdrvServiceName));
                mitm::applet::LogLine("dispdrv wrapped for %s", probe_dd ? "the probe (HOME-menu processes) and the Album" : "the Album (home_menu)");
                any = true;
            }
            if (mitm::applet::UiProbeWants("vi")) {
                ::Service vs = {};
                ::Result vrc = 0;
                {
                    mitm::applet::SmUse sm_vs;
                    vrc = ::smGetService(std::addressof(vs), "vi:s");
                }
                if (R_FAILED(vrc)) {
                    mitm::applet::LogLine("uiprobe: vi:s not opened rc=0x%x - applets not wrapped", vrc);
                } else {
                    const u32 pbs = static_cast<u32>(vs.pointer_buffer_size);
                    ::serviceClose(std::addressof(vs));
                    mitm::applet::LogLine("uiprobe: vi:s pointer buffer %#x (ours %#zx, vi server %#zx)", pbs, UiServerOptions::PointerBufferSize, ServerOptions::PointerBufferSize);
                    if (pbs <= UiServerOptions::PointerBufferSize && pbs <= ServerOptions::PointerBufferSize && UiServer() != nullptr) {
                        mitm::applet::g_ui_vi_mitm.store(true);
                        SFT_ABORT_UNLESS(UiServer()->RegisterMitmServer<mitm::applet::ViSystemRootMitm>(UiPortIndex_ViSystem, ViSystemServiceName));
                        mitm::applet::LogLine("uiprobe: library applets' vi:s and vi:m wrapped (shared-layer presents logged)");
                        any = true;
                    } else {
                        mitm::applet::LogLine("uiprobe: vi:s pointer buffer too large - applets not wrapped");
                    }
                }
            }
            if (!any) { return; }
            const s32 prio = mitm::applet::ServerThreadPriority;
            for (size_t i = 0; i < UiThreads; ++i) {
                SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_ui_threads[i]), UiLoopThread, nullptr, g_ui_stacks[i], sizeof(g_ui_stacks[i]), prio));
                os::SetThreadNamePointer(std::addressof(g_ui_threads[i]), "applet-mitm.Ui");
                os::StartThread(std::addressof(g_ui_threads[i]));
                mitm::applet::NoteThread(std::addressof(g_ui_threads[i]));
            }
            mitm::applet::LogLine("home menu: dispdrv server up, %zu threads", UiThreads);
        }
#pragma GCC diagnostic pop

        enum NvPortIndex {
            NvPortIndex_Nvdrv,
            NvPortIndex_Count,
        };

        constexpr sm::ServiceName NvdrvMitmServiceName = sm::ServiceName::Encode("nvdrv:s");
        constexpr size_t NvMaxSessions = 16;
        constexpr size_t NvThreads     = 4;

#if SFT_GRC_RESEARCH
        struct NvServerOptions {
            static constexpr size_t PointerBufferSize   = 0x10000;
            static constexpr size_t MaxDomains          = 0;
            static constexpr size_t MaxDomainObjects    = 0;
            static constexpr bool CanDeferInvokeRequest = false;
            static constexpr bool CanManageMitmServers  = true;
        };

        class NvServerManager final : public sf::hipc::ServerManager<NvPortIndex_Count, NvServerOptions, NvMaxSessions> {
            private:
                virtual Result OnNeedsToAccept(int port_index, Server *server) override;
        };

        NvServerManager g_nv_server_manager;

        alignas(os::ThreadStackAlignment) constinit u8 g_nv_stacks[NvThreads][32_KB];
        constinit os::ThreadType g_nv_threads[NvThreads];

        void NvLoopThread(void *) { g_nv_server_manager.LoopProcess(); }

#endif

#if !SFT_GRC_RESEARCH
        void StartGrcRecorder() {
            constexpr const char *Lst = "sdmc:/atmosphere/contents/4200000000004E54/mitm.lst";
            fs::DirectoryEntryType t;
            if (mitm::applet::g_grc_armed) { mitm::applet::LogLine("grc IPC interceptor: armed, but not in this build (SFT_GRC_RESEARCH)"); }
            if (R_FAILED(fs::GetEntryType(std::addressof(t), Lst))) { mitm::applet::LogLine("grc IPC interceptor: off"); return; }
            const Result crc = sm::mitm::ClearFutureMitm(NvdrvMitmServiceName);
            const Result drc = fs::DeleteFile(Lst);
            mitm::applet::LogLine("grc IPC interceptor: off - stale mitm.lst found: future nvdrv:s mitm cleared rc=0x%x, file deleted rc=0x%x", crc.GetValue(), drc.GetValue());
        }
#else
        void StartGrcRecorder() {
            bool have_lst = false;
            {
                fs::FileHandle lf;
                if (R_SUCCEEDED(fs::OpenFile(std::addressof(lf), "sdmc:/atmosphere/contents/4200000000004E54/mitm.lst", fs::OpenMode_Read))) {
                    fs::CloseFile(lf);
                    have_lst = true;
                }
            }
            if (!mitm::applet::g_grc_armed) {
                if (!have_lst) {
                    mitm::applet::LogLine("grc IPC interceptor: off");
                    return;
                }
                mitm::applet::LogLine("grc IPC interceptor: off, but mitm.lst is PRESENT - registering as a pass-through "
                                      "(accepts nobody) so nvdrv:s clients are not blocked. Delete mitm.lst.");
            } else if (!have_lst) {
                mitm::applet::LogLine("grc IPC interceptor: ARMED but mitm.lst is MISSING - refusing to register");
                return;
            } else {
                mitm::applet::LogLine("grc IPC interceptor: ARMED (this handler has hung the console before)");
            }
            SFT_ABORT_UNLESS(g_nv_server_manager.RegisterMitmServer<mitm::applet::NvDrvMitm>(NvPortIndex_Nvdrv, NvdrvMitmServiceName));
            const s32 prio = mitm::applet::ThreadBasePriority();
            for (size_t i = 0; i < NvThreads; ++i) {
                SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_nv_threads[i]), NvLoopThread, nullptr,
                                                g_nv_stacks[i], sizeof(g_nv_stacks[i]), prio));
                os::SetThreadNamePointer(std::addressof(g_nv_threads[i]), "applet-mitm.NvDrv");
                os::StartThread(std::addressof(g_nv_threads[i]));
                mitm::applet::NoteThread(std::addressof(g_nv_threads[i]));
            }
        }

#endif

        alignas(os::ThreadStackAlignment) constinit u8 g_hb_stack[8_KB];
        constinit os::ThreadType g_hb_thread;
        constinit std::atomic<bool> g_screen_off{false};

        void LogAppletLimits(const char *tag = nullptr) {
            if (mitm::applet::g_pmdmnt_rc != 0) { return; }
            static s64 last[5] = { -1, -1, -1, -1, -1 };
            static const svc::LimitableResource res[5] = {
                svc::LimitableResource_PhysicalMemoryMax, svc::LimitableResource_ThreadCountMax, svc::LimitableResource_EventCountMax,
                svc::LimitableResource_TransferMemoryCountMax, svc::LimitableResource_SessionCountMax,
            };
            u64 cur[5] = {}, lim[5] = {};
            bool moved = false;
            for (u32 k = 0; k < 5; ++k) {
                if (R_FAILED(pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(cur[k]), std::addressof(lim[k]), pm::ResourceLimitGroup_Applet, res[k]))) { return; }
                const s64 v = k == 0 ? static_cast<s64>(cur[k] >> 20) : static_cast<s64>(cur[k]);
                if (v != last[k]) { moved = true; last[k] = v; }
            }
            u64 scur = 0, slim = 0;
            static s64 last_sys = -1;
            if (R_SUCCEEDED(pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(scur), std::addressof(slim), pm::ResourceLimitGroup_System, svc::LimitableResource_PhysicalMemoryMax))) {
                const s64 v = static_cast<s64>(scur >> 18);
                if (v != last_sys) { moved = true; last_sys = v; }
            }
            u64 sc[3] = {}, sl[3] = {};
            static s64 last_cnt[3] = { -1, -1, -1 };
            static const svc::LimitableResource cres[3] = { svc::LimitableResource_SessionCountMax, svc::LimitableResource_ThreadCountMax, svc::LimitableResource_EventCountMax };
            for (u32 k = 0; k < 3; ++k) {
                if (R_SUCCEEDED(pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(sc[k]), std::addressof(sl[k]), pm::ResourceLimitGroup_System, cres[k]))) {
                    if (static_cast<s64>(sc[k]) != last_cnt[k]) { moved = true; last_cnt[k] = static_cast<s64>(sc[k]); }
                }
            }
            {
                static size_t low = ~size_t(0);
                const size_t fr = init::GetAllocator()->GetTotalFreeSize();
                if (fr + 16_KB <= low || (tag != nullptr && fr < low)) {
                    low = fr;
                    mitm::applet::LogLine("malloc: at most %zu KB of %zu KB in use so far", (MallocBufferSize - low) / 1024, MallocBufferSize / 1024);
                }
            }
            if (!moved && tag == nullptr) { return; }
            if (tag != nullptr) { mitm::applet::LogLine("applet group @ %s:", tag); }
            mitm::applet::LogLine("system group: memory %llu/%llu KB used (%lld KB free), sessions %llu/%llu, threads %llu/%llu, events %llu/%llu; nxtap:s sessions made %u",
                                  static_cast<unsigned long long>(scur >> 10), static_cast<unsigned long long>(slim >> 10),
                                  static_cast<long long>((static_cast<s64>(slim) - static_cast<s64>(scur)) / 1024),
                                  static_cast<unsigned long long>(sc[0]), static_cast<unsigned long long>(sl[0]),
                                  static_cast<unsigned long long>(sc[1]), static_cast<unsigned long long>(sl[1]),
                                  static_cast<unsigned long long>(sc[2]), static_cast<unsigned long long>(sl[2]),
                                  mitm::applet::SessionsMadeViaSm());
            mitm::applet::LogLine("applet group: memory %llu/%llu MB, threads %llu/%llu, events %llu/%llu, transfer memories %llu/%llu, sessions %llu/%llu",
                                  static_cast<unsigned long long>(cur[0] >> 20), static_cast<unsigned long long>(lim[0] >> 20),
                                  static_cast<unsigned long long>(cur[1]), static_cast<unsigned long long>(lim[1]),
                                  static_cast<unsigned long long>(cur[2]), static_cast<unsigned long long>(lim[2]),
                                  static_cast<unsigned long long>(cur[3]), static_cast<unsigned long long>(lim[3]),
                                  static_cast<unsigned long long>(cur[4]), static_cast<unsigned long long>(lim[4]));
        }

        void ScreenFollowStream() {
            static bool dark = false, tried = false, lbl_ok = false;
            const u64 qt = mitm::applet::g_queue_tick.load(std::memory_order_relaxed);
            const bool drawing = qt != 0 && armTicksToNs(armGetSystemTick() - qt) < UINT64_C(2500000000);
            const bool want = g_screen_off.load(std::memory_order_relaxed) && drawing &&
                              mitm::applet::g_streaming_now.load(std::memory_order_relaxed);
            if (want == dark) { return; }
            if (!tried) {
                tried = true;
                mitm::applet::SmUse sm;
                const Result r = lblInitialize();
                lbl_ok = R_SUCCEEDED(r);
                mitm::applet::LogLine("screen: lbl %s (rc=0x%x)", lbl_ok ? "ready" : "unavailable", r.GetValue());
            }
            if (!lbl_ok) { return; }
            const Result r = want ? lblSwitchBacklightOff(0) : lblSwitchBacklightOn(0);
            dark = want;
            mitm::applet::LogLine("screen: %s (rc=0x%x)", want ? "off while streaming" : "on", r.GetValue());
        }

        constinit size_t g_ballast_mb = 0;
        constinit u32 g_ballast_delay_s = 0;
        void BallastRead() {
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), "sdmc:/config/nxstream/tap/ballast.txt", fs::OpenMode_Read))) { return; }
            char b[16] = {};
            size_t n = 0;
            static_cast<void>(fs::ReadFile(std::addressof(n), f, 0, b, sizeof(b) - 1));
            fs::CloseFile(f);
            char *e = nullptr;
            g_ballast_mb = static_cast<size_t>(std::strtoul(b, std::addressof(e), 10)) & ~size_t(1);
            g_ballast_delay_s = static_cast<u32>(std::strtoul(e, nullptr, 10));
            mitm::applet::LogLine("TEST ballast: %zu MB, taken %u s after boot", g_ballast_mb, g_ballast_delay_s);
        }
        void BallastTake() {
            if (g_ballast_mb == 0) { return; }
            uintptr_t addr = 0;
            const Result r1 = os::SetMemoryHeapSize(g_ballast_mb << 20);
            const Result r2 = R_SUCCEEDED(r1) ? os::AllocateMemoryBlock(std::addressof(addr), g_ballast_mb << 20) : r1;
            if (addr != 0) { std::memset(reinterpret_cast<void *>(addr), 0, g_ballast_mb << 20); }
            mitm::applet::LogLine("TEST ballast: %zu MB held now (heap rc=0x%x, block rc=0x%x)", g_ballast_mb, r1.GetValue(), r2.GetValue());
            g_ballast_mb = 0;
            LogAppletLimits("ballast");
        }

        bool StartedInTime() {
            constexpr const char *OwnFlag = "sdmc:/atmosphere/contents/4200000000004E54/flags/boot2.flag";
            constexpr const char *LauncherFlag = "sdmc:/atmosphere/contents/4200000000004E55/flags/boot2.flag";
            constexpr const char *Go = "sdmc:/config/nxstream/tap/launch.txt";
            fs::DirectoryEntryType t;
            if (R_SUCCEEDED(fs::GetEntryType(std::addressof(t), OwnFlag))) {
                static_cast<void>(fs::CreateDirectory("sdmc:/atmosphere/contents/4200000000004E55/flags"));
                static_cast<void>(fs::CreateFile(LauncherFlag, 0));
                static_cast<void>(fs::DeleteFile(OwnFlag));
                mitm::applet::LogLine("boot2.flag moved to the launcher (4200000000004E55): it starts NXStream Native after the system has booted");
            }
            static_cast<void>(fs::DeleteFile("sdmc:/atmosphere/contents/4200000000004E54/toolbox.json"));
            u64 at = 0;
            {
                fs::FileHandle f;
                if (R_SUCCEEDED(fs::OpenFile(std::addressof(f), Go, fs::OpenMode_Read))) {
                    char b[24] = {};
                    size_t n = 0;
                    static_cast<void>(fs::ReadFile(std::addressof(n), f, 0, b, sizeof(b) - 1));
                    fs::CloseFile(f);
                    static_cast<void>(fs::DeleteFile(Go));
                    at = std::strtoull(b, nullptr, 10);
                }
            }
            const u64 now = armTicksToNs(armGetSystemTick()) / 1000000;
            const bool by_launcher = at != 0 && now >= at && now - at < 15000;
            if (!by_launcher && now < 60000) {
                mitm::applet::LogLine("started at %llu.%03llu s, before the system has booted (an older install's boot2.flag) - exiting; the launcher starts it once the HOME menu is up",
                                      static_cast<unsigned long long>(now / 1000), static_cast<unsigned long long>(now % 1000));
                return false;
            }
            fs::FileHandle f;
            if (R_SUCCEEDED(fs::OpenFile(std::addressof(f), "sdmc:/config/nxstream/tap/launcher.txt", fs::OpenMode_Read))) {
                char b[768] = {};
                size_t n = 0;
                static_cast<void>(fs::ReadFile(std::addressof(n), f, 0, b, sizeof(b) - 1));
                fs::CloseFile(f);
                for (char *line = b; line != nullptr && *line != 0; ) {
                    char *nl = std::strchr(line, '\n');
                    if (nl != nullptr) { *nl = 0; }
                    if (*line != 0) { mitm::applet::LogLine("launcher: %s", line); }
                    line = nl != nullptr ? nl + 1 : nullptr;
                }
            }
            if (!by_launcher) { mitm::applet::LogLine("started by hand, %llu s after boot", static_cast<unsigned long long>(now / 1000)); }
            return true;
        }

        void HeartbeatThread(void *) {
            for (u32 i = 1; ; i++) {
                for (u32 t = 0; t < 12; ++t) {
                    os::SleepThread(TimeSpan::FromMilliSeconds(250));
                    if (g_ballast_mb != 0 && armTicksToNs(armGetSystemTick()) >= static_cast<u64>(g_ballast_delay_s) * UINT64_C(1000000000)) { BallastTake(); }
                    if (t == 0 && (i == 40 || i == 200 || (i > 200 && i % 1200 == 0))) { mitm::applet::LogStackUse(); }
                    LogAppletLimits();
                    mitm::applet::UiProbeStep();
                    ScreenFollowStream();
                }
                if (mitm::applet::LogQuiet()) { mitm::applet::LogFlushStale(); continue; }
                mitm::applet::LogFlushHeld();
                char b[96];
                const auto &st = mitm::applet::g_stats;
                std::snprintf(b, sizeof(b), "hb:%u sess=%u getdisp=%u relay=%u txn=%u vic=%s",
                              i, st.sessions.load(), st.getdisp.load(),
                              st.relay.load(), st.txns.load(),
                              mitm::applet::g_vic_stage.load(std::memory_order_relaxed));
                mitm::applet::LogMark(b);
                mitm::applet::FlushNvdrvRecords();
            }
        }

        void SyncSoundPatches() {
            constexpr const char *Bundled = "sdmc:/config/nxstream/tap/dvr-patches";
            constexpr const char *Ours    = "sdmc:/atmosphere/exefs_patches/nxstream-tap-sound";
            fs::DirectoryEntryType t;
            if (R_FAILED(fs::GetEntryType(std::addressof(t), Ours)) || t != fs::DirectoryEntryType_Directory) { return; }
            fs::DirectoryHandle d;
            if (R_FAILED(fs::OpenDirectory(std::addressof(d), Bundled, fs::OpenDirectoryMode_File))) { return; }
            struct Scratch { fs::DirectoryEntry entries[48]; u8 a[4096], b[4096]; };
            std::unique_ptr<Scratch> scratch(new (std::nothrow) Scratch);
            if (!scratch) { fs::CloseDirectory(d); return; }
            fs::DirectoryEntry *entries = scratch->entries;
            s64 n = 0;
            const Result rr = fs::ReadDirectory(std::addressof(n), entries, d, 48);
            fs::CloseDirectory(d);
            if (R_FAILED(rr)) { return; }
            u32 updated = 0, same = 0;
            u8 *a = scratch->a, *b = scratch->b;
            auto read_all = [](const char *path, u8 *buf, size_t cap, size_t *len) -> bool {
                fs::FileHandle f;
                if (R_FAILED(fs::OpenFile(std::addressof(f), path, fs::OpenMode_Read))) { return false; }
                s64 sz = 0;
                bool ok = R_SUCCEEDED(fs::GetFileSize(std::addressof(sz), f)) && sz > 0 && static_cast<size_t>(sz) <= cap &&
                          R_SUCCEEDED(fs::ReadFile(f, 0, buf, static_cast<size_t>(sz)));
                fs::CloseFile(f);
                *len = ok ? static_cast<size_t>(sz) : 0;
                return ok;
            };
            for (s64 i = 0; i < n; ++i) {
                const char *name = entries[i].name;
                const size_t nl = std::strlen(name);
                if (nl < 5 || (std::strcmp(name + nl - 4, ".ips") != 0 && std::strcmp(name + nl - 4, ".IPS") != 0)) { continue; }
                char from[256], to[256];
                std::snprintf(from, sizeof(from), "%s/%s", Bundled, name);
                std::snprintf(to, sizeof(to), "%s/%s", Ours, name);
                size_t la = 0, lb = 0;
                if (!read_all(from, a, sizeof(scratch->a), std::addressof(la))) { continue; }
                if (read_all(to, b, sizeof(scratch->b), std::addressof(lb)) && la == lb && std::memcmp(a, b, la) == 0) { ++same; continue; }
                fs::DeleteFile(to);
                fs::FileHandle f;
                if (R_SUCCEEDED(fs::CreateFile(to, static_cast<s64>(la))) && R_SUCCEEDED(fs::OpenFile(std::addressof(f), to, fs::OpenMode_Write))) {
                    if (R_SUCCEEDED(fs::WriteFile(f, 0, a, la, fs::WriteOption::Flush))) { ++updated; }
                    fs::CloseFile(f);
                }
            }
            mitm::applet::LogLine("sound in no-record games: on (dvr-patches, %u current, %u refreshed from the bundle)", same, updated);
        }

        size_t ReadFileText(const char *path, char *buf, size_t cap) {
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), path, fs::OpenMode_Read))) {
                return 0;
            }
            s64 fsz = 0;
            if (R_FAILED(fs::GetFileSize(std::addressof(fsz), f))) { fs::CloseFile(f); return 0; }
            size_t n = static_cast<size_t>(fsz);
            if (n > cap - 1) { n = cap - 1; }
            const bool ok = n > 0 && R_SUCCEEDED(fs::ReadFile(f, 0, buf, n));
            fs::CloseFile(f);
            if (!ok) { return 0; }
            buf[n] = '\0';
            return n;
        }

        constinit char g_arm_buf[512] = {};
        constinit size_t g_arm_len = 0;
        constinit bool g_arm_loaded = false;

        void LoadArm() {
            if (g_arm_loaded) { return; }
            g_arm_loaded = true;
            if (!mitm::applet::g_release_mode) {
                g_arm_len = ReadFileText("sdmc:/applet-mitm.armed", g_arm_buf, sizeof(g_arm_buf));
                return;
            }
            static char ini[4096];
            const size_t n = ReadFileText("sdmc:/config/nxstream/tap/config.ini", ini, sizeof(ini));
            g_arm_len = mitm::applet::armfile::BuildReleaseArm(ini, n, g_arm_buf, sizeof(g_arm_buf));
        }

        bool ArmFileContains(const char *keyword) {
            LoadArm();
            return mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, keyword);
        }

        void ApplySessionSettings() {
            mitm::applet::g_nvstream_qp   = mitm::applet::armfile::Number(g_arm_buf, g_arm_len, "nvqp", 20);
            mitm::applet::g_nvstream_gop  = mitm::applet::armfile::Number(g_arm_buf, g_arm_len, "nvgop", 0);
            mitm::applet::g_cap720_armed  = mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "cap720");
            mitm::applet::g_anyfw_armed   = mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "anyfw");
            mitm::applet::g_shot_enabled.store(mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "shot"));
            mitm::applet::g_shot_combo.store(std::min(mitm::applet::armfile::Number(g_arm_buf, g_arm_len, "shotkey", 0), 4u));
            mitm::applet::g_shot_mask.store(mitm::applet::armfile::Number(g_arm_buf, g_arm_len, "shotmask", 0) & sft::shotbuttons::AllButtons);
            mitm::applet::g_shot_capture.store(mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "shotcap"));
            mitm::applet::g_shot_album.store(mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "shotalbum"));
            mitm::applet::g_audio_enabled.store(mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "audio"));
            mitm::applet::g_nvab_armed = mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "nvab");
            mitm::applet::g_detach_on_home = !mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "nodethome");
            g_screen_off.store(mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "screenoff"), std::memory_order_relaxed);
            mitm::applet::g_hb_docked.store(mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "hbdocked"), std::memory_order_relaxed);
            mitm::applet::g_force_docked.store(mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "forcedock"), std::memory_order_relaxed);
            mitm::applet::g_vpad_enabled.store(mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "vinput"), std::memory_order_relaxed);
            mitm::applet::g_early_collect = mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "earlyoff") ? 0
                                          : mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "earlyab") ? 2 : 1;
            mitm::applet::g_mid_collect = mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "midoff") ? 0
                                        : mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "midon") ? 1
                                        : mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "midab") ? 2 : mitm::applet::MidCollectDefault;
            mitm::applet::g_scale1080.store(mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "scale1080"), std::memory_order_relaxed);
            mitm::applet::g_memory_boost.store(mitm::applet::armfile::Contains(g_arm_buf, g_arm_len, "memboost"), std::memory_order_relaxed);
            mitm::applet::g_max_fps.store(mitm::applet::armfile::Number(g_arm_buf, g_arm_len, "maxfps", 0), std::memory_order_relaxed);
            mitm::applet::g_usb_gop.store(mitm::applet::armfile::Number(g_arm_buf, g_arm_len, "usbgop", mitm::applet::keyframe::UsbGopDefault),
                                          std::memory_order_relaxed);
        }

        u32 ArmFileNumber(const char *key, u32 def) {
            LoadArm();
            return mitm::applet::armfile::Number(g_arm_buf, g_arm_len, key, def);
        }

        void LogMemoryPools() {
            static const char *const names[4] = { "Application", "Applet", "System", "SystemUnsafe" };
            mitm::applet::LogLine("---- physical memory pools (svcGetSystemInfo) ----");
            for (u64 pool = 0; pool < 4; ++pool) {
                u64 tot = 0, used = 0;
                const auto r1 = svc::GetSystemInfo(std::addressof(tot),  svc::SystemInfoType_TotalPhysicalMemorySize, svc::InvalidHandle, pool);
                const auto r2 = svc::GetSystemInfo(std::addressof(used), svc::SystemInfoType_UsedPhysicalMemorySize,  svc::InvalidHandle, pool);
                mitm::applet::LogLine("   pool %llu %-12s total=%8llu KB  used=%8llu KB  free=%9lld KB   rc=%x/%x",
                                      static_cast<unsigned long long>(pool), names[pool],
                                      static_cast<unsigned long long>(tot / 1024),
                                      static_cast<unsigned long long>(used / 1024),
                                      static_cast<long long>((static_cast<s64>(tot) - static_cast<s64>(used)) / 1024),
                                      r1.GetValue(), r2.GetValue());
            }
            u64 ptot = 0, pused = 0;
            svc::GetInfo(std::addressof(ptot),  svc::InfoType_TotalMemorySize, svc::PseudoHandle::CurrentProcess, 0);
            svc::GetInfo(std::addressof(pused), svc::InfoType_UsedMemorySize,  svc::PseudoHandle::CurrentProcess, 0);
            mitm::applet::LogLine("   THIS PROCESS (pool_partition 1 = Applet, at boot): total=%llu KB used=%llu KB free=%lld KB",
                                  static_cast<unsigned long long>(ptot / 1024),
                                  static_cast<unsigned long long>(pused / 1024),
                                  static_cast<long long>((static_cast<s64>(ptot) - static_cast<s64>(pused)) / 1024));
            mitm::applet::LogLine("   for scale: one 1080p frame = 7913 KB, one block-row strip = 960 KB");
        }

        alignas(0x1000) constinit u8 g_usb_ep_in_buf[0x1000]  = {};
        alignas(0x1000) constinit u8 g_usb_ep_out_buf[0x1000] = {};
        constinit bool g_usb_armed = false;

        constinit UsbDsInterface *g_usb_iface  = nullptr;
        constinit UsbDsEndpoint  *g_usb_ep_in  = nullptr;
        constinit UsbDsEndpoint  *g_usb_ep_out = nullptr;

        constexpr size_t UsbChunk     = 0x40000;
        constexpr u64    UsbTimeoutNs = UINT64_C(5000000000);

        void TryUsbEnumerate() {
            if (!g_usb_armed) {
                mitm::applet::LogLine("   usb: not armed (add \"usb\" to the arm file)");
                return;
            }
            if (mitm::applet::g_uvc_mode) {
                static_cast<void>(mitm::applet::UvcSetupDevice());
                return;
            }
            mitm::applet::LogLine("---- USB device enumeration (usb:ds) ----");

            ::Result rc = usbDsInitialize();
            mitm::applet::LogLine("   usbDsInitialize rc=0x%x %s", rc,
                                  R_SUCCEEDED(rc) ? "" : "<- could not acquire usb:ds");
            if (R_FAILED(rc)) { return; }

            u8 iMan = 0, iProd = 0, iSer = 0;
            static const u16 langs[1] = { 0x0409 };
            rc = usbDsAddUsbLanguageStringDescriptor(nullptr, langs, 1);
            if (R_SUCCEEDED(rc)) { rc = usbDsAddUsbStringDescriptor(std::addressof(iMan),  "NXStream"); }
            if (R_SUCCEEDED(rc)) { rc = usbDsAddUsbStringDescriptor(std::addressof(iProd), "NXStream Native"); }
            if (R_SUCCEEDED(rc)) { rc = usbDsAddUsbStringDescriptor(std::addressof(iSer),  "NXStream"); }
            mitm::applet::LogLine("   string descriptors rc=0x%x", rc);

            struct usb_device_descriptor dd = {
                .bLength            = USB_DT_DEVICE_SIZE,
                .bDescriptorType    = USB_DT_DEVICE,
                .bcdUSB             = 0x0110,
                .bDeviceClass       = 0x00,
                .bDeviceSubClass    = 0x00,
                .bDeviceProtocol    = 0x00,
                .bMaxPacketSize0    = 0x40,
                .idVendor           = 0x18D1,
                .idProduct          = 0x4D00,
                .bcdDevice          = 0x0100,
                .iManufacturer      = iMan,
                .iProduct           = iProd,
                .iSerialNumber      = iSer,
                .bNumConfigurations = 0x01,
            };
            rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, std::addressof(dd));
            dd.bcdUSB = 0x0200;
            if (R_SUCCEEDED(rc)) { rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, std::addressof(dd)); }
            dd.bcdUSB          = 0x0300;
            dd.bMaxPacketSize0 = 9;
            if (R_SUCCEEDED(rc)) { rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, std::addressof(dd)); }
            mitm::applet::LogLine("   device descriptors (Full+High+Super) rc=0x%x", rc);
            if (R_FAILED(rc)) { return; }

            {
                const u8 bos[0x16] = {
                    0x05, USB_DT_BOS, 0x16, 0x00, 0x02,

                    0x07, USB_DT_DEVICE_CAPABILITY, 0x02,
                    0x02, 0x00, 0x00, 0x00,

                    0x0a, USB_DT_DEVICE_CAPABILITY, 0x03,
                    0x00,
                    0x0c, 0x00,
                    0x03,
                    0x00,
                    0x00, 0x00,
                };
                rc = usbDsSetBinaryObjectStore(bos, sizeof(bos));
                mitm::applet::LogLine("   usbDsSetBinaryObjectStore rc=0x%x%s", rc,
                                      R_SUCCEEDED(rc) ? "" : "  <- SuperSpeed will not be offered");
                if (R_FAILED(rc)) { return; }
            }

            UsbDsInterface *&iface  = g_usb_iface;
            UsbDsEndpoint  *&ep_in  = g_usb_ep_in;
            UsbDsEndpoint  *&ep_out = g_usb_ep_out;
            iface = nullptr; ep_in = nullptr; ep_out = nullptr;

            struct usb_interface_descriptor id = {
                .bLength            = USB_DT_INTERFACE_SIZE,
                .bDescriptorType    = USB_DT_INTERFACE,
                .bInterfaceNumber   = 4,
                .bAlternateSetting  = 0,
                .bNumEndpoints      = 2,
                .bInterfaceClass    = USB_CLASS_VENDOR_SPEC,
                .bInterfaceSubClass = USB_CLASS_VENDOR_SPEC,
                .bInterfaceProtocol = USB_CLASS_VENDOR_SPEC,
                .iInterface         = 0,
            };
            struct usb_endpoint_descriptor ep_i = {
                .bLength          = USB_DT_ENDPOINT_SIZE,
                .bDescriptorType  = USB_DT_ENDPOINT,
                .bEndpointAddress = USB_ENDPOINT_IN,
                .bmAttributes     = USB_TRANSFER_TYPE_BULK,
                .wMaxPacketSize   = 0x200,
                .bInterval        = 0,
            };
            struct usb_endpoint_descriptor ep_o = {
                .bLength          = USB_DT_ENDPOINT_SIZE,
                .bDescriptorType  = USB_DT_ENDPOINT,
                .bEndpointAddress = USB_ENDPOINT_OUT,
                .bmAttributes     = USB_TRANSFER_TYPE_BULK,
                .wMaxPacketSize   = 0x40,
                .bInterval        = 0,
            };

            std::memset(g_usb_ep_in_buf,  0, sizeof(g_usb_ep_in_buf));
            std::memset(g_usb_ep_out_buf, 0, sizeof(g_usb_ep_out_buf));

            rc = usbDsRegisterInterface(std::addressof(iface));
            mitm::applet::LogLine("   usbDsRegisterInterface rc=0x%x", rc);
            if (R_FAILED(rc)) { return; }

            id.bInterfaceNumber   = iface->interface_index;
            ep_i.bEndpointAddress = static_cast<u8>(ep_i.bEndpointAddress + id.bInterfaceNumber + 1);
            ep_o.bEndpointAddress = static_cast<u8>(ep_o.bEndpointAddress + id.bInterfaceNumber + 1);

            rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Full, std::addressof(id),   USB_DT_INTERFACE_SIZE);
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Full, std::addressof(ep_i), USB_DT_ENDPOINT_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Full, std::addressof(ep_o), USB_DT_ENDPOINT_SIZE); }
            ep_i.wMaxPacketSize = 0x200;
            ep_o.wMaxPacketSize = 0x200;
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_High, std::addressof(id),   USB_DT_INTERFACE_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_High, std::addressof(ep_i), USB_DT_ENDPOINT_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_High, std::addressof(ep_o), USB_DT_ENDPOINT_SIZE); }
            struct usb_ss_endpoint_companion_descriptor ss_comp = {
                .bLength           = sizeof(struct usb_ss_endpoint_companion_descriptor),
                .bDescriptorType   = USB_DT_SS_ENDPOINT_COMPANION,
                .bMaxBurst         = 0x0f,
                .bmAttributes      = 0x00,
                .wBytesPerInterval = 0x00,
            };
            ep_i.wMaxPacketSize = 0x400;
            ep_o.wMaxPacketSize = 0x400;
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Super, std::addressof(id),      USB_DT_INTERFACE_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Super, std::addressof(ep_i),    USB_DT_ENDPOINT_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Super, std::addressof(ss_comp), USB_DT_SS_ENDPOINT_COMPANION_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Super, std::addressof(ep_o),    USB_DT_ENDPOINT_SIZE); }
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_AppendConfigurationData(iface, UsbDeviceSpeed_Super, std::addressof(ss_comp), USB_DT_SS_ENDPOINT_COMPANION_SIZE); }
            mitm::applet::LogLine("   configuration descriptors (Full+High+Super) rc=0x%x", rc);
            if (R_FAILED(rc)) { return; }

            rc = usbDsInterface_RegisterEndpoint(iface, std::addressof(ep_in), ep_i.bEndpointAddress);
            if (R_SUCCEEDED(rc)) { rc = usbDsInterface_RegisterEndpoint(iface, std::addressof(ep_out), ep_o.bEndpointAddress); }
            mitm::applet::LogLine("   endpoints IN=0x%02x OUT=0x%02x rc=0x%x", ep_i.bEndpointAddress, ep_o.bEndpointAddress, rc);
            if (R_FAILED(rc)) { return; }

            rc = usbDsInterface_EnableInterface(iface);
            mitm::applet::LogLine("   EnableInterface rc=0x%x", rc);
            if (R_FAILED(rc)) { return; }

            rc = usbDsEnable();
            mitm::applet::LogLine("   usbDsEnable rc=0x%x", rc);
            if (R_FAILED(rc)) {
                mitm::applet::LogLine("   usbDsEnable failed - device will not appear on the bus");
                return;
            }

            static const char *const state_names[] = {
                "Detached", "Attached", "Powered", "Default", "Address", "Configured", "Suspended",
            };
            UsbState st = UsbState_Detached, best = UsbState_Detached;
            for (int i = 0; i < 100; ++i) {
                if (R_FAILED(usbDsGetState(std::addressof(st)))) { break; }
                if (static_cast<int>(st) > static_cast<int>(best)) { best = st; }
                if (st == UsbState_Configured) { break; }
                os::SleepThread(TimeSpan::FromMilliSeconds(100));
            }
            const unsigned b = static_cast<unsigned>(best);
            mitm::applet::LogLine("   usb state high-water = %u (%s)", b,
                                  b < (sizeof(state_names) / sizeof(state_names[0])) ? state_names[b] : "?");
            {
                UsbDeviceSpeed neg = UsbDeviceSpeed_None;
                const ::Result srrc = usbDsGetSpeed(std::addressof(neg));
                static const char *const speed_names[] = {
                    "None", "Low(1.5Mbps)", "Full(12Mbps)", "High(480Mbps)", "SUPER(5Gbps)",
                };
                const unsigned sn = static_cast<unsigned>(neg);
                mitm::applet::LogLine("   *** NEGOTIATED SPEED = %u (%s) *** rc=0x%x", sn,
                                      sn < (sizeof(speed_names) / sizeof(speed_names[0])) ? speed_names[sn] : "?",
                                      srrc);
                mitm::applet::LogLine("   %s", neg == UsbDeviceSpeed_Super ? "SuperSpeed: NV12 1080p60 (178 MiB/s) is within reach"
                                             : neg == UsbDeviceSpeed_High  ? "High Speed (USB 2.0) - all the stream needs"
                                             : neg == UsbDeviceSpeed_Full  ? "Full Speed (USB 1.1, 12 Mbps) - too slow for the stream (a hub or adapter that is USB 1.1)"
                                             : "no link yet");
            }
            mitm::applet::LogLine("   %s", best == UsbState_Configured
                ? "*** HOST CONFIGURED US - we own the bus, expect 18d1:4d00 in lsusb ***"
                : "host did NOT configure us - no cable, or the PC is not the host: a USB-C to USB-C cable can make the Switch the host; use USB-C to USB-A (#22)");
        }

        Result ServerManager::OnNeedsToAccept(int port_index, Server *server) {
            std::shared_ptr<::Service> fsrv;
            sm::MitmProcessInfo client_info;
            server->AcknowledgeMitmSession(std::addressof(fsrv), std::addressof(client_info));
            mitm::applet::g_stats.sessions.fetch_add(1);

            mitm::applet::LogLine("OnNeedsToAccept port=%d program=%016llx", port_index,
                                  static_cast<unsigned long long>(client_info.program_id.value));

            switch (port_index) {
                case PortIndex_AppletMitm:
                    {
                        const Result r = this->AcceptMitmImpl(server,
                            sf::CreateSharedObjectEmplaced<mitm::applet::IViRootMitm, mitm::applet::ViRootMitm>(decltype(fsrv)(fsrv), client_info),
                            fsrv);
                        mitm::applet::LogLine("  AcceptMitmImpl rc=0x%x", r.GetValue());
                        R_RETURN(r);
                    }
                case PortIndex_ViManagerMitm:
                    {
                        const Result r = this->AcceptMitmImpl(server,
                            sf::CreateSharedObjectEmplaced<mitm::applet::IViManagerRootMitm, mitm::applet::ViManagerRootMitm>(decltype(fsrv)(fsrv), client_info),
                            fsrv);
                        mitm::applet::LogLine("  AcceptMitmImpl (vi:m) rc=0x%x", r.GetValue());
                        R_RETURN(r);
                    }
                AMS_UNREACHABLE_DEFAULT_CASE();
            }
        }

        Result UiServerManager::OnNeedsToAccept(int port_index, Server *server) {
            std::shared_ptr<::Service> fsrv;
            sm::MitmProcessInfo client_info;
            server->AcknowledgeMitmSession(std::addressof(fsrv), std::addressof(client_info));
            mitm::applet::LogLine("%s accept program=%016llx forward pointer_buffer_size=%#x", port_index == UiPortIndex_Dispdrv ? "dispdrv" : "vi:s",
                                  static_cast<unsigned long long>(client_info.program_id.value), static_cast<unsigned>(fsrv->pointer_buffer_size));
            switch (port_index) {
                case UiPortIndex_Dispdrv:
                    R_RETURN(this->AcceptMitmImpl(server,
                        sf::CreateSharedObjectEmplaced<mitm::applet::IUiBinderMitm, mitm::applet::UiBinderMitm>(decltype(fsrv)(fsrv), client_info),
                        fsrv));
                case UiPortIndex_ViSystem:
                    R_RETURN(this->AcceptMitmImpl(server,
                        sf::CreateSharedObjectEmplaced<mitm::applet::IViSystemRootMitm, mitm::applet::ViSystemRootMitm>(decltype(fsrv)(fsrv), client_info),
                        fsrv));
                AMS_UNREACHABLE_DEFAULT_CASE();
            }
        }

#if SFT_GRC_RESEARCH
        Result NvServerManager::OnNeedsToAccept(int port_index, Server *server) {
            std::shared_ptr<::Service> fsrv;
            sm::MitmProcessInfo client_info;
            server->AcknowledgeMitmSession(std::addressof(fsrv), std::addressof(client_info));
            AMS_ABORT_UNLESS(port_index == NvPortIndex_Nvdrv);
            mitm::applet::LogLine("nvdrv:s accept program=%016llx forward pointer_buffer_size=%#x (ours %#zx)",
                                  static_cast<unsigned long long>(client_info.program_id.value),
                                  static_cast<unsigned>(fsrv->pointer_buffer_size), NvServerOptions::PointerBufferSize);
            R_RETURN(this->AcceptMitmImpl(server,
                sf::CreateSharedObjectEmplaced<mitm::applet::INvDrvMitm, mitm::applet::NvDrvMitm>(decltype(fsrv)(fsrv), client_info),
                fsrv));
        }
#endif

    }

    namespace init {

        void InitializeSystemModule() {
            R_ABORT_UNLESS(sm::Initialize());
            fs::InitializeForSystem();
            fs::SetEnabledAutoAbort(false);

            mitm::applet::g_pmdmnt_rc = ::pmdmntInitialize();

        }

        void FinalizeSystemModule() {  }

        void Startup() {
            init::InitializeAllocator(g_malloc_buffer, sizeof(g_malloc_buffer));
        }

    }

    void Main() {
        mitm::applet::SetThreadBasePriority(os::GetThreadPriority(os::GetCurrentThread()));
        os::SetThreadNamePointer(os::GetCurrentThread(), "applet-mitm.Main");

        mitm::applet::LogInit();
        if (!StartedInTime()) { return; }
        LogAppletLimits("main start");

        mitm::applet::g_grc_armed = ArmFileContains("grc");
        StartGrcRecorder();

        mitm::applet::LogLine("applet-mitm v0.7.22: up (grc IPC interceptor %s), built against Atmosphere %d.%d.%d",
                              mitm::applet::g_grc_armed ? "ARMED" : "off",
                              ATMOSPHERE_RELEASE_VERSION_MAJOR, ATMOSPHERE_RELEASE_VERSION_MINOR, ATMOSPHERE_RELEASE_VERSION_MICRO);
        {
            const auto api = exosphere::GetApiInfo();
            const u32 hv = hosversionGet();
            mitm::applet::LogLine("Atmosphere %u.%u.%u, firmware %u.%u.%u (built with Atmosphere %u.%u.%u, tested on firmware 22.5.0; 23.0.0 by a user)",
                                  api.GetMajorVersion(), api.GetMinorVersion(), api.GetMicroVersion(),
                                  HOSVER_MAJOR(hv), HOSVER_MINOR(hv), HOSVER_MICRO(hv),
                                  ATMOSPHERE_RELEASE_VERSION_MAJOR, ATMOSPHERE_RELEASE_VERSION_MINOR, ATMOSPHERE_RELEASE_VERSION_MICRO);
        }
        SyncSoundPatches();

        mitm::applet::g_vic_armed   = ArmFileContains("vic");
        mitm::applet::g_vic_execute = ArmFileContains("exec");
        mitm::applet::g_dbg_armed   = ArmFileContains("dbg");
        mitm::applet::g_dump_armed  = ArmFileContains("dump");
        g_usb_armed                 = ArmFileContains("usb");
        mitm::applet::g_uvc_mode    = ArmFileContains("uvc");
        mitm::applet::g_uvc_raw     = mitm::applet::g_uvc_mode && ArmFileContains("uvcraw");
        mitm::applet::g_net_armed   = ArmFileContains("net");
        mitm::applet::g_bench_armed   = ArmFileContains("bench");
        mitm::applet::g_nvenc_armed   = ArmFileContains("nvenc");
        mitm::applet::g_nvjpg_armed   = ArmFileContains("jpg");
        mitm::applet::g_nvjpg_quality = ArmFileNumber("q", 85);
        mitm::applet::g_nvjpg_attempts = ArmFileNumber("jat", 90);
        mitm::applet::g_sweep_armed   = ArmFileContains("sweep");
        mitm::applet::g_matrix_armed  = ArmFileContains("mtx");
        mitm::applet::g_grcscan_armed = ArmFileContains("grcscan");
        mitm::applet::g_clk_armed     = ArmFileContains("clk");
        mitm::applet::g_jpgdec_armed  = ArmFileContains("jpgdec");
        mitm::applet::g_nvgrc_armed   = ArmFileContains("nvgrc");
        mitm::applet::g_csc_armed     = ArmFileContains("csc");
        mitm::applet::g_nvframe_armed = ArmFileContains("nvframe");
        mitm::applet::g_nvframe_n     = ArmFileNumber("nvframe", 120);
        mitm::applet::g_nvstream_armed = ArmFileContains("nvstream");
        mitm::applet::g_nvstream_n    = ArmFileNumber("nvstream", 3600);
        mitm::applet::g_nvstream_qp   = ArmFileNumber("nvqp", 20);
        mitm::applet::g_nvstream_gop  = ArmFileNumber("nvgop", 0);
        mitm::applet::g_nvab_armed    = ArmFileContains("nvab");
        mitm::applet::g_live_armed    = ArmFileContains("live");
        mitm::applet::g_detach_on_home = !ArmFileContains("nodethome");
        mitm::applet::g_ui_stream_armed = ArmFileContains("homemenu");
        mitm::applet::LogLine("home: HOME streaming is %s (home_menu, read at boot)", mitm::applet::g_ui_stream_armed ? "on" : "off");
        g_screen_off.store(ArmFileContains("screenoff"), std::memory_order_relaxed);
        mitm::applet::g_early_collect = ArmFileContains("earlyoff") ? 0 : ArmFileContains("earlyab") ? 2 : 1;
        mitm::applet::g_mid_collect   = ArmFileContains("midoff") ? 0 : ArmFileContains("midon") ? 1 : ArmFileContains("midab") ? 2 : mitm::applet::MidCollectDefault;
        mitm::applet::g_max_fps.store(ArmFileNumber("maxfps", 0), std::memory_order_relaxed);
        mitm::applet::g_usb_gop.store(ArmFileNumber("usbgop", mitm::applet::keyframe::UsbGopDefault), std::memory_order_relaxed);
        mitm::applet::g_nv1080_armed  = ArmFileContains("nv1080");
        mitm::applet::g_nv1080_n      = ArmFileNumber("nv1080", 120);
        mitm::applet::g_cap720_armed  = ArmFileContains("cap720");
        mitm::applet::g_anyfw_armed   = ArmFileContains("anyfw");
        mitm::applet::g_shot_enabled.store(ArmFileContains("shot"));
        mitm::applet::g_shot_combo.store(std::min(ArmFileNumber("shotkey", 0), 4u));
        mitm::applet::g_shot_mask.store(ArmFileNumber("shotmask", 0) & sft::shotbuttons::AllButtons);
        mitm::applet::g_shot_capture.store(ArmFileContains("shotcap"));
        mitm::applet::g_shot_album.store(ArmFileContains("shotalbum"));
        mitm::applet::NoteAlbumPath();
        mitm::applet::g_audio_enabled.store(ArmFileContains("audio"));
        mitm::applet::LogLine("install type from GetEntryType(sdmc:/applet-mitm.armed) rc=0x%x", mitm::applet::g_arm_probe_rc);
        mitm::applet::LogLine("%s: %.*s", mitm::applet::g_release_mode ? "release install, sdmc:/config/nxstream/tap/config.ini ->" : "TEST install, sdmc:/applet-mitm.armed",
                              static_cast<int>(g_arm_len), g_arm_buf);
        if (mitm::applet::g_live_armed) { mitm::applet::g_nvstream_armed = true; }
        mitm::applet::LoadStreamEnabled();
        mitm::applet::LoadExcluded();
        if (!mitm::applet::g_live_armed) { mitm::applet::g_live_state.store(mitm::applet::LiveState_NotArmed); }
        mitm::applet::g_nvp_armed     = ArmFileContains("nvp");
        mitm::applet::g_nvp_n         = ArmFileNumber("nvp", 30);
        mitm::applet::g_matrix_mode   = ArmFileNumber("mtx", 1);
        mitm::applet::g_stream_armed  = ArmFileContains("stream");
        mitm::applet::g_stream_w      = ArmFileNumber("sw", 0);
        mitm::applet::g_stream_h      = ArmFileNumber("sh", 0);
        mitm::applet::g_stream_auto   = (mitm::applet::g_stream_w == 0 ||
                                         mitm::applet::g_stream_h == 0);
        mitm::applet::g_stream_frames = ArmFileNumber("sframes", 600);
        mitm::applet::g_probe_delay_s = ArmFileNumber("wait", 120);
        mitm::applet::LogLine("ARMED FLAGS: vic=%d exec=%d dbg=%d dump=%d usb=%d bench=%d nvenc=%d "
                              "jpg=%d sweep=%d mtx=%d stream=%d grc=%d grcscan=%d clk=%d jpgdec=%d nvgrc=%d csc=%d nvframe=%d(%u) nvstream=%d(%u, qp %u, gop %u) nvp=%d(%u) live=%d nv1080=%d(%u) wait=%u",
                              mitm::applet::g_vic_armed, mitm::applet::g_vic_execute,
                              mitm::applet::g_dbg_armed, mitm::applet::g_dump_armed,
                              g_usb_armed, mitm::applet::g_bench_armed,
                              mitm::applet::g_nvenc_armed, mitm::applet::g_nvjpg_armed,
                              mitm::applet::g_sweep_armed, mitm::applet::g_matrix_armed,
                              mitm::applet::g_stream_armed, mitm::applet::g_grc_armed,
                              mitm::applet::g_grcscan_armed, mitm::applet::g_clk_armed,
                              mitm::applet::g_jpgdec_armed, mitm::applet::g_nvgrc_armed,
                              mitm::applet::g_csc_armed, mitm::applet::g_nvframe_armed, mitm::applet::g_nvframe_n,
                              mitm::applet::g_nvstream_armed, mitm::applet::g_nvstream_n, mitm::applet::g_nvstream_qp,
                              mitm::applet::g_nvstream_gop,
                              mitm::applet::g_nvp_armed, mitm::applet::g_nvp_n,
                              mitm::applet::g_live_armed,
                              mitm::applet::g_nv1080_armed, mitm::applet::g_nv1080_n,
                              mitm::applet::g_probe_delay_s);
        mitm::applet::LogLine("detach-on-home=%s (exp: end session when presents stall ~1s; config detach_on_home=0 to keep)",
                              mitm::applet::g_detach_on_home ? "on" : "off");
        if ((mitm::applet::g_jpgdec_armed || mitm::applet::g_nvgrc_armed ||
             mitm::applet::g_csc_armed || mitm::applet::g_nvframe_armed || mitm::applet::g_nvstream_armed ||
             mitm::applet::g_nvp_armed) && !mitm::applet::g_vic_armed) {
            mitm::applet::LogLine("jpgdec/nvgrc/csc/nvframe/nvstream armed without vic: they run inside the VIC worker, so they will NOT run");
        }
        if ((mitm::applet::g_nvframe_armed || mitm::applet::g_nvstream_armed || mitm::applet::g_nvp_armed || mitm::applet::g_nv1080_armed) && !mitm::applet::g_dbg_armed) {
            mitm::applet::LogLine("nvframe/nvstream/nvp armed without dbg: they run inside the debug capture, so they will NOT run");
        }
        if ((mitm::applet::g_csc_armed || mitm::applet::g_nvframe_armed || mitm::applet::g_nvstream_armed ||
             mitm::applet::g_nvp_armed) && !mitm::applet::g_vic_execute) {
            mitm::applet::LogLine("csc/nvframe/nvstream/nvp armed without exec: the VIC buffers are never mapped, so they will NOT run");
        }
        if (mitm::applet::g_nvstream_armed && !g_usb_armed) {
            mitm::applet::LogLine("nvstream armed without usb: the USB device is never brought up, so it will NOT stream");
        }
        mitm::applet::LogLine("arm file (sdmc:/applet-mitm.armed): vic=%s exec=%s",
                              mitm::applet::g_vic_armed   ? "ARMED" : "absent - observer only",
                              mitm::applet::g_vic_execute ? "PhaseB-full-blit" : "PhaseA-noop-cmdbuf");
        if (mitm::applet::g_stream_armed) {
            if (mitm::applet::g_stream_auto) {
                mitm::applet::LogLine("stream ARMED: size AUTO (from the negotiated link speed), %u frames",
                                      mitm::applet::g_stream_frames);
            } else {
                mitm::applet::LogLine("stream ARMED: %ux%u packed-420, %u frames (%u B/frame); 60 fps needs <=16667 us/frame",
                                      mitm::applet::g_stream_w, mitm::applet::g_stream_h,
                                      mitm::applet::g_stream_frames,
                                      mitm::applet::g_stream_w * mitm::applet::g_stream_h * 3 / 2);
            }
        }
        LogMemoryPools();
        BallastRead();
        if (g_ballast_delay_s == 0) { BallastTake(); }
        if (mitm::applet::g_stream_enabled.load(std::memory_order_relaxed)) {
            TryUsbEnumerate();
        } else {
            mitm::applet::NoteUsbLeftAlone();
        }

        mitm::applet::LogLine("probe fires at t=%u s; frame dump to SD: %s",
                              mitm::applet::g_probe_delay_s,
                              mitm::applet::g_dump_armed ? "ARMED (freezes the game ~0.3-2.5 s)" : "off");
        mitm::applet::LogLine("debug-capture route: %s   pmdmntInitialize rc=0x%x",
                              mitm::applet::g_dbg_armed ? "ARMED" : "off (add \"dbg\" to the arm file)",
                              mitm::applet::g_pmdmnt_rc);

        SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_hb_thread), HeartbeatThread, nullptr,
                                        g_hb_stack, sizeof(g_hb_stack),
                                        mitm::applet::ThreadBasePriority()));
        os::SetThreadNamePointer(std::addressof(g_hb_thread), "applet-mitm.HB");
        os::StartThread(std::addressof(g_hb_thread));
        mitm::applet::NoteThread(std::addressof(g_hb_thread));
        mitm::applet::LogMark("main:heartbeat_started");

        mitm::applet::StartControlService();
        mitm::applet::StartAppWatch();
        mitm::applet::StartShotInput();
        mitm::applet::StartAudio();
        mitm::applet::SoundProbeStart();
        mitm::applet::OvlProbeStart();
        mitm::applet::StartUsbOutReader();
        mitm::applet::StartNet();         
        mitm::applet::StartVicWorker();
        mitm::applet::StartAppletWatch();
        mitm::applet::StartClockProbe(mitm::applet::g_jpgdec_armed || mitm::applet::g_nvenc_armed ||
                                      mitm::applet::g_nvjpg_armed || mitm::applet::g_nvgrc_armed ||
                                      mitm::applet::g_nvframe_armed || mitm::applet::g_nvstream_armed ||
                                      mitm::applet::g_nvp_armed);

        ::ams::sf::impl::g_tier4_create_session = mitm::applet::CreateSessionViaSm;
        {
            os::NativeHandle ws = os::InvalidNativeHandle, wc = os::InvalidNativeHandle;
            const Result wrc = mitm::applet::CreateSessionViaSm(std::addressof(ws), std::addressof(wc));
            mitm::applet::LogLine("sessions: warm-up pair rc=0x%x (server 0x%x, client 0x%x) - closed", wrc.GetValue(), ws, wc);
            if (R_SUCCEEDED(wrc)) { os::CloseNativeHandle(wc); os::CloseNativeHandle(ws); }
        }
        SFT_ABORT_UNLESS(g_server_manager.RegisterMitmServer<mitm::applet::ViRootMitm>(PortIndex_AppletMitm, AppletMitmServiceName));
        mitm::applet::LogLine("registered mitm server for vi:u");
        mitm::applet::g_hb_docked.store(ArmFileContains("hbdocked"), std::memory_order_relaxed);
        if (mitm::applet::g_hb_docked.load()) { mitm::applet::LogLine("am: homebrew from the Homebrew Menu is told the console is docked (homebrew_docked)"); }
        mitm::applet::g_force_docked.store(ArmFileContains("forcedock"), std::memory_order_relaxed);
        if (mitm::applet::g_force_docked.load()) { mitm::applet::LogLine("am: games are told the console is docked (force_docked)"); }
        mitm::applet::g_vpad_enabled.store(ArmFileContains("vinput"), std::memory_order_relaxed);
        if (mitm::applet::g_vpad_enabled.load()) { mitm::applet::LogLine("vpad: a controller on the viewer's computer may play the console (viewer_input)"); }
        mitm::applet::g_scale1080.store(ArmFileContains("scale1080"), std::memory_order_relaxed);
        mitm::applet::g_memory_boost.store(ArmFileContains("memboost"), std::memory_order_relaxed);
        if (mitm::applet::g_scale1080.load()) { mitm::applet::LogLine("nvstream: docked pictures between 720p and 1080p are scaled up to 1080p (scale_to_1080)"); }
        if (ArmFileContains("noam")) { mitm::applet::LogLine("am: not wrapped (noam)"); } else { mitm::applet::StartAmMitm(); }
        if (ArmFileContains("noaudout")) { mitm::applet::LogLine("audout: not wrapped (noaudout)"); } else { mitm::applet::StartAudOutMitm(); }
        mitm::applet::OverlayArm(ArmFileContains("overlays"));
        if (ArmFileContains("nohb")) {
            mitm::applet::LogLine("vi:m NOT wrapped (homebrew = 0): homebrew apps are not streamed this boot");
        } else {
            SFT_ABORT_UNLESS(g_server_manager.RegisterMitmServer<mitm::applet::ViManagerRootMitm>(PortIndex_ViManagerMitm, ViManagerMitmServiceName));
            mitm::applet::LogLine("registered mitm server for vi:m (homebrew applications only)");
        }

        {
            const s32 prio = mitm::applet::ServerThreadPriority;
            for (size_t i = 0; i < ViThreads - 1; ++i) {
                SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_vi_threads[i]), ViLoopThread, nullptr,
                                                g_vi_stacks[i], sizeof(g_vi_stacks[i]), prio));
                os::SetThreadNamePointer(std::addressof(g_vi_threads[i]), "applet-mitm.Vi");
                os::StartThread(std::addressof(g_vi_threads[i]));
                mitm::applet::NoteThread(std::addressof(g_vi_threads[i]));
            }
            mitm::applet::LogLine("vi server: %zu threads, %zu sessions", ViThreads, MaxSessions);
        }
        StartUiBinderMitm();
        {
            const Result src = mitm::applet::SmReleaseBoot();
            mitm::applet::LogLine("sm: boot session detached (rc=0x%x) and released - sm is now opened only while a service is fetched", src.GetValue());
        }
        LogAppletLimits("after sm released");
        mitm::applet::LogMark("main:LoopProcess");
        os::ChangeThreadPriority(os::GetCurrentThread(), mitm::applet::ServerThreadPriority);
        mitm::applet::LoopProcessSafely(g_server_manager, "vi");
        mitm::applet::LogMark("main:LoopProcess_RETURNED");
    }

}

namespace ams::mitm::applet {

    void ReloadReleaseConfig() {
        if (!g_release_mode) { LogLine("nxtap: ReloadConfig ignored - test install (arm file)"); return; }
        ::ams::g_arm_loaded = false;
        ::ams::LoadArm();
        ::ams::ApplySessionSettings();
        LogLine("nxtap: config reloaded -> %.*s", static_cast<int>(::ams::g_arm_len), ::ams::g_arm_buf);
        g_reconfig_request.store(true, std::memory_order_relaxed);
    }

    bool UsbIsSuperSpeed() {
        UsbDeviceSpeed sp = UsbDeviceSpeed_None;
        if (R_FAILED(usbDsGetSpeed(std::addressof(sp)))) { return false; }
        return sp == UsbDeviceSpeed_Super;
    }

    void UsbCancelIn();

    namespace {
        constinit std::atomic<u32> g_viewer_reason{0};

        bool UsbPostWait(void *buf, u32 size, u32 timeout_ms) {
            u32 urb = 0;
            if (R_FAILED(usbDsEndpoint_PostBufferAsync(::ams::g_usb_ep_in, buf, size, std::addressof(urb)))) { return false; }
            if (R_FAILED(eventWait(std::addressof(::ams::g_usb_ep_in->CompletionEvent), static_cast<u64>(timeout_ms) * 1000000))) {
                UsbCancelIn();
                return false;
            }
            eventClear(std::addressof(::ams::g_usb_ep_in->CompletionEvent));
            return true;
        }

        void UsbSendStatus(u32 reason, bool net = false) {
            struct Status { u32 version; u32 reason; char head[64]; char detail[120]; };
            static_assert(sizeof(Status) == 192);
            static const char *const text[][2] = {
                { "", "" },
                { "Connected - no game is running", "Start a game and the picture appears here." },
                { "Connected - the game is not on screen", "The HOME menu and system screens can't be streamed. Go back to the game." },
                { "Paused - a manual, the eShop or another applet is open", "The stream comes back a few seconds after you close it." },
                { "Streaming is turned off", "Turn it on in the NXStream Native overlay or manager app." },
                { "This app is excluded from streaming", "Remove it from the excluded apps in the manager app." },
                { "This firmware is untested", "Set allow_untested_firmware = 1 in config.ini to stream anyway." },
                { "The video engine stopped responding", "Restart the console to stream again." },
                { "Not enough memory to stream right now", "It tries again every few seconds." },
                { "This game's picture was not found", "Restart the game. If it keeps happening, please send the log." },
                { "Starting the stream...", "" },
                { "Paused - the keyboard is open", "There is no memory left to stream while it is open. The stream comes back when you close it." },
                { "This game draws above 1080p", "NXStream Native streams up to 1920x1080." },
                { "The USB link is too slow (USB 1.1, 12 Mbps)", "Try another USB hub, adapter or cable, or use Network mode." },
            };
            static_assert(sizeof(text) / sizeof(text[0]) == ViewerReason_UsbSlow + 1, "a text for every reason");
            if (reason >= sizeof(text) / sizeof(text[0])) { return; }
            alignas(0x1000) static u8 hdr_buf[0x1000];
            alignas(0x1000) static u8 body_buf[0x1000];
            const u32 h[8] = { 0x52544653u, 2u | ((8u | 16u | VPadHdrFlag()) << 16), 0, 0, 0, static_cast<u32>(sizeof(Status)), 0, 0 };
            std::memcpy(hdr_buf, h, sizeof(h));
            Status st = {};
            st.version = 1;
            st.reason = reason;
            std::snprintf(st.head, sizeof(st.head), "%s", text[reason][0]);
            std::snprintf(st.detail, sizeof(st.detail), "%s", text[reason][1]);
            std::memcpy(body_buf, std::addressof(st), sizeof(st));
            if (net) {
                if (NetSendBuffer(hdr_buf, sizeof(h))) { static_cast<void>(NetSendBuffer(body_buf, sizeof(st))); }
                return;
            }
            if (UsbPostWait(hdr_buf, sizeof(h), 300)) { static_cast<void>(UsbPostWait(body_buf, sizeof(st), 300)); }
        }
    }

    namespace {
        constexpr u32 InfoFlag = 32;

        constinit u32 g_app_sent_serial = 0;

        bool SendConsoleInfo(bool net, bool live = false) {
            static char s_info[256];
            static size_t s_len = 0;
            static bool s_read = false;
            if (!s_read) {
                s_read = true;
                SetSysFirmwareVersion fw = {};
                SetSysDeviceNickName nick = {};
                bool fw_ok = false, nick_ok = false;
                {
                    SmUse sm_set;
                    if (R_SUCCEEDED(::setsysInitialize())) {
                        fw_ok = R_SUCCEEDED(::setsysGetFirmwareVersion(std::addressof(fw)));
                        nick_ok = R_SUCCEEDED(::setsysGetDeviceNickname(std::addressof(nick)));
                        ::setsysExit();
                    }
                }
                nick.nickname[sizeof(nick.nickname) - 1] = '\0';
                for (char *p = nick.nickname; *p != '\0'; ++p) { if (static_cast<unsigned char>(*p) < 0x20) { *p = ' '; } }
                int n = 0;
                if (nick_ok && nick.nickname[0] != '\0') { n += std::snprintf(s_info + n, sizeof(s_info) - n, "name=%s\n", nick.nickname); }
                if (fw_ok) { n += std::snprintf(s_info + n, sizeof(s_info) - n, "firmware=%u.%u.%u\n", static_cast<unsigned>(fw.major), static_cast<unsigned>(fw.minor), static_cast<unsigned>(fw.micro)); }
                s_len = n > 0 && static_cast<size_t>(n) < sizeof(s_info) ? static_cast<size_t>(n) : 0;
                LogLine("info: console info for the viewer: name %s, firmware %s", nick_ok ? "read" : "unavailable", fw_ok ? "read" : "unavailable");
            }
            alignas(0x1000) static u8 hdr_buf[0x1000];
            alignas(0x1000) static u8 body_buf[0x1000];
            char app[512];
            size_t app_len = 0;
            const u32 serial = AppInfoFormat(app, sizeof(app), &app_len);
            const size_t total = s_len + app_len;
            if (total == 0 || total > sizeof(body_buf)) { g_app_sent_serial = serial; return true; }
            const u32 h[8] = { 0x52544653u, 2u | ((InfoFlag | VPadHdrFlag()) << 16), 0, 0, 0, static_cast<u32>(total), 0, 0 };
            std::memcpy(hdr_buf, h, sizeof(h));
            std::memcpy(body_buf, s_info, s_len);
            std::memcpy(body_buf + s_len, app, app_len);
            bool ok = false;
            if (live) {
                size_t sent = 0;
                ok = UsbSendBuffer(hdr_buf, sizeof(h), std::addressof(sent)) && UsbSendBuffer(body_buf, total, std::addressof(sent));
            } else if (net) {
                ok = NetSendBuffer(hdr_buf, sizeof(h)) && NetSendBuffer(body_buf, total);
                if (!ok) { NetDropClient("info send failed"); }
            } else {
                ok = UsbPostWait(hdr_buf, sizeof(h), 300) && UsbPostWait(body_buf, static_cast<u32>(total), 300);
            }
            if (ok) { g_app_sent_serial = serial; }
            return ok;
        }

        constinit u32 g_info_serial = 0;
        constinit bool g_info_usb_sent = false;
    }

    bool SendConsoleInfoIfDue() {
        if (AppInfoSerial() == g_app_sent_serial) { return true; }
        if (!(g_tx_net.load(std::memory_order_relaxed) ? NetClientPresent() : UsbReady())) { return true; }
        return SendConsoleInfo(false, true);
    }

    void SetViewerReason(ViewerReason r) { g_viewer_reason.store(r, std::memory_order_relaxed); }

    namespace {
        constinit bool g_usb_released = false;
    }

    void NoteUsbLeftAlone() {
        g_usb_released = true;
        LogLine("usb: streaming is off - the USB port is left to other apps until it is switched on");
    }

    void UsbFollowStreamSwitch() {
        if (!::ams::g_usb_armed || g_uvc_mode) { return; }
        const bool on = g_stream_enabled.load(std::memory_order_relaxed);
        if (!on && !g_usb_released && ::ams::g_usb_ep_in != nullptr) {
            static_cast<void>(usbDsEndpoint_Cancel(::ams::g_usb_ep_in));
            UsbOutPark();
            if (::ams::g_usb_ep_out != nullptr) { static_cast<void>(usbDsEndpoint_Cancel(::ams::g_usb_ep_out)); }
            usbDsExit();
            ::ams::g_usb_iface = nullptr;
            ::ams::g_usb_ep_in = nullptr;
            ::ams::g_usb_ep_out = nullptr;
            g_usb_released = true;
            LogLine("usb: streaming switched off - USB port released for other apps");
        } else if (on && g_usb_released) {
            g_usb_released = false;
            LogLine("usb: streaming switched on - taking the USB port again");
            SmUse sm_usb;
            ::ams::TryUsbEnumerate();
            UsbOutUnpark();
        }
    }

    bool UsbViewerPresent(u32 timeout_ms) {
        if (NetClientPresent()) {
            static u64 s_status_ns = 0;
            if (const u32 serial = NetClientSerial(); serial != g_info_serial) {
                g_info_serial = serial;
                SendConsoleInfo(true);
            } else if (AppInfoSerial() != g_app_sent_serial) {
                SendConsoleInfo(true);
            }
            const u64 now = armTicksToNs(armGetSystemTick());
            if (now - s_status_ns >= UINT64_C(1000000000)) {
                s_status_ns = now;
                if (const u32 r = g_viewer_reason.load(std::memory_order_relaxed); r != ViewerReason_None) { UsbSendStatus(r, true); }
                else {
                    const u32 hello[8] = { 0x52544653u, 2u | (static_cast<u32>(VPadHdrFlag()) << 16), 0, 0, 0, 0, 0, 0 };
                    static_cast<void>(NetSendBuffer(hello, sizeof(hello)));
                }
            }
            return true;
        }
        if (!UsbReady()) { g_info_usb_sent = false; return false; }
        if (g_uvc_mode) {
            for (u32 t = 0; !g_uvc_streaming.load(std::memory_order_relaxed); t += 50) {
                if (t >= timeout_ms) { return false; }
                os::SleepThread(TimeSpan::FromMilliSeconds(50));
            }
            return !UvcHostStalled() || UvcHostReading(timeout_ms);
        }
        alignas(0x1000) static u8 hello[0x1000];
        const u32 h[8] = { 0x52544653u, 2u | (static_cast<u32>(VPadHdrFlag()) << 16), 0, 0, 0, 0, 0, 0 };
        std::memcpy(hello, h, sizeof(h));
        u32 urb = 0;
        if (R_FAILED(usbDsEndpoint_PostBufferAsync(::ams::g_usb_ep_in, hello, sizeof(h), std::addressof(urb)))) { g_info_usb_sent = false; return false; }
        if (R_FAILED(eventWait(std::addressof(::ams::g_usb_ep_in->CompletionEvent), static_cast<u64>(timeout_ms) * 1000000))) {
            UsbCancelIn();
            g_info_usb_sent = false;
            return false;
        }
        eventClear(std::addressof(::ams::g_usb_ep_in->CompletionEvent));
        UsbDsReportData report = {};
        u32 transferred = 0;
        if (R_FAILED(usbDsEndpoint_GetReportData(::ams::g_usb_ep_in, std::addressof(report)))) { return false; }
        if (R_FAILED(usbDsParseReportData(std::addressof(report), urb, nullptr, std::addressof(transferred)))) { return false; }
        if (transferred != sizeof(h)) { return false; }
        if (UsbDeviceSpeed neg = UsbDeviceSpeed_None; R_SUCCEEDED(usbDsGetSpeed(std::addressof(neg))) && neg == UsbDeviceSpeed_Full) {
            static bool said = false;
            if (!said) { said = true; LogLine("usb: the viewer is on a Full Speed link (USB 1.1, 12 Mbps) - too slow to stream; it is told so"); }
            UsbSendStatus(ViewerReason_UsbSlow);
            return false;
        }
        if (!g_info_usb_sent) {
            g_info_usb_sent = true;
            SendConsoleInfo(false);
        } else if (AppInfoSerial() != g_app_sent_serial) {
            SendConsoleInfo(false);
        }
        if (const u32 r = g_viewer_reason.load(std::memory_order_relaxed); r != ViewerReason_None) { UsbSendStatus(r); }
        return true;
    }

    void ViewerKeepAlive() {
        if (g_uvc_mode) { return; }
        static u64 s_ns = 0;
        const u64 now = armTicksToNs(armGetSystemTick());
        if (now - s_ns < UINT64_C(1000000000)) { return; }
        s_ns = now;
        static_cast<void>(UsbViewerPresent(100));
    }

    bool UsbReady() {
        if (::ams::g_usb_ep_in == nullptr && !g_uvc_mode) { return false; }
        UsbState st = UsbState_Detached;
        if (R_FAILED(usbDsGetState(std::addressof(st)))) { return false; }
        return st == UsbState_Configured;
    }

    bool UsbPostAsync(const void *buf, size_t len, u32 *out_urb) {
        if (out_urb != nullptr) { *out_urb = 0; }
        if (g_tx_net.load(std::memory_order_relaxed)) {
            if (NetPostAsync(buf, len)) { return true; }
            NetDropClient("send failed");
            return false;
        }
        if (::ams::g_usb_ep_in == nullptr) { return false; }

        u32 urb_id = 0;
        const ::Result rc = usbDsEndpoint_PostBufferAsync(::ams::g_usb_ep_in,
                                const_cast<void *>(buf), static_cast<u32>(len),
                                std::addressof(urb_id));
        if (R_FAILED(rc)) {
            LogLine("   usb: PostBufferAsync(%zu B) rc=0x%x", len, rc);
            return false;
        }
        if (out_urb != nullptr) { *out_urb = urb_id; }
        return true;
    }

    void UsbCancelIn() {
        if (::ams::g_usb_ep_in == nullptr) { return; }
        static_cast<void>(usbDsEndpoint_Cancel(::ams::g_usb_ep_in));
        static_cast<void>(eventWait(std::addressof(::ams::g_usb_ep_in->CompletionEvent), UINT64_C(1000000000)));
        eventClear(std::addressof(::ams::g_usb_ep_in->CompletionEvent));
    }

    bool UsbWaitAsync(u32 urb, size_t *out_sent) {
        if (out_sent != nullptr) { *out_sent = 0; }
        if (g_tx_net.load(std::memory_order_relaxed)) {
            if (NetWaitAsync(out_sent)) { return true; }
            NetDropClient("the connection dropped");
            return false;
        }
        if (::ams::g_usb_ep_in == nullptr) { return false; }

        ::Result rc = eventWait(std::addressof(::ams::g_usb_ep_in->CompletionEvent), ::ams::UsbTimeoutNs);
        if (R_FAILED(rc)) {
            LogLine("   usb: async completion timed out rc=0x%x (host not draining?) - cancelled", rc);
            UsbCancelIn();
            return false;
        }
        eventClear(std::addressof(::ams::g_usb_ep_in->CompletionEvent));

        UsbDsReportData report = {};
        rc = usbDsEndpoint_GetReportData(::ams::g_usb_ep_in, std::addressof(report));
        if (R_FAILED(rc)) { LogLine("   usb: async GetReportData rc=0x%x", rc); return false; }

        u32 transferred = 0;
        rc = usbDsParseReportData(std::addressof(report), urb, nullptr, std::addressof(transferred));
        if (R_FAILED(rc)) { LogLine("   usb: async ParseReportData rc=0x%x", rc); return false; }

        if (out_sent != nullptr) { *out_sent = transferred; }
        return transferred > 0;
    }

    void SetScreenOffNow(bool on) { ::ams::g_screen_off.store(on, std::memory_order_relaxed); }

    namespace {
        constinit std::atomic<bool> g_out_bye{false};
        constinit std::atomic<bool> g_out_park{false};
        constinit std::atomic<bool> g_out_parked{true};
        constinit bool g_out_started = false;
        alignas(os::ThreadStackAlignment) constinit u8 g_out_stack[8_KB];
        constinit os::ThreadType g_out_thread;

        void UsbOutHandle(const u8 *p, u32 got) {
            namespace vm = sft::viewermsg;
            for (u32 k = 0; k < got; ) {
                const vm::Msg m = vm::Next(p + k, got - k);
                if (m.kind == vm::Kind::Partial) { break; }
                switch (m.kind) {
                    case vm::Kind::Key:    g_viewer_key_request.store(true, std::memory_order_relaxed); g_viewer_can_ask.store(true, std::memory_order_relaxed); break;
                    case vm::Kind::Crop:   g_viewer_crop.store(true, std::memory_order_relaxed); break;
                    case vm::Kind::NoCrop: g_viewer_crop.store(false, std::memory_order_relaxed); break;
                    case vm::Kind::Lock:   ViewerLockSize(true); break;
                    case vm::Kind::Unlock: ViewerLockSize(false); break;
                    case vm::Kind::Bye:
                        g_viewer_crop.store(false, std::memory_order_relaxed);
                        ViewerLockSize(false);
                        VPadRelease("the viewer closed");
                        g_viewer_can_ask.store(false, std::memory_order_relaxed);
                        g_out_bye.store(true);
                        break;
                    case vm::Kind::Pad:    VPadFromViewer(vm::ReadPad(p + k)); break;
                    default: break;
                }
                k += static_cast<u32>(m.len);
            }
        }

        void UsbOutThread(void *) {
            UsbDsEndpoint *ep = nullptr;
            u32 urb = 0, fails = 0, idle = 0;
            for (;;) {
                VPadTick();
                const bool ready = !g_out_park.load() && ::ams::g_usb_ep_out != nullptr &&
                                   ((ep != nullptr && idle < 10) || UsbReady());
                if (!ready) {
                    if (ep != nullptr) {
                        static_cast<void>(usbDsEndpoint_Cancel(ep));
                        static_cast<void>(eventWait(std::addressof(ep->CompletionEvent), UINT64_C(100000000)));
                        eventClear(std::addressof(ep->CompletionEvent));
                        ep = nullptr;
                    }
                    g_out_parked.store(true);
                    os::SleepThread(TimeSpan::FromMilliSeconds(250));
                    continue;
                }
                if (idle >= 10) { idle = 0; }
                g_out_parked.store(false);
                if (g_out_park.load()) { continue; }
                if (ep == nullptr) {
                    UsbDsEndpoint *const e = ::ams::g_usb_ep_out;
                    eventClear(std::addressof(e->CompletionEvent));
                    std::memset(::ams::g_usb_ep_out_buf, 0, 64);
                    if (R_FAILED(usbDsEndpoint_PostBufferAsync(e, ::ams::g_usb_ep_out_buf, sizeof(::ams::g_usb_ep_out_buf), std::addressof(urb)))) {
                        if (++fails == 1) { LogLine("usb: the viewer's messages can't be read (OUT post failed) - trying again"); }
                        os::SleepThread(TimeSpan::FromMilliSeconds(500));
                        continue;
                    }
                    ep = e;
                    idle = 0;
                }
                if (R_FAILED(eventWait(std::addressof(ep->CompletionEvent), UINT64_C(100000000)))) { ++idle; continue; }
                eventClear(std::addressof(ep->CompletionEvent));
                UsbDsEndpoint *const done = ep;
                ep = nullptr;
                UsbDsReportData report = {};
                u32 got = 0;
                if (R_FAILED(usbDsEndpoint_GetReportData(done, std::addressof(report))) ||
                    R_FAILED(usbDsParseReportData(std::addressof(report), urb, nullptr, std::addressof(got)))) {
                    os::SleepThread(TimeSpan::FromMilliSeconds(10));
                    continue;
                }
                fails = 0;
                UsbOutHandle(::ams::g_usb_ep_out_buf, std::min<u32>(got, sizeof(::ams::g_usb_ep_out_buf)));
            }
        }
    }

    void StartUsbOutReader() {
        if (!::ams::g_usb_armed || g_uvc_mode || g_out_started) { return; }
        if (R_FAILED(os::CreateThread(std::addressof(g_out_thread), UsbOutThread, nullptr, g_out_stack, sizeof(g_out_stack),
                                      mitm::applet::ThreadBasePriority()))) {
            LogLine("usb: no thread for the viewer's messages - goodbyes and requests are not read this boot");
            return;
        }
        os::SetThreadNamePointer(std::addressof(g_out_thread), "applet-mitm.UsbOut");
        os::StartThread(std::addressof(g_out_thread));
        NoteThread(std::addressof(g_out_thread));
        g_out_started = true;
    }

    void UsbOutPark() {
        g_out_park.store(true);
        if (!g_out_started) { return; }
        for (int i = 0; i < 50 && !g_out_parked.load(); ++i) { os::SleepThread(TimeSpan::FromMilliSeconds(10)); }
        if (!g_out_parked.load()) { LogLine("usb: the OUT reader did not give its buffer back in 500 ms"); }
    }

    void UsbOutUnpark() { g_out_park.store(false); }

    bool UsbViewerSaidBye() {
        if (g_tx_net.load(std::memory_order_relaxed) || g_uvc_mode) { return false; }
        return g_out_bye.exchange(false);
    }

    void UsbByeStop() { g_out_bye.store(false); }

    bool UsbSendBuffer(const void *buf, size_t len, size_t *out_sent) {
        if (out_sent != nullptr) { *out_sent = 0; }
        if (g_tx_net.load(std::memory_order_relaxed)) {
            if (NetSendBuffer(buf, len)) { if (out_sent != nullptr) { *out_sent = len; } return true; }
            NetDropClient("the connection dropped");
            return false;
        }
        if (::ams::g_usb_ep_in == nullptr) { return false; }

        const u8 *p = static_cast<const u8 *>(buf);
        size_t done = 0;

        while (done < len) {
            const size_t chunk = ((len - done) < ::ams::UsbChunk) ? (len - done) : ::ams::UsbChunk;

            u32 urb_id = 0;
            ::Result rc = usbDsEndpoint_PostBufferAsync(::ams::g_usb_ep_in,
                              const_cast<u8 *>(p + done), static_cast<u32>(chunk),
                              std::addressof(urb_id));
            if (R_FAILED(rc)) {
                LogLine("   usb: PostBufferAsync(+%zu, %zu B) rc=0x%x", done, chunk, rc);
                return false;
            }

            rc = eventWait(std::addressof(::ams::g_usb_ep_in->CompletionEvent), ::ams::UsbTimeoutNs);
            if (R_FAILED(rc)) {
                LogLine("   usb: completion wait timed out at +%zu rc=0x%x (host not reading?) - cancelled", done, rc);
                UsbCancelIn();
                return false;
            }
            eventClear(std::addressof(::ams::g_usb_ep_in->CompletionEvent));

            UsbDsReportData report = {};
            rc = usbDsEndpoint_GetReportData(::ams::g_usb_ep_in, std::addressof(report));
            if (R_FAILED(rc)) { LogLine("   usb: GetReportData rc=0x%x", rc); return false; }

            u32 transferred = 0;
            rc = usbDsParseReportData(std::addressof(report), urb_id, nullptr, std::addressof(transferred));
            if (R_FAILED(rc)) { LogLine("   usb: ParseReportData rc=0x%x", rc); return false; }

            if (transferred == 0) {
                LogLine("   usb: zero-length completion at +%zu - host is not draining", done);
                return false;
            }
            if (transferred != chunk) {
                done += transferred;
                if (out_sent != nullptr) { *out_sent = done; }
                LogLine("   usb: short completion %u/%zu at +%zu - stopping (would break alignment)",
                        transferred, chunk, done - transferred);
                return false;
            }

            done += transferred;
            if (out_sent != nullptr) { *out_sent = done; }
        }
        return true;
    }

}
