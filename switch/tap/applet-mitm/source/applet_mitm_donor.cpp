#include "applet_mitm_donor.hpp"
#include "applet_mitm_control.hpp"
#include "applet_mitm_sm.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_shot.hpp"
#include "applet_mitm_service.hpp"
#include "applet_mitm_nv.hpp"
#include "applet_mitm_uisource.hpp"

namespace ams::mitm::applet {

    namespace {

        constexpr u64 DonorProgramId = 0x4200000000004E55ull;

        constinit os::SdkMutex g_lock;
        constinit std::atomic<int> g_state{0};
        constinit os::NativeHandle g_handle = os::InvalidNativeHandle;
        constinit u64 g_size = 0;
        constinit os::NativeHandle g_nv_handle = os::InvalidNativeHandle;
        constinit u64 g_nv_size = 0;
        constinit u32 g_fail_rc = 0;
        constinit u64 g_pid = 0;
        constinit os::TransferMemoryType g_tmem = {};
        constinit bool g_mapped = false;
        constinit void *g_addr = nullptr;

        constinit bool g_pmshell_open = false;

        void TerminateDonor() {
            if (g_nv_handle != os::InvalidNativeHandle) { os::CloseNativeHandle(g_nv_handle); g_nv_handle = os::InvalidNativeHandle; }
            if (g_pid == 0) { return; }
            ::Result rc = 0;
            if (g_pmshell_open) {
                rc = ::pmshellTerminateProcess(g_pid);
            } else {
                SmUse sm_pm;
                const ::Result r0 = ::pmshellInitialize();
                rc = R_SUCCEEDED(r0) ? ::pmshellTerminateProcess(g_pid) : r0;
                if (R_SUCCEEDED(r0)) { ::pmshellExit(); }
            }
            if (g_pmshell_open) { ::pmshellExit(); g_pmshell_open = false; }
            LogLine("donor: pid %llu terminated rc=0x%x", static_cast<unsigned long long>(g_pid), rc);
            g_pid = 0;
        }

    }

    constinit std::atomic<bool> g_applet_needs_memory{false};

    namespace {
        bool WebAppletRunning() {
            static constexpr u64 Web[] = { 0x010000000000100Aull, 0x010000000000100Bull, 0x010000000000100Full, 0x0100000000001010ull, 0x0100000000001011ull,
                                           0x0100000000001042ull, 0x0100000000001043ull };
            for (const u64 id : Web) {
                os::ProcessId pid{};
                if (R_SUCCEEDED(::ams::pm::dmnt::GetProcessId(std::addressof(pid), ncm::ProgramId{id})) && pid.value != 0) { return true; }
            }
            return false;
        }

        constexpr u64 TightMb = 48;
        constexpr u64 WebRoomMb = 96;
        constexpr u32 MinDonorMb = 30, FullDonorMb = 42;
        constinit std::atomic<u32> g_cap_mb{FullDonorMb};

        struct Beside { u64 take_mb, stop_mb; const char *what; };
        constexpr Beside BesideOther    = { TightMb, TightMb, nullptr };
        constexpr Beside BesideAlbum    = { 12, 8, "the Album" };
        constexpr Beside BesideSettings = { 20, 14, "System Settings" };

        bool Running(u64 program_id, os::ProcessId *out_pid = nullptr) {
            os::ProcessId pid{};
            if (R_FAILED(::ams::pm::dmnt::GetProcessId(std::addressof(pid), ncm::ProgramId{program_id})) || pid.value == 0) { return false; }
            if (out_pid != nullptr) { *out_pid = pid; }
            return true;
        }

        bool Album100DIsHbl(bool *is_hbl) {
            os::ProcessId pid{};
            if (!Running(0x010000000000100Dull, std::addressof(pid))) { return false; }
            os::NativeHandle h = os::InvalidNativeHandle;
            ncm::ProgramLocation loc = {};
            cfg::OverrideStatus st = {};
            if (R_FAILED(::ams::pm::dmnt::AtmosphereGetProcessInfo(std::addressof(h), std::addressof(loc), std::addressof(st), pid))) { return false; }
            os::CloseNativeHandle(h);
            *is_hbl = st.IsHbl();
            return true;
        }
        bool RealAlbumRunning() {
            bool hbl = false;
            return Album100DIsHbl(std::addressof(hbl)) && !hbl;
        }

        bool SwitchUInstalled() {
            static u64 s_checked_ns = 0;
            static bool s_installed = false;
            const u64 now = armTicksToNs(armGetSystemTick());
            if (s_checked_ns == 0 || now - s_checked_ns >= UINT64_C(10000000000)) {
                s_checked_ns = now;
                fs::DirectoryEntryType t, q;
                s_installed = R_SUCCEEDED(fs::GetEntryType(std::addressof(t), "sdmc:/config/SwitchU")) && t == fs::DirectoryEntryType_Directory &&
                              R_SUCCEEDED(fs::GetEntryType(std::addressof(q), "sdmc:/atmosphere/contents/0100000000001000/exefs.nsp")) && q == fs::DirectoryEntryType_File;
            }
            return s_installed;
        }
        bool IsSwitchUMenu(os::ProcessId pid) {
            static constinit std::atomic<u64> s_known{0};
            const u64 known = s_known.load(std::memory_order_relaxed);
            if (known != 0 && (known >> 1) == pid.value) { return (known & 1) != 0; }
            os::NativeHandle h = os::InvalidNativeHandle;
            ncm::ProgramLocation loc = {};
            cfg::OverrideStatus st = {};
            if (R_FAILED(::ams::pm::dmnt::AtmosphereGetProcessInfo(std::addressof(h), std::addressof(loc), std::addressof(st), pid))) { return true; }
            u32 modules = 0;
            u64 addr = 0, biggest = 0;
            for (u32 guard = 0; guard < 4096; ++guard) {
                ::MemoryInfo mi = {};
                u32 pi = 0;
                if (R_FAILED(::svcQueryProcessMemory(&mi, &pi, h, addr)) || mi.size == 0) { break; }
                if ((mi.type & MemState_Type) == MemType_CodeStatic && mi.perm == Perm_Rx) {
                    ++modules;
                    if (mi.size > biggest) { biggest = mi.size; }
                }
                const u64 next = mi.addr + mi.size;
                if (next <= addr) { break; }
                addr = next;
            }
            os::CloseNativeHandle(h);
            if (modules == 0) { return true; }
            const bool menu = st.IsHbl() && modules <= 2 && biggest >= (1ull << 20);
            s_known.store(pid.value << 1 | (menu ? 1 : 0), std::memory_order_relaxed);
            LogLine("donor: 100D (pid %llu) is %s - %u code module(s), the biggest %llu KB, hbl flag %u", static_cast<unsigned long long>(pid.value),
                    menu ? "SwitchU's menu" : st.IsHbl() ? "hbmenu" : "the Album", modules, static_cast<unsigned long long>(biggest >> 10), st.IsHbl() ? 1u : 0u);
            return menu;
        }
        bool SwitchURunning() {
            os::ProcessId pid{};
            return SwitchUInstalled() && Running(0x010000000000100Dull, std::addressof(pid)) && IsSwitchUMenu(pid);
        }

        bool AlbumOrSwitchURunning() {
            return RealAlbumRunning() || SwitchURunning();
        }

        constexpr u64 HomeWithSettingsMb = 200;
        u64 HomeMenuMb() {
            os::ProcessId pid{};
            if (!Running(0x0100000000001000ull, std::addressof(pid))) { return 0; }
            os::NativeHandle h = os::InvalidNativeHandle;
            ncm::ProgramLocation loc = {};
            cfg::OverrideStatus st = {};
            if (R_FAILED(::ams::pm::dmnt::AtmosphereGetProcessInfo(std::addressof(h), std::addressof(loc), std::addressof(st), pid))) { return 0; }
            u64 used = 0;
            const bool ok = R_SUCCEEDED(::svcGetInfo(&used, InfoType_UsedMemorySize, h, 0));
            os::CloseNativeHandle(h);
            return ok ? used >> 20 : 0;
        }

        constinit std::atomic<u64> g_beside_free{~u64(0)};
        const Beside &BesideNow(u64 free_mb) {
            static constinit std::atomic<u64> &s_free = g_beside_free;
            static constinit std::atomic<int> s_kind{0};
            if (free_mb >= TightMb + FullDonorMb) { s_free.store(~u64(0), std::memory_order_relaxed); return BesideOther; }
            if (s_free.load(std::memory_order_relaxed) != free_mb) {
                const u64 home_mb = HomeMenuMb();
                const int kind = AlbumOrSwitchURunning() ? 1 : home_mb >= HomeWithSettingsMb ? 2 : 0;
                if (s_kind.exchange(kind, std::memory_order_relaxed) != kind || s_free.load(std::memory_order_relaxed) == ~u64(0)) {
                    LogLine("donor: %llu MB free in the Applet pool - %s (the HOME menu holds %llu MB)", static_cast<unsigned long long>(free_mb),
                            kind == 1 ? "the Album / SwitchU is open" : kind == 2 ? "System Settings is open" : "no Album or System Settings",
                            static_cast<unsigned long long>(home_mb));
                }
                s_free.store(free_mb, std::memory_order_relaxed);
            }
            const int k = s_kind.load(std::memory_order_relaxed);
            return k == 1 ? BesideAlbum : k == 2 ? BesideSettings : BesideOther;
        }

        constinit u64 g_beside_max_pid = 0;
        bool NewProcessBeside(u64 donor_pid, u64 *out_pid) {
            u64 &s_max_pid = g_beside_max_pid;
            u64 pids[96];
            s32 n = 0;
            if (R_FAILED(::svcGetProcessList(&n, pids, 96))) { return false; }
            u64 top = 0;
            for (s32 i = 0; i < n; ++i) { if (pids[i] != donor_pid && pids[i] > top) { top = pids[i]; } }
            const bool fresh = s_max_pid != 0 && top > s_max_pid;
            s_max_pid = top;
            *out_pid = top;
            return fresh;
        }
        u64 ProgramIdOf(u64 pid) {
            os::NativeHandle h = os::InvalidNativeHandle;
            ncm::ProgramLocation loc = {};
            cfg::OverrideStatus st = {};
            if (R_FAILED(::ams::pm::dmnt::AtmosphereGetProcessInfo(std::addressof(h), std::addressof(loc), std::addressof(st), os::ProcessId{pid}))) { return 0; }
            os::CloseNativeHandle(h);
            return loc.program_id.value;
        }
        u64 AppletFreeMb() {
            u64 cur = 0, lim = 0;
            if (R_FAILED(::ams::pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(cur), std::addressof(lim), ::ams::pm::ResourceLimitGroup_Applet, svc::LimitableResource_PhysicalMemoryMax))) { return 0; }
            return (lim - cur) >> 20;
        }
    }

    u32 DonorCapMb() { return g_cap_mb.load(std::memory_order_relaxed); }

    namespace {
        alignas(os::ThreadStackAlignment) constinit u8 g_watch_stack[8_KB];
        constinit os::ThreadType g_watch_thread;
        constinit u64 g_trigger_ns = 0;
        constinit std::atomic<u64> g_hold_until_ns{0};
        constinit std::atomic<u64> g_pool_moved_ns{0};
        constinit std::atomic<u64> g_donor_change_ns{0};
        constexpr u64 PoolCalmNs = 2'000'000'000ull;

        constexpr u64 SystemWebProgramId = 0x0100000000001042ull;
        constexpr u64 WebHookMaxHoldNs = 1'500'000'000ull;
        constinit os::NativeHandle g_web_hook = os::InvalidNativeHandle;
        constinit u64 g_web_hook_fired_ns = 0;

        void StartHookedWeb(const char *memory) {
            os::ProcessId pid{};
            ::Result rc = ::ams::pm::dmnt::GetProcessId(std::addressof(pid), ncm::ProgramId{SystemWebProgramId}).GetValue();
            if (R_SUCCEEDED(rc)) { rc = ::ams::pm::dmnt::StartProcess(pid).GetValue(); }
            LogLine("donor: systemWeb started (pid %llu) rc=0x%x - memory %s after %llu ms", static_cast<unsigned long long>(pid.value), rc, memory,
                    static_cast<unsigned long long>(g_web_hook_fired_ns != 0 ? (armTicksToNs(armGetSystemTick()) - g_web_hook_fired_ns) / 1000000 : 0));
        }

        void CloseWebHook() {
            os::CloseNativeHandle(g_web_hook);
            g_web_hook = os::InvalidNativeHandle;
            g_web_hook_fired_ns = 0;
        }

        void WebHookStep() {
            if (g_web_hook == os::InvalidNativeHandle) {
                if (!g_mapped || g_pmdmnt_rc != 0) { return; }
                static bool said = false;
                os::NativeHandle h = os::InvalidNativeHandle;
                const ::Result rc = ::ams::pm::dmnt::HookToCreateProcess(std::addressof(h), ncm::ProgramId{SystemWebProgramId}).GetValue();
                if (R_FAILED(rc)) {
                    if (!said) { LogLine("donor: no systemWeb launch hook (rc=0x%x, another tool holds pm's hook) - the pool watch only", rc); said = true; }
                    return;
                }
                g_web_hook = h;
                g_web_hook_fired_ns = 0;
                return;
            }
            const u64 now = armTicksToNs(armGetSystemTick());
            if (g_web_hook_fired_ns == 0) {
                if (R_SUCCEEDED(::svcWaitSynchronizationSingle(g_web_hook, 0))) {
                    static_cast<void>(::svcResetSignal(g_web_hook));
                    g_web_hook_fired_ns = now;
                    if (g_mapped) {
                        LogDefer(true);
                        LogLine("donor: systemWeb (eShop, news, a login page) is starting - held until the stream gives the memory back");
                        g_hold_until_ns.store(now + 4'000'000'000ull, std::memory_order_relaxed);
                        g_trigger_ns = now;
                        g_applet_needs_memory.store(true, std::memory_order_relaxed);
                    }
                } else if (!g_mapped) {
                    static_cast<void>(::pmdmntClearHook(1));
                    if (R_SUCCEEDED(::svcWaitSynchronizationSingle(g_web_hook, 0))) {
                        static_cast<void>(::svcResetSignal(g_web_hook));
                        StartHookedWeb("not held");
                    }
                    CloseWebHook();
                }
                return;
            }
            if (!g_mapped || now - g_web_hook_fired_ns >= WebHookMaxHoldNs) {
                if (!g_mapped) { os::SleepThread(TimeSpan::FromMilliSeconds(15)); }
                StartHookedWeb(g_mapped ? "STILL HELD" : "given back");
                CloseWebHook();
            }
        }

        void AppletWatchThread(void *) {
            u64 checked_mb = 0, base_mb = 0;
            bool was_mapped = false;
            u64 calm_prev_mb = 0;
            for (u32 tick = 0;; ++tick) {
                os::SleepThread(TimeSpan::FromMilliSeconds(20));
                WebHookStep();
                if ((tick % 5) == 0) {
                    u64 c = 0, l = 0;
                    if (R_SUCCEEDED(::ams::pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(c), std::addressof(l), ::ams::pm::ResourceLimitGroup_Applet, svc::LimitableResource_PhysicalMemoryMax))) {
                        const u64 m = c >> 20, now = armTicksToNs(armGetSystemTick());
                        const u64 d = m > calm_prev_mb ? m - calm_prev_mb : calm_prev_mb - m;
                        if (calm_prev_mb != 0 && d >= 16 && now - g_donor_change_ns.load(std::memory_order_relaxed) >= 1'500'000'000ull) {
                            g_pool_moved_ns.store(now, std::memory_order_relaxed);
                        }
                        calm_prev_mb = m;
                    }
                }
                if (g_applet_needs_memory.load(std::memory_order_relaxed)) {
                    if (armTicksToNs(armGetSystemTick()) - g_trigger_ns > 3'000'000'000ull) { LogDefer(false); }
                    continue;
                }
                if (!g_mapped) { was_mapped = false; g_beside_max_pid = 0; continue; }
                u64 cur = 0, lim = 0;
                if (R_FAILED(::ams::pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(cur), std::addressof(lim), ::ams::pm::ResourceLimitGroup_Applet, svc::LimitableResource_PhysicalMemoryMax))) { continue; }
                const u64 mb = cur >> 20, donor_mb = g_size >> 20;
                if (!was_mapped) { was_mapped = true; checked_mb = base_mb = mb; }
                if (mb < checked_mb) { checked_mb = mb; }
                if (mb < base_mb) { base_mb = mb; }
                const u64 free_mb = (lim >> 20) - mb;
                const Beside &beside = free_mb < TightMb ? BesideNow(free_mb) : BesideOther;
                const bool tight = free_mb < beside.stop_mb;
                if (beside.what == nullptr) { g_beside_max_pid = 0; }
                u64 new_pid = 0;
                const bool spawned = beside.what != nullptr && NewProcessBeside(g_pid, std::addressof(new_pid));
                bool web = false;
                if (mb >= checked_mb + 32 || mb >= base_mb + 24 || tight) {
                    checked_mb = mb;
                    web = (free_mb < WebRoomMb || mb >= base_mb + 24) && WebAppletRunning();
                }
                const bool trigger = web || tight || spawned;
                if (trigger) {
                    if (tight && !web && !spawned && SwitchURunning()) {
                        static u64 s_swu_headroom_ns = 0;
                        const u64 now_hr = armTicksToNs(armGetSystemTick());
                        if (s_swu_headroom_ns == 0 || now_hr - s_swu_headroom_ns >= UINT64_C(5000000000)) {
                            s_swu_headroom_ns = now_hr;
                            const u32 ask = donor_mb > 0 ? static_cast<u32>(donor_mb + 8) : 42;
                            LogLine("donor: Applet pool tight beside SwitchU (%llu MB free) - asking NeedHeadroom %u MB before teardown",
                                    static_cast<unsigned long long>(free_mb), ask);
                            HeadroomAskAndWait(ask, 1500);
                            HeadroomClear();
                            const u64 free_after = AppletFreeMb();
                            if (free_after >= beside.stop_mb) {
                                LogLine("donor: SwitchU reclaimed - pool has %llu MB free, keeping the stream",
                                        static_cast<unsigned long long>(free_after));
                                continue;
                            }
                            LogLine("donor: still tight after NeedHeadroom (%llu MB free) - stopping the stream",
                                    static_cast<unsigned long long>(free_after));
                        }
                    }
                    LogDefer(true);
                    g_trigger_ns = armTicksToNs(armGetSystemTick());
                    g_applet_needs_memory.store(true, std::memory_order_relaxed);
                    LogLine("donor: an applet is starting (Applet group %llu MB, %llu of them ours%s%s) - stopping the stream to give the memory back",
                            static_cast<unsigned long long>(mb), static_cast<unsigned long long>(donor_mb),
                            spawned ? "; a new process beside " : "", spawned ? beside.what : "");
                    if (spawned) { LogLine("donor: the new process: pid %llu, program %016llx", static_cast<unsigned long long>(new_pid), static_cast<unsigned long long>(ProgramIdOf(new_pid))); }
                }
            }
        }
    }

    bool AppletHoldActive() {
        const u64 now = armTicksToNs(armGetSystemTick());
        const u64 until = g_hold_until_ns.load(std::memory_order_relaxed);
        if (now >= until) { return false; }
        static u64 s_until = 0, s_start = 0, s_prev_mb = 0, s_stable_ns = 0, s_app = 0;
        if (until != s_until) { s_until = until; s_start = now; s_prev_mb = 0; s_stable_ns = 0; s_app = g_app_pid.load(std::memory_order_relaxed); }
        constexpr u64 MinHoldNs = 2'000'000'000ull, SettledNs = 750'000'000ull;
        const u64 start = s_start;
        if (SwitchUInstalled() && now - start >= 300'000'000ull && s_app != 0 && g_app_pid.load(std::memory_order_relaxed) == s_app &&
            !Running(0x010000000000100Dull)) {
            constexpr u64 IdlePoolMb = 100;
            const u64 lp = armTicksToNs(g_app_present_tick.load(std::memory_order_relaxed));
            u64 cur = 0, lim = 0;
            if (lp > start && now - lp < 200'000'000ull &&
                R_SUCCEEDED(::ams::pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(cur), std::addressof(lim), ::ams::pm::ResourceLimitGroup_Applet, svc::LimitableResource_PhysicalMemoryMax)) &&
                (cur >> 20) < IdlePoolMb) {
                g_hold_until_ns.store(0, std::memory_order_relaxed);
                LogLine("donor: the game is back (SwitchU's menu closed, the Applet pool at %llu MB) - no more waiting after %llu ms",
                        static_cast<unsigned long long>(cur >> 20), static_cast<unsigned long long>((now - start) / 1000000));
                return false;
            }
        }
        if (now - start < MinHoldNs) { return true; }
        {
            os::ProcessId app{};
            if (g_pmdmnt_rc != 0 || R_FAILED(::ams::pm::dmnt::GetApplicationProcessId(std::addressof(app)))) { return true; }
        }
        {
            const u64 moved = g_pool_moved_ns.load(std::memory_order_relaxed);
            if (moved != 0 && now - moved < PoolCalmNs) { s_stable_ns = 0; return true; }
        }
        u64 cur = 0, lim = 0;
        if (R_FAILED(::ams::pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(cur), std::addressof(lim), ::ams::pm::ResourceLimitGroup_Applet, svc::LimitableResource_PhysicalMemoryMax))) { return true; }
        const u64 mb = cur >> 20;
        const u64 moved = g_pool_moved_ns.load(std::memory_order_relaxed);
        const bool calm_after_move = moved != 0 && moved + 1'000'000'000ull >= start;
        if (s_stable_ns == 0 || mb > s_prev_mb + 4) { s_stable_ns = now; }
        s_prev_mb = mb;
        if (!calm_after_move && now - s_stable_ns < SettledNs) { return true; }
        g_hold_until_ns.store(0, std::memory_order_relaxed);
        LogLine("donor: the Applet pool settled at %llu MB after %llu ms - no more waiting", static_cast<unsigned long long>(mb),
                static_cast<unsigned long long>((now - start) / 1000000));
        return false;
    }

    void PrepareForApplet(u32 applet_id) {
        const u64 t0 = armTicksToNs(armGetSystemTick());
        g_hold_until_ns.store(t0 + 4'000'000'000ull, std::memory_order_relaxed);
        if (!g_mapped) { return; }
        LogDefer(true);
        LogLine("am: the game opens library applet %#x - stopping the stream to give the memory back first", applet_id);
        g_trigger_ns = t0;
        g_applet_needs_memory.store(true, std::memory_order_relaxed);
        while (g_mapped && armTicksToNs(armGetSystemTick()) - t0 < 1'500'000'000ull) { os::SleepThread(TimeSpan::FromMilliSeconds(5)); }
        os::SleepThread(TimeSpan::FromMilliSeconds(15));
        LogLine("am: memory %s after %llu ms - letting the applet start", g_mapped ? "STILL HELD" : "given back",
                static_cast<unsigned long long>((armTicksToNs(armGetSystemTick()) - t0) / 1000000));
    }

    void PrepareForHomeMenu() {
        const u64 t0 = armTicksToNs(armGetSystemTick());
        g_hold_until_ns.store(t0 + 5'000'000'000ull, std::memory_order_relaxed);
        g_trigger_ns = t0;
        g_applet_needs_memory.store(true, std::memory_order_relaxed);
        LogLine("home: detach-on-home - release capture memory for home menu (no engine re-setup for 5 s)");
    }

    void StartAppletWatch() {
        const s32 prio = ThreadBasePriority();
        if (R_FAILED(os::CreateThread(std::addressof(g_watch_thread), AppletWatchThread, nullptr, g_watch_stack, sizeof(g_watch_stack), prio))) {
            LogLine("donor: applet watch thread not created - opening a manual while streaming will crash it");
            return;
        }
        os::SetThreadNamePointer(std::addressof(g_watch_thread), "applet-mitm.AppletWatch");
        os::StartThread(std::addressof(g_watch_thread));
        NoteThread(std::addressof(g_watch_thread));
    }

    void DonorAccept(u64 size, os::NativeHandle tmem) {
        if (g_state.load() != 0 || g_handle != os::InvalidNativeHandle) { os::CloseNativeHandle(tmem); return; }
        g_handle = tmem;
        g_size = size;
        g_state.store(1);
    }

    void DonorAcceptNv(u64 size, os::NativeHandle tmem) {
        if (g_nv_handle != os::InvalidNativeHandle) { os::CloseNativeHandle(g_nv_handle); }
        g_nv_handle = tmem;
        g_nv_size = size;
    }

    bool DonorTakeNvTmem(os::NativeHandle *out, u64 *out_size) {
        std::scoped_lock lk(g_lock);
        if (g_nv_handle == os::InvalidNativeHandle) { return false; }
        *out = g_nv_handle;
        *out_size = g_nv_size;
        g_nv_handle = os::InvalidNativeHandle;
        return true;
    }

    void DonorFailed(u32 rc) {
        g_fail_rc = rc;
        g_state.store(2);
    }

    bool DonorActive() { return g_mapped; }

    bool WebAppletOpen() { return WebAppletRunning(); }

    bool SwitchUOpen() { return SwitchURunning(); }
    bool SwitchUMenuClosed() { return SwitchUInstalled() && !Running(0x010000000000100Dull); }

    bool AppletBusy(u64 *out_mb) {
        u64 cur = 0, lim = 0;
        if (R_FAILED(::ams::pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(cur), std::addressof(lim), ::ams::pm::ResourceLimitGroup_Applet, svc::LimitableResource_PhysicalMemoryMax))) { return false; }
        const u64 mb = cur >> 20;
        if (out_mb != nullptr) { *out_mb = mb; }
        const u64 free_mb = (lim >> 20) - mb;
        if (free_mb < MinDonorMb + BesideNow(free_mb).take_mb) { return true; }
        if (!WebAppletRunning()) { return false; }
        const u64 moved = g_pool_moved_ns.load(std::memory_order_relaxed);
        if (moved != 0 && armTicksToNs(armGetSystemTick()) - moved < PoolCalmNs) { return true; }
        return free_mb < MinDonorMb + TightMb + WebRoomMb;
    }

    bool KeyboardHoldsStream() { return !WebAppletRunning() && Running(0x0100000000001008ull); }

    bool DonorCanGrow() {
        if (!g_mapped || g_cap_mb.load(std::memory_order_relaxed) >= FullDonorMb) { return false; }
        static u64 s_last_ns = 0;
        const u64 now = armTicksToNs(armGetSystemTick());
        if (s_last_ns != 0 && now - s_last_ns < UINT64_C(60000000000)) { return false; }
        const u64 free_mb = AppletFreeMb(), ours_mb = g_size >> 20;
        if (free_mb + ours_mb < TightMb + FullDonorMb || WebAppletRunning()) { return false; }
        s_last_ns = now;
        LogLine("donor: the Applet pool has room again (%llu MB free beside our %llu) - the full %u MB for a 1080p picture",
                static_cast<unsigned long long>(free_mb), static_cast<unsigned long long>(ours_mb), FullDonorMb);
        return true;
    }

    bool HomeDetachShouldFire(u64 stall_age_ms, u32 probe_ms, u32 fallback_ms, const char **out_reason) {
        if (probe_ms < 50) { probe_ms = 50; }
        if (fallback_ms < probe_ms) { fallback_ms = probe_ms; }

        if (HomeButtonRecent(3000) && stall_age_ms >= 50) {
            if (out_reason != nullptr) { *out_reason = "HOME button"; }
            return true;
        }

        if (stall_age_ms < probe_ms) { return false; }

        if (g_applet_needs_memory.load(std::memory_order_relaxed)) {
            if (out_reason != nullptr) { *out_reason = "applet needs memory"; }
            return true;
        }
        static u64 s_asked_ns = 0;
        static bool s_hold = false, s_album_now = false, s_album_was = false, s_album_inited = false, s_swu = false, s_swu_self = false;
        const u64 now = armTicksToNs(armGetSystemTick());
        if (now - s_asked_ns >= UINT64_C(100000000)) {
            s_asked_ns = now;
            s_hold = AppletHoldActive();
            os::ProcessId album_pid{};
            const bool album = Running(0x010000000000100Dull, std::addressof(album_pid));
            s_swu = album && SwitchURunning();
            s_swu_self = album && SwitchUInstalled() && album_pid.value == g_live_pid.load(std::memory_order_relaxed);
            if (!s_album_inited) {
                s_album_was = album;
                s_album_now = album;
                s_album_inited = true;
            } else {
                s_album_was = s_album_now;
                s_album_now = album;
            }
        }
        if (s_hold) {
            if (out_reason != nullptr) { *out_reason = "applet hold active"; }
            return true;
        }
        if (!s_swu_self && (s_swu ? (s_album_now && !s_album_was) : s_album_now)) {
            if (out_reason != nullptr) { *out_reason = "Album/SwitchU 100D up"; }
            return true;
        }
        if (stall_age_ms >= fallback_ms) {
            if (out_reason != nullptr) { *out_reason = "present stall fallback"; }
            return true;
        }
        return false;
    }

    bool DonorObtain(uintptr_t *out_addr, size_t *out_size) {
        g_donor_change_ns.store(armTicksToNs(armGetSystemTick()), std::memory_order_relaxed);
        u32 ask_cap = FullDonorMb;
        {
            const u64 free_mb = AppletFreeMb();
            const Beside &beside = BesideNow(free_mb);
            const u64 room = free_mb > beside.take_mb ? free_mb - beside.take_mb : 0;
            ask_cap = room >= FullDonorMb ? FullDonorMb : (room >= MinDonorMb ? static_cast<u32>(room) : MinDonorMb);
        }
        bool need_headroom = false;
        {
            std::scoped_lock peek(g_lock);
            need_headroom = !g_mapped;
        }
        if (need_headroom) {
            if (SwitchUOpen()) {
                constexpr u32 kTotalMs = 15000;
                constexpr u32 kRoundMs = 2500;
                u32 elapsed = 0;
                bool room = false;
                while (elapsed < kTotalMs) {
                    HeadroomAskAndWait(ask_cap, kRoundMs);
                    u64 used_mb = 0;
                    if (!AppletBusy(std::addressof(used_mb))) { room = true; break; }
                    HeadroomClear();
                    LogLine("donor: SwitchU still holds the Applet pool (%llu MB used, need ~%u MB free) - waiting, not launching",
                            static_cast<unsigned long long>(used_mb), ask_cap);
                    os::SleepThread(TimeSpan::FromMilliSeconds(500));
                    elapsed += kRoundMs + 500;
                }
                if (!room) {
                    LogLine("donor: no Applet-pool headroom after %u ms beside SwitchU - deferring donor (engines will retry)",
                            kTotalMs);
                    HeadroomClear();
                    return false;
                }
                HeadroomAskAndWait(ask_cap, 500);
            }
        }
        std::scoped_lock lk(g_lock);
        if (!g_mapped) {
            g_state.store(0);
            if (SwitchUOpen()) {
                u64 used_mb = 0;
                if (AppletBusy(std::addressof(used_mb))) {
                    LogLine("donor: Applet pool filled again beside SwitchU (%llu MB) - deferring launch",
                            static_cast<unsigned long long>(used_mb));
                    HeadroomClear();
                    return false;
                }
            }
            u32 cap = FullDonorMb;
            {
                const u64 free_mb = AppletFreeMb();
                const Beside &beside = BesideNow(free_mb);
                const u64 room = free_mb > beside.take_mb ? free_mb - beside.take_mb : 0;
                cap = room >= FullDonorMb ? FullDonorMb : (room >= MinDonorMb ? static_cast<u32>(room) : MinDonorMb);
                if (cap < FullDonorMb) {
                    if (beside.what != nullptr) {
                        LogLine("donor: the Applet pool has %llu MB free - taking at most %u MB (%s is open: %llu MB are left beside it, the stream stops under %llu)",
                                static_cast<unsigned long long>(free_mb), cap, beside.what,
                                static_cast<unsigned long long>(beside.take_mb), static_cast<unsigned long long>(beside.stop_mb));
                    } else {
                        LogLine("donor: the Applet pool has %llu MB free - taking at most %u MB", static_cast<unsigned long long>(free_mb), cap);
                    }
                }
            }
            g_cap_mb.store(cap, std::memory_order_relaxed);
            {
                SmUse sm_pm;
                ::Result rc = ::pmshellInitialize();
                if (R_FAILED(rc)) { LogLine("donor: pm:shell rc=0x%x", rc); HeadroomClear(); return false; }
                const NcmProgramLocation loc = { .program_id = DonorProgramId, .storageID = NcmStorageId_None, .pad = {} };
                u64 pid = 0;
                rc = ::pmshellLaunchProgram(0, std::addressof(loc), std::addressof(pid));
                LogLine("donor: launch rc=0x%x pid %llu", rc, static_cast<unsigned long long>(pid));
                if (R_FAILED(rc)) { ::pmshellExit(); HeadroomClear(); return false; }
                g_pmshell_open = true;
                g_pid = pid;
            }
            for (int i = 0; i < 500 && g_state.load() == 0; ++i) { os::SleepThread(TimeSpan::FromMilliSeconds(10)); }
            if (g_state.load() != 1) {
                LogLine("donor: no memory (%s, rc=0x%x)", g_state.load() == 2 ? "the Applet pool refused every size" : "no answer in 5 s", g_fail_rc);
                TerminateDonor();
                HeadroomClear();
                return false;
            }
            os::AttachTransferMemory(std::addressof(g_tmem), g_size, g_handle, true);
            g_handle = os::InvalidNativeHandle;
            if (const Result rc = os::MapTransferMemory(std::addressof(g_addr), std::addressof(g_tmem), os::MemoryPermission_None); R_FAILED(rc)) {
                LogLine("donor: map %llu MB rc=0x%x", static_cast<unsigned long long>(g_size >> 20), rc.GetValue());
                os::DestroyTransferMemory(std::addressof(g_tmem));
                TerminateDonor();
                HeadroomClear();
                return false;
            }
            g_mapped = true;
            g_donor_change_ns.store(armTicksToNs(armGetSystemTick()), std::memory_order_relaxed);
            LogLine("donor: %llu MB from the Applet pool mapped at %p", static_cast<unsigned long long>(g_size >> 20), g_addr);
            HeadroomClear();
        }
        *out_addr = reinterpret_cast<uintptr_t>(g_addr);
        *out_size = g_size;
        return true;
    }

    void DonorRelease() {
        std::scoped_lock lk(g_lock);
        if (g_mapped) {
            const Result arc = svc::SetMemoryAttribute(reinterpret_cast<uintptr_t>(g_addr), g_size, svc::MemoryAttribute_Uncached, 0);
            if (R_FAILED(arc)) { LogLine("donor: clearing Uncached rc=0x%x", arc.GetValue()); }
            Result rc = ResultSuccess();
            int tries = 0;
            for (; tries < 100; ++tries) {
                rc = svc::UnmapTransferMemory(g_tmem.handle, reinterpret_cast<uintptr_t>(g_addr), g_size);
                if (R_SUCCEEDED(rc)) { break; }
                os::SleepThread(TimeSpan::FromMilliSeconds(50));
            }
            if (R_FAILED(rc)) {
                svc::MemoryInfo mi = {};
                svc::PageInfo pi = {};
                static_cast<void>(svc::QueryMemory(std::addressof(mi), std::addressof(pi), reinterpret_cast<uintptr_t>(g_addr)));
                LogLine("donor: unmap refused for 5 s rc=0x%x (state %u attr %#x device maps %u ipc %u) - keeping the memory for the next stream",
                        rc.GetValue(), static_cast<unsigned>(mi.state), static_cast<unsigned>(mi.attribute),
                        static_cast<unsigned>(mi.device_count), static_cast<unsigned>(mi.ipc_count));
                LogDefer(false);
                return;
            }
            g_tmem.state = os::TransferMemoryType::State_Created;
            g_tmem.address = nullptr;
            g_tmem.allocated = false;
            os::DestroyTransferMemory(std::addressof(g_tmem));
            g_mapped = false;
            g_addr = nullptr;
            LogLine("donor: memory unmapped%s", tries > 0 ? " (after waiting for nvservices)" : "");
            if (tries > 0) { LogLine("donor: waited %d ms", tries * 50); }
        }
        TerminateDonor();
        g_donor_change_ns.store(armTicksToNs(armGetSystemTick()), std::memory_order_relaxed);
        LogDefer(false);
    }

}
