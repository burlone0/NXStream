#include "applet_mitm_service.hpp"
#include <cstdio>
#include <cstring>
#include "applet_mitm_sm.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_gbuf.hpp"
#include "applet_mitm_nv.hpp"
#include "applet_mitm_control.hpp"
#include "applet_mitm_uibinder.hpp"
#include <atomic>

namespace ams::mitm::applet {

    namespace {

        constinit std::atomic<u32> g_txn_total{0};
        constinit std::atomic<u32> g_txn_per_code[16] = {};
        constinit std::atomic<bool> g_blit_attempted{false};

        s32 ParseQueueBufferSlot(const u8 *p, size_t sz) {
            if (p == nullptr || sz < 24) { return -1; }
            u32 data_off = 0;
            std::memcpy(std::addressof(data_off), p + 4, sizeof(data_off));
            if (static_cast<size_t>(data_off) + 8 > sz) { return -1; }

            u32 len = 0;
            std::memcpy(std::addressof(len), p + data_off + 4, sizeof(len));
            if (len == 0 || len > 128) { return -1; }

            size_t off = static_cast<size_t>(data_off) + 8 + (static_cast<size_t>(len) + 1) * 2;
            off = (off + 3) & ~static_cast<size_t>(3);
            if (off + 4 > sz) { return -1; }

            s32 slot = 0;
            std::memcpy(std::addressof(slot), p + off, sizeof(slot));
            return slot;
        }

        bool ParseQueueBufferFence(const u8 *p, size_t sz, u32 *n, u64 out[4], u32 *transform, s32 crop[4]) {
            if (p == nullptr || sz < 24) { return false; }
            u32 data_off = 0, len = 0;
            std::memcpy(std::addressof(data_off), p + 4, sizeof(data_off));
            if (static_cast<size_t>(data_off) + 8 > sz) { return false; }
            std::memcpy(std::addressof(len), p + data_off + 4, sizeof(len));
            if (len == 0 || len > 128) { return false; }
            size_t off = static_cast<size_t>(data_off) + 8 + (static_cast<size_t>(len) + 1) * 2;
            off = (off + 3) & ~static_cast<size_t>(3);
            off += 4;
            if (off + 8 + 84 > sz) { return false; }
            u32 flen = 0;
            std::memcpy(std::addressof(flen), p + off, sizeof(flen));
            if (flen < 84 || flen > 0x100) { return false; }
            const u8 *in = p + off + 8;
            std::memcpy(transform, in + 32, 4);
            std::memcpy(crop, in + 12, 16);
            u32 cnt = 0;
            std::memcpy(std::addressof(cnt), in + 48, sizeof(cnt));
            if (cnt > 4) { return false; }
            for (u32 k = 0; k < 4; ++k) {
                u32 id = 0, v = 0;
                std::memcpy(std::addressof(id), in + 52 + 8 * k, 4);
                std::memcpy(std::addressof(v), in + 56 + 8 * k, 4);
                out[k] = (static_cast<u64>(id) << 32) | v;
            }
            *n = cnt;
            return true;
        }

        const char *TxnName(u32 code) {
            switch (code) {
                case 1:  return "requestBuffer";
                case 2:  return "setBufferCount";
                case 3:  return "dequeueBuffer";
                case 4:  return "detachBuffer";
                case 5:  return "detachNextBuffer";
                case 6:  return "attachBuffer";
                case 7:  return "queueBuffer";
                case 8:  return "cancelBuffer";
                case 9:  return "query";
                case 10: return "connect";
                case 11: return "disconnect";
                case 12: return "setSidebandStream";
                case 13: return "allocateBuffers";
                case 14: return "setPreallocatedBuffer";
                default: return "?";
            }
        }

    }

    s32 ParseParcelSlot(const u8 *p, size_t sz) { return ParseQueueBufferSlot(p, sz); }
    bool ParseParcelQueueInput(const u8 *p, size_t sz, u32 *n, u64 out[4], u32 *transform, s32 crop[4]) {
        return ParseQueueBufferFence(p, sz, n, out, transform, crop);
    }

    constinit std::atomic<bool> g_ui_session_active{false};
    constinit std::atomic<bool> g_game_presented_in_ui{false};

    static u32 CenteredCrop(const s32 crop[4]) {
        const s32 cw = crop[2] - crop[0], chh = crop[3] - crop[1];
        const s32 sw = static_cast<s32>(g_game_surface.width), sh = static_cast<s32>(g_game_surface.height);
        if (cw <= 0 || chh <= 0 || sw <= 0 || sh <= 0 || (crop[0] == 0 && crop[1] == 0)) { return 0; }
        const s32 dx = 2 * crop[0] + cw - sw, dy = 2 * crop[1] + chh - sh;
        if (dx < -2 || dx > 2 || dy < -2 || dy > 2) { return 0; }
        return static_cast<u32>(cw) << 16 | static_cast<u32>(chh);
    }

    static u32 OffsetCrop(const s32 crop[4], u32 *xy) {
        *xy = 0;
        const s32 cw = crop[2] - crop[0], chh = crop[3] - crop[1];
        const s32 sw = static_cast<s32>(g_game_surface.width), sh = static_cast<s32>(g_game_surface.height);
        if (cw <= 0 || chh <= 0 || sw <= 0 || sh <= 0 || crop[0] < 0 || crop[1] < 0) { return 0; }
        if ((crop[0] == 0 && crop[1] == 0) || CenteredCrop(crop) != 0 || crop[2] > sw || crop[3] > sh) { return 0; }
        *xy = static_cast<u32>(crop[0]) << 16 | static_cast<u32>(crop[1]);
        return static_cast<u32>(cw) << 16 | static_cast<u32>(chh);
    }

    void RecordPresent(s32 qs, bool have_fence, u32 fn, const u64 fv[4], u32 xf, const s32 crop[4]) {
        static constinit os::SdkMutex s_record_lock;
        std::scoped_lock lk(s_record_lock);
        g_queue_transform.store(xf, std::memory_order_relaxed);
        {
            const s32 cw = crop[2] - crop[0], chh = crop[3] - crop[1];
            g_queue_crop_wh.store((cw > 0 && chh > 0) ? (static_cast<u32>(cw) << 16 | static_cast<u32>(chh)) : 0, std::memory_order_relaxed);
            g_queue_crop_origin0.store(crop[0] == 0 && crop[1] == 0, std::memory_order_relaxed);
            g_queue_crop_c.store(CenteredCrop(crop), std::memory_order_relaxed);
            {
                u32 xy = 0;
                const u32 owh = OffsetCrop(crop, std::addressof(xy));
                g_queue_crop_o.store(xy, std::memory_order_relaxed);
                g_queue_crop_ow.store(owh, std::memory_order_relaxed);
            }
            if (cw >= 1920 && chh >= 1080) { g_crop_full_pid.store(g_app_pid.load(std::memory_order_relaxed), std::memory_order_relaxed); }
        }
        g_queue_seq.fetch_add(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        if (qs >= 0 && qs < 8) { g_queue_slot.store(qs, std::memory_order_relaxed); }
        if (have_fence) {
            for (u32 k = 0; k < 4; ++k) { g_queue_fence[k].store(fv[k], std::memory_order_relaxed); }
            g_queue_fence_n.store(fn, std::memory_order_relaxed);
        } else {
            g_queue_fence_n.store(0, std::memory_order_relaxed);
        }
        const u64 qtick = armGetSystemTick();
        g_queue_tick.store(qtick, std::memory_order_relaxed);
        {
            const u32 c = g_queue_count.load(std::memory_order_relaxed) + 1;
            PresentRec &r = g_present_ring[c % PresentRingSize];
            r.seq.fetch_add(1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            r.count.store(c, std::memory_order_relaxed);
            r.slot.store((qs >= 0 && qs < 8) ? qs : -1, std::memory_order_relaxed);
            r.fence_n.store(have_fence ? fn : 0, std::memory_order_relaxed);
            for (u32 k = 0; k < 4; ++k) { r.fence[k].store(have_fence ? fv[k] : 0, std::memory_order_relaxed); }
            r.tick.store(qtick, std::memory_order_relaxed);
            r.transform.store(xf, std::memory_order_relaxed);
            {
                const s32 cw = crop[2] - crop[0], chh = crop[3] - crop[1];
                r.crop.store((cw > 0 && chh > 0 && crop[0] == 0 && crop[1] == 0) ? (static_cast<u32>(cw) << 16 | static_cast<u32>(chh)) : 0, std::memory_order_relaxed);
                r.crop_c.store(CenteredCrop(crop), std::memory_order_relaxed);
                u32 xy = 0;
                const u32 owh = OffsetCrop(crop, std::addressof(xy));
                r.crop_o.store(xy, std::memory_order_relaxed);
                r.crop_ow.store(owh, std::memory_order_relaxed);
            }
            r.seq.fetch_add(1, std::memory_order_release);
        }
        g_queue_count.fetch_add(1, std::memory_order_release);
        g_queue_seq.fetch_add(1, std::memory_order_release);
    }

    Result BinderMitm::TransactParcelAuto(s32 session_id, u32 code, u32 flags, const sf::InAutoSelectBuffer &parcel_in, const sf::OutAutoSelectBuffer &parcel_out) {
        R_RETURN(this->Transact(session_id, code, flags, parcel_in.GetPointer(), parcel_in.GetSize(), parcel_out.GetSize()));
    }

    Result BinderMitm::TransactParcel(s32 session_id, u32 code, u32 flags, const sf::InBuffer &parcel_in, const sf::OutBuffer &parcel_out) {
        static bool said = false;
        if (!said) { said = true; LogLine("binder: program %016llx uses TransactParcel (command 0, before firmware 3.0.0)", static_cast<unsigned long long>(m_client_info.program_id.value)); }
        R_RETURN(this->Transact(session_id, code, flags, parcel_in.GetPointer(), parcel_in.GetSize(), parcel_out.GetSize()));
    }

    Result BinderMitm::Transact(s32 session_id, u32 code, u32 flags, const void *in, size_t in_size, size_t out_size) {
        if (m_client_info.program_id.value == TeslaProgramId) {
            OvlNoteTeslaTransaction(code, in, in_size);
            R_RETURN(sm::mitm::ResultShouldForwardToSession());
        }
        if (IsLibraryApplet(m_client_info.program_id.value)) {
            UiNoteTransaction(m_client_info.program_id.value, session_id, code, in, in_size);
            R_RETURN(sm::mitm::ResultShouldForwardToSession());
        }
        GameBinderTransaction(m_client_info, session_id, code, flags, in, in_size, out_size);
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }

    void GameBinderTransaction(const sm::MitmProcessInfo &ci, s32 session_id, u32 code, u32 flags, const void *in_ptr, size_t in_size, size_t out_size) {
        struct InView { const void *p; size_t n; const void *GetPointer() const { return p; } size_t GetSize() const { return n; } };
        const InView parcel_in{ in_ptr, in_size };
        const struct { size_t n; size_t GetSize() const { return n; } } parcel_out{ out_size };
        const bool album = IsAlbumProgram(ci.program_id.value);
        const u32 total = g_txn_total.fetch_add(1) + 1;
        const u32 per   = (code < 16) ? (g_txn_per_code[code].fetch_add(1) + 1) : 0;
        g_stats.txns.store(total, std::memory_order_relaxed);

        if (per <= 3 || (total % 600) == 0) {
            LogLine("*** binder txn #%u  program=%016llx  session=%d  code=%u(%s) x%u  flags=0x%x  in=%zu out=%zu",
                    total,
                    static_cast<unsigned long long>(ci.program_id.value),
                    session_id, code, TxnName(code), per, flags,
                    parcel_in.GetSize(), parcel_out.GetSize());
        }
        if (total == 1) { LogMark("binder:first_txn"); }

        {
            const u64 ap = g_app_pid.load(std::memory_order_relaxed);
            if (ap != 0 && ap != ci.process_id.value && !album) { return; }
        }

        if (code == 14) {
            LogMark("binder:parse_preallocated");
            if (const auto *gb = FindGraphicBuffer(parcel_in.GetPointer(), parcel_in.GetSize()); gb != nullptr) {
                const s32 pslot = ParseQueueBufferSlot(static_cast<const u8 *>(parcel_in.GetPointer()), parcel_in.GetSize());
                char tag[48];
                std::snprintf(tag, sizeof(tag), "setPreallocatedBuffer#%u slot %d", per, pslot);
                LogGraphicBuffer(tag, gb);
                if (album) { CaptureAppletSurface(gb, pslot); } else { CaptureGameSurface(gb, pslot); }
            } else {
                LogLine("    (no NvGraphicBuffer magic found in %zu-byte parcel)", parcel_in.GetSize());
            }
        }

        if (code == 7) {
            static constinit os::SdkMutex s_present_lock;
            std::scoped_lock present_lk(s_present_lock);
            const s32 qs = ParseQueueBufferSlot(static_cast<const u8 *>(parcel_in.GetPointer()), parcel_in.GetSize());
            u32 fn = 0, xf = 0;
            u64 fv[4] = {};
            s32 crop[4] = {};
            const bool have_fence = ParseQueueBufferFence(static_cast<const u8 *>(parcel_in.GetPointer()), parcel_in.GetSize(), std::addressof(fn), fv, std::addressof(xf), crop);
            if (!have_fence) { xf = 0; }
            {
                static constinit u32 s_last_xf = ~0u;
                static constinit s32 s_last_crop[4] = { -1, -1, -1, -1 };
                static constinit u32 s_crop_logs = 0;
                const bool crop_changed = crop[0] != s_last_crop[0] || crop[1] != s_last_crop[1] || crop[2] != s_last_crop[2] || crop[3] != s_last_crop[3];
                if ((xf != s_last_xf || crop_changed) && s_crop_logs < 60) {
                    ++s_crop_logs;
                    for (u32 k = 0; k < 4; ++k) { s_last_crop[k] = crop[k]; }
                    s_last_xf = xf;
                    LogLine("   present transform 0x%x%s%s%s, crop (%d,%d)-(%d,%d)", xf,
                            (xf & 1) ? " flip-H" : "", (xf & 2) ? " flip-V" : "", (xf & 4) ? " rot-90 (not handled)" : "",
                            crop[0], crop[1], crop[2], crop[3]);
                }
            }
            if (album) { g_applet_target_pid.store(ci.process_id.value, std::memory_order_relaxed); }
            else if (ci.process_id.value == g_app_pid.load(std::memory_order_relaxed)) { g_app_present_tick.store(armGetSystemTick(), std::memory_order_relaxed); }
            if (g_ui_session_active.load(std::memory_order_relaxed)) {
                g_game_presented_in_ui.store(true, std::memory_order_relaxed);
            } else {
                g_present_pid.store(ci.process_id.value, std::memory_order_relaxed);
                RecordPresent(qs, have_fence, fn, fv, xf, crop);
            }
        }

        const u64 uptime_s = armTicksToNs(armGetSystemTick()) / UINT64_C(1000000000);
        if (code == 7 && g_vic_armed && uptime_s >= g_probe_delay_s && g_game_surface.armed) {
            bool ex = false;
            if (g_blit_attempted.compare_exchange_strong(ex, true)) {
                const auto *p  = static_cast<const u8 *>(parcel_in.GetPointer());
                const size_t psz = parcel_in.GetSize();
                const s32 qslot  = ParseQueueBufferSlot(p, psz);
                LogMark("binder:vic_blit_requested");
                LogLine("   queueBuffer txn#%u parcel=%zu slot=%d -> handed to VIC worker", total, psz, qslot);
                RequestVicBlit(qslot);
            }
        }
    }

    std::shared_ptr<::Service> MakeForwardService(const ::Service &s) {
        return std::shared_ptr<::Service>(new ::Service(s), [](::Service *p) { serviceClose(p); delete p; });
    }

    Result ViDisplaySvcMitm::GetRelayService(sf::Out<sf::SharedPointer<IBinderMitm>> out) {
        g_stats.relay.fetch_add(1);
        LogMark("GetRelayService:enter");

        ::Service binder_svc = {};
        const Result rc = serviceDispatch(m_forward_service.get(), 100,
            .out_num_objects = 1,
            .out_objects     = std::addressof(binder_svc),
        );
        if (R_FAILED(rc)) {
            LogMark("GetRelayService:forward_FAILED");
            LogLine("   rc=0x%x", rc.GetValue());
            R_RETURN(rc);
        }

        auto shared_srv = MakeForwardService(binder_svc);
        const sf::cmif::DomainObjectId target_object_id{ serviceGetObjectId(std::addressof(binder_svc)) };

        ::ams::sf::impl::g_tier4_pending_mitm_forward = shared_srv;
        out.SetValue(sf::CreateSharedObjectEmplaced<IBinderMitm, BinderMitm>(std::shared_ptr<::Service>(shared_srv), m_client_info), target_object_id);

        LogMark("GetRelayService:wrapped");

        R_SUCCEED();
    }

    Result ViDisplaySvcMitm::GetSystemDisplayService(sf::Out<sf::SharedPointer<IUiSysDisplayMitm>> out) {
        if (!IsLibraryApplet(m_client_info.program_id.value)) { R_RETURN(sm::mitm::ResultShouldForwardToSession()); }
        ::Service sys_svc = {};
        const Result rc = serviceDispatch(m_forward_service.get(), 101,
            .out_num_objects = 1,
            .out_objects     = std::addressof(sys_svc),
        );
        LogLine("   %016llx: GetSystemDisplayService rc=0x%x - wrapped (shared-layer presents logged)",
                static_cast<unsigned long long>(m_client_info.program_id.value), rc.GetValue());
        R_TRY(rc);
        auto shared_srv = MakeForwardService(sys_svc);
        const sf::cmif::DomainObjectId target_object_id{ serviceGetObjectId(std::addressof(sys_svc)) };
        ::ams::sf::impl::g_tier4_pending_mitm_forward = shared_srv;
        out.SetValue(sf::CreateSharedObjectEmplaced<IUiSysDisplayMitm, UiSysDisplayMitm>(std::shared_ptr<::Service>(shared_srv), m_client_info), target_object_id);
        R_SUCCEED();
    }

    Result ViDisplaySvcMitm::OpenDisplay(const DisplayName &display_name, sf::Out<u64> out_display_id) {
        LogMark("OpenDisplay:enter");
        u64 display_id = 0;
        const Result rc = serviceDispatchInOut(m_forward_service.get(), 1010, display_name, display_id);
        if (R_SUCCEEDED(rc)) {
            out_display_id.SetValue(display_id);
            LogLine("   OpenDisplay \"%.32s\" -> display_id=%llu",
                    display_name.data, static_cast<unsigned long long>(display_id));
        } else {
            LogMark("OpenDisplay:forward_FAILED");
            LogLine("   rc=0x%x", rc.GetValue());
        }
        R_RETURN(rc);
    }

    namespace {
        constinit os::SdkMutex g_sess_lock;
        constinit os::NativeHandle g_sess_port = os::InvalidNativeHandle;
        constinit u32 g_sess_made = 0, g_sess_fail = 0;
        constexpr sm::ServiceName SessServiceName = sm::ServiceName::Encode("nxtap:s");
    }

    u32 SessionsMadeViaSm() { return g_sess_made; }

    Result CreateSessionViaSm(os::NativeHandle *out_server, os::NativeHandle *out_client) {
        std::scoped_lock lk(g_sess_lock);
        struct TlsSave {
            u8 buf[0x100];
            TlsSave()  { std::memcpy(buf, armGetTls(), sizeof(buf)); }
            ~TlsSave() { std::memcpy(armGetTls(), buf, sizeof(buf)); }
        } tls_save;
        SmUse sm_use;
        if (g_sess_port == os::InvalidNativeHandle) {
            if (const Result rc = sm::RegisterService(std::addressof(g_sess_port), SessServiceName, 64, false); R_FAILED(rc)) {
                g_sess_port = os::InvalidNativeHandle;
                if (g_sess_fail++ < 4) { LogLine("sessions: could not register nxtap:s rc=0x%x - creating them ourselves", rc.GetValue()); }
                R_RETURN(rc);
            }
        }
        os::NativeHandle client = os::InvalidNativeHandle;
        if (const Result rc = sm::GetServiceHandle(std::addressof(client), SessServiceName); R_FAILED(rc)) {
            if (g_sess_fail++ < 4) { LogLine("sessions: sm connect failed rc=0x%x - creating one ourselves", rc.GetValue()); }
            R_RETURN(rc);
        }
        ::ams::svc::Handle server = ::ams::svc::InvalidHandle;
        if (const Result rc = ::ams::svc::AcceptSession(std::addressof(server), g_sess_port); R_FAILED(rc)) {
            ::ams::svc::CloseHandle(client);
            if (g_sess_fail++ < 4) { LogLine("sessions: accept failed rc=0x%x - creating one ourselves", rc.GetValue()); }
            R_RETURN(rc);
        }
        if (g_sess_made++ == 0) { LogLine("sessions: wrapped objects get their sessions through sm (nxtap:s), not from the Applet group's 6"); }
        *out_server = server;
        *out_client = client;
        R_SUCCEED();
    }

    constinit std::atomic<u64> g_app_pid{0};
    constinit std::atomic<u64> g_crop_full_pid{0};

    namespace {
        alignas(os::ThreadStackAlignment) constinit u8 g_app_watch_stack[8_KB];
        constinit os::ThreadType g_app_watch_thread;

        struct ProcMem { u64 tid; u64 base; u64 last; };
        constinit ProcMem g_procmem[48] = {};
        constinit u32 g_procmem_n = 0;

        const char *ProcName(u64 tid) {
            switch (tid) {
                case 0x0100000000000000: return "fs";
                case 0x0100000000000003: return "pm";
                case 0x0100000000000004: return "sm";
                case 0x0100000000000012: return "bsdsockets";
                case 0x0100000000000013: return "hid";
                case 0x0100000000000019: return "nvservices";
                case 0x010000000000001F: return "ns";
                case 0x0100000000000022: return "capsrv";
                case 0x0100000000000023: return "am";
                case 0x0100000000000024: return "ssl";
                case 0x010000000000002D: return "vi";
                case 0x0100000000000036: return "grc";
                case 0x4200000000004E54: return "SFT";
                case 0x4200000000004E55: return "SFT-donor";
                default: return nullptr;
            }
        }

        void LogProcessMemory(u64 app_pid) {
            static u64 pids[96];
            s32 n = 0;
            if (R_FAILED(::svcGetProcessList(&n, pids, 96))) { return; }
            const bool first = g_procmem_n == 0;
            char line[480];
            int len = 0;
            u64 total = 0;
            for (s32 i = 0; i < n; ++i) {
                if (pids[i] == app_pid) { continue; }
                os::NativeHandle h = os::InvalidNativeHandle;
                ncm::ProgramLocation loc = {};
                cfg::OverrideStatus st = {};
                if (R_FAILED(::ams::pm::dmnt::AtmosphereGetProcessInfo(std::addressof(h), std::addressof(loc), std::addressof(st), os::ProcessId{pids[i]}))) { continue; }
                u64 used = 0;
                const bool ok = R_SUCCEEDED(::svcGetInfo(&used, InfoType_UsedMemorySize, h, 0));
                os::CloseNativeHandle(h);
                const u64 tid = loc.program_id.value;
                if (!ok || (tid >= 0x0100000000001000ull && tid < 0x0100000000002000ull)) { continue; }
                total += used;
                ProcMem *e = nullptr;
                for (u32 k = 0; k < g_procmem_n; ++k) { if (g_procmem[k].tid == tid) { e = std::addressof(g_procmem[k]); break; } }
                if (e == nullptr) {
                    if (g_procmem_n >= 48) { continue; }
                    e = std::addressof(g_procmem[g_procmem_n++]);
                    *e = { tid, used, used };
                    if (!first && len < static_cast<int>(sizeof(line)) - 64) { len += std::snprintf(line + len, sizeof(line) - len, " new %016llx %llu KB;", static_cast<unsigned long long>(tid), static_cast<unsigned long long>(used / 1024)); }
                    continue;
                }
                if (used != e->last && len < static_cast<int>(sizeof(line)) - 64) {
                    const char *nm = ProcName(tid);
                    char idb[20];
                    if (nm == nullptr) { std::snprintf(idb, sizeof(idb), "%llx", static_cast<unsigned long long>(tid)); nm = idb; }
                    len += std::snprintf(line + len, sizeof(line) - len, " %s %llu KB (%+lld);", nm, static_cast<unsigned long long>(used / 1024),
                                         static_cast<long long>((static_cast<s64>(used) - static_cast<s64>(e->base)) / 1024));
                    e->last = used;
                }
            }
            if (first) { LogLine("process memory: %u processes besides the game, %llu KB - changes from here on are logged", g_procmem_n, static_cast<unsigned long long>(total / 1024)); }
            else if (len > 0) { LogLine("process memory (KB, change since the first look):%s", line); }
        }

        void LogAppLeftovers() {
            u64 mem = 0;
            static_cast<void>(svc::GetSystemInfo(std::addressof(mem), svc::SystemInfoType_UsedPhysicalMemorySize, svc::InvalidHandle, svc::PhysicalMemorySystemInfo_Application));
            LogLine("app left: Application memory in use %llu KB (pm starts the next app at 0 or 98304 KB)", static_cast<unsigned long long>(mem / 1024));
            static constexpr struct { svc::LimitableResource r; const char *name; } Res[] = {
                { svc::LimitableResource_PhysicalMemoryMax, "memory" }, { svc::LimitableResource_ThreadCountMax, "threads" },
                { svc::LimitableResource_EventCountMax, "events" }, { svc::LimitableResource_TransferMemoryCountMax, "transfer memories" },
                { svc::LimitableResource_SessionCountMax, "sessions" },
            };
            char line[256];
            int len = 0;
            for (const auto &e : Res) {
                u64 cur = 0, lim = 0;
                const Result rc = ::ams::pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(cur), std::addressof(lim), ::ams::pm::ResourceLimitGroup_Application, e.r);
                if (len < static_cast<int>(sizeof(line)) - 48) {
                    if (R_SUCCEEDED(rc)) { len += std::snprintf(line + len, sizeof(line) - len, " %s %llu;", e.name, static_cast<unsigned long long>(e.r == svc::LimitableResource_PhysicalMemoryMax ? cur / 1024 : cur)); }
                    else { len += std::snprintf(line + len, sizeof(line) - len, " %s rc=0x%x;", e.name, rc.GetValue()); }
                }
            }
            LogLine("app left: Application group still holds (memory in KB):%s", line);
            u64 prev = mem;
            for (u32 i = 0; i < 12; ++i) {
                os::SleepThread(TimeSpan::FromMilliSeconds(250));
                static_cast<void>(svc::GetSystemInfo(std::addressof(mem), svc::SystemInfoType_UsedPhysicalMemorySize, svc::InvalidHandle, svc::PhysicalMemorySystemInfo_Application));
                if (mem != prev) {
                    LogLine("app left: +%u ms Application memory %llu KB", (i + 1) * 250, static_cast<unsigned long long>(mem / 1024));
                    prev = mem;
                }
            }
            LogLine("app left: after 3 s Application memory %llu KB", static_cast<unsigned long long>(mem / 1024));
        }

        constexpr size_t AppNameMax = 128;
        alignas(0x1000) constinit u8 g_ns_buf[sizeof(::NsApplicationControlData)];
        constinit os::SdkMutex g_app_info_lock;
        constinit u64 g_info_tid = 0;
        constinit u64 g_info_launch = 0;
        constinit bool g_info_launch_ok = false;
        constinit bool g_info_fg = false;
        constinit bool g_info_set = false;
        constinit bool g_info_hb = false;
        constinit u64 g_info_hb_launch = 0;
        constinit bool g_info_hb_launch_ok = false;
        constinit char g_info_name[AppNameMax + 1] = {};
        constinit std::atomic<u32> g_app_info_serial{0};
        constinit std::atomic<bool> g_time_ok{false};

        bool WallClock(u64 *out) {
            return g_time_ok.load(std::memory_order_relaxed) && R_SUCCEEDED(::timeGetCurrentTime(TimeType_UserSystemClock, out));
        }

        size_t CleanName(const char *src, char *dst) {
            size_t n = 0;
            for (size_t i = 0; src[i] != '\0'; ++i) {
                const unsigned char c = static_cast<unsigned char>(src[i]);
                if (c < 0x20 || c == 0x7F) { continue; }
                if (n == 0 && (c & 0xC0) == 0x80) { continue; }
                if (n >= AppNameMax) { break; }
                dst[n++] = static_cast<char>(c);
            }
            while (n > 0) {
                size_t k = n;
                while (k > 0 && (static_cast<unsigned char>(dst[k - 1]) & 0xC0) == 0x80) { --k; }
                if (k == 0) { n = 0; break; }
                const unsigned char lead = static_cast<unsigned char>(dst[k - 1]);
                const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
                if (n - (k - 1) < need) { n = k - 1; }
                break;
            }
            dst[n] = '\0';
            return n;
        }

        bool ReadAppName(u64 tid, char *dst) {
            dst[0] = '\0';
            bool ok = false;
            {
                SmUse sm_ns;
                if (R_SUCCEEDED(::nsInitialize())) {
                    u64 actual = 0;
                    if (R_SUCCEEDED(::nsGetApplicationControlData(NsApplicationControlSource_Storage, tid & ~UINT64_C(0xFFF),
                                                                  reinterpret_cast<::NsApplicationControlData *>(g_ns_buf), sizeof(g_ns_buf), &actual)) &&
                        actual >= sizeof(::NacpStruct)) {
                        const ::NacpStruct *nacp = reinterpret_cast<const ::NacpStruct *>(g_ns_buf);
                        const int order[3] = { 0, 1, -1 };
                        for (int o : order) {
                            if (o >= 0) {
                                if (nacp->lang_data.lang[o].name[0] != '\0') { ok = CleanName(nacp->lang_data.lang[o].name, dst) > 0; if (ok) { break; } }
                            } else {
                                for (int i = 2; i < 16 && !ok; ++i) {
                                    if (nacp->lang_data.lang[i].name[0] != '\0') { ok = CleanName(nacp->lang_data.lang[i].name, dst) > 0; }
                                }
                            }
                        }
                    }
                    ::nsExit();
                }
            }
            return ok;
        }

        void NoteAppForeground(bool fg) {
            std::scoped_lock lk(g_app_info_lock);
            if (!g_info_set || g_info_fg == fg) { return; }
            g_info_fg = fg;
            g_app_info_serial.fetch_add(1, std::memory_order_relaxed);
        }

        bool PidIsHbl(u64 pid) {
            os::NativeHandle h = os::InvalidNativeHandle;
            ncm::ProgramLocation loc = {};
            cfg::OverrideStatus st = {};
            if (R_FAILED(::ams::pm::dmnt::AtmosphereGetProcessInfo(std::addressof(h), std::addressof(loc), std::addressof(st), os::ProcessId{pid}))) { return false; }
            os::CloseNativeHandle(h);
            return st.IsHbl();
        }

        void NoteHbl(bool active, const char *source) {
            u64 now = 0;
            const bool clock_ok = active && WallClock(&now);
            std::scoped_lock lk(g_app_info_lock);
            if (g_info_hb == active) { return; }
            g_info_hb = active;
            g_info_hb_launch = now;
            g_info_hb_launch_ok = clock_ok;
            g_app_info_serial.fetch_add(1, std::memory_order_relaxed);
            if (active) { LogLine("info: hbl detected (%s)", source); } else { LogLine("info: hbl ended"); }
        }

        void NoteAppChange(u64 tid, bool fg) {
            char name[AppNameMax + 1] = {};
            bool name_ok = false;
            if (tid != 0) {
                name_ok = ReadAppName(tid, name);
                LogLine("info: app %016llx name %s", static_cast<unsigned long long>(tid), name_ok ? "read" : "unavailable");
            }
            u64 now = 0;
            const bool clock_ok = tid != 0 && WallClock(&now);
            std::scoped_lock lk(g_app_info_lock);
            g_info_tid = tid;
            g_info_launch = now;
            g_info_launch_ok = clock_ok;
            g_info_fg = tid != 0 && fg;
            std::memcpy(g_info_name, name, sizeof(g_info_name));
            g_info_set = true;
            g_app_info_serial.fetch_add(1, std::memory_order_relaxed);
        }

        void AppWatchThread(void *) {
            u64 last = ~UINT64_C(0);
            u64 last_hb_pid = ~UINT64_C(0);
            const bool info_ok = [] { SmUse sm_pm; return R_SUCCEEDED(::pminfoInitialize()); }();
            g_time_ok.store([] { SmUse sm_time; return R_SUCCEEDED(::timeInitialize()); }(), std::memory_order_relaxed);
            LogLine("info: wall clock %s", g_time_ok.load(std::memory_order_relaxed) ? "ready" : "unavailable");
            for (;;) {
                os::ProcessId pid{};
                const u64 now = R_SUCCEEDED(::ams::pm::dmnt::GetApplicationProcessId(std::addressof(pid))) ? pid.value : 0;
                g_app_pid.store(now, std::memory_order_relaxed);
                if (now != last) {
                    u64 tid = 0;
                    if (now != 0 && info_ok && R_FAILED(::pminfoGetProgramId(&tid, now))) { tid = 0; }
                    g_app_tid.store(tid, std::memory_order_relaxed);
                    g_app_excluded.store(tid != 0 && IsExcluded(tid), std::memory_order_relaxed);
                    if (now != 0) { g_app_present_tick.store(armGetSystemTick(), std::memory_order_relaxed); }
                    LogLine("app watch: application pid %llu, program %016llx%s", static_cast<unsigned long long>(now),
                            static_cast<unsigned long long>(tid), g_app_excluded.load() ? " (EXCLUDED)" : "");
                    const bool app_left = now == 0 && last != 0 && last != ~UINT64_C(0);
                    last = now;
                    NoteAppChange(tid, !g_ui_session_active.load(std::memory_order_relaxed));
                    LogProcessMemory(now);
                    if (app_left) { LogAppLeftovers(); }
                }
                u64 hb_pid = now;
                if (now == 0) {
                    os::ProcessId album{};
                    hb_pid = R_SUCCEEDED(::ams::pm::dmnt::GetProcessId(std::addressof(album), ncm::ProgramId{0x010000000000100Dull})) ? album.value : 0;
                }
                if (hb_pid != last_hb_pid) {
                    last_hb_pid = hb_pid;
                    NoteHbl(hb_pid != 0 && PidIsHbl(hb_pid), now != 0 ? "application" : "album");
                }
                NoteAppForeground(now != 0 && !g_ui_session_active.load(std::memory_order_relaxed));
                os::SleepThread(TimeSpan::FromMilliSeconds(100));
            }
        }
    }

    u32 AppInfoSerial() { return g_app_info_serial.load(std::memory_order_relaxed); }

    u32 AppInfoFormat(char *out, size_t cap, size_t *len) {
        u64 now = 0;
        const bool clock_ok = WallClock(&now);
        std::scoped_lock lk(g_app_info_lock);
        *len = 0;
        const u32 serial = g_app_info_serial.load(std::memory_order_relaxed);
        if (cap == 0) { return serial; }
        if (g_info_hb) {
            int h = std::snprintf(out, cap, "app=0000000000000000\napphb=1\n");
            if (g_info_hb_launch_ok && clock_ok && now >= g_info_hb_launch && h > 0 && static_cast<size_t>(h) < cap) {
                h += std::snprintf(out + h, cap - h, "appage=%llu\n", static_cast<unsigned long long>(now - g_info_hb_launch));
            }
            if (h > 0 && static_cast<size_t>(h) < cap) { h += std::snprintf(out + h, cap - h, "appfg=1\n"); }
            if (h > 0 && static_cast<size_t>(h) < cap) { *len = static_cast<size_t>(h); }
            return serial;
        }
        if (!g_info_set) { return serial; }
        int n = std::snprintf(out, cap, "app=%016llx\n", static_cast<unsigned long long>(g_info_tid));
        if (g_info_name[0] != '\0' && n > 0 && static_cast<size_t>(n) < cap) { n += std::snprintf(out + n, cap - n, "appname=%s\n", g_info_name); }
        if (g_info_tid != 0 && g_info_launch_ok && clock_ok && now >= g_info_launch && n > 0 && static_cast<size_t>(n) < cap) {
            n += std::snprintf(out + n, cap - n, "appage=%llu\n", static_cast<unsigned long long>(now - g_info_launch));
        }
        if (n > 0 && static_cast<size_t>(n) < cap) { n += std::snprintf(out + n, cap - n, "appfg=%d\n", g_info_fg ? 1 : 0); }
        if (n > 0 && static_cast<size_t>(n) < cap) { *len = static_cast<size_t>(n); }
        return serial;
    }

    void StartAppWatch() {
        if (g_pmdmnt_rc != 0) { LogLine("app watch: pm:dmnt unavailable - homebrew applications need an application-range program id"); return; }
        SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_app_watch_thread), AppWatchThread, nullptr,
                                        g_app_watch_stack, sizeof(g_app_watch_stack),
                                        ThreadBasePriority()));
        os::SetThreadNamePointer(std::addressof(g_app_watch_thread), "applet-mitm.AppWatch");
        os::StartThread(std::addressof(g_app_watch_thread));
        NoteThread(std::addressof(g_app_watch_thread));
    }

    Result ForwardGetDisplayService(::Service *fwd, u32 cmd, u32 mode, const sm::MitmProcessInfo &ci, sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> &out) {
        g_stats.getdisp.fetch_add(1);
        LogMark("GetDisplayService:enter");
        LogLine("   program=%016llx cmd %u mode=%u%s", static_cast<unsigned long long>(ci.program_id.value), cmd, mode,
                cmd == 2 ? " (vi:m - a homebrew application)" : "");
        if (cmd == 2) { g_vim_program.store(ci.program_id.value, std::memory_order_relaxed); }

        ::Service disp_svc = {};
        const Result rc = serviceDispatchIn(fwd, cmd, mode,
            .out_num_objects = 1,
            .out_objects     = std::addressof(disp_svc),
        );
        if (R_FAILED(rc)) {
            LogLine("   fwd FAILED rc=0x%x", rc.GetValue());
            R_RETURN(rc);
        }
        WrapDisplayService(disp_svc, ci, out);
        LogMark("GetDisplayService:done");
        R_SUCCEED();
    }

    void WrapDisplayService(::Service disp_svc, const sm::MitmProcessInfo &ci, sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> &out) {
        auto shared_srv = MakeForwardService(disp_svc);
        const sf::cmif::DomainObjectId target_object_id{ serviceGetObjectId(std::addressof(disp_svc)) };

        ::ams::sf::impl::g_tier4_pending_mitm_forward = shared_srv;
        out.SetValue(sf::CreateSharedObjectEmplaced<IViDisplaySvcMitm, ViDisplaySvcMitm>(std::shared_ptr<::Service>(shared_srv), ci), target_object_id);
    }

    Result ForwardProxyNameExchange(::Service *fwd, u32 cmd, const u8 *msg, const sm::MitmProcessInfo &ci, sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> &out, u32 plain_cmd) {
        g_stats.getdisp.fetch_add(1);
        LogMark("GetDisplayServiceWithProxyNameExchange:enter");

        const HipcParsedRequest req = hipcParseRequest(const_cast<u8 *>(msg));
        const uintptr_t words = reinterpret_cast<uintptr_t>(req.data.data_words);
        const uintptr_t raw   = (words + 0xF) & ~static_cast<uintptr_t>(0xF);
        const size_t total    = static_cast<size_t>(req.meta.num_data_words) * 4;
        const CmifInHeader *hdr = reinterpret_cast<const CmifInHeader *>(raw);
        size_t args = 0;
        const bool sane = total >= sizeof(CmifInHeader) + (raw - words) && raw + sizeof(CmifInHeader) <= reinterpret_cast<uintptr_t>(msg) + 0x100
                       && hdr->magic == CMIF_IN_HEADER_MAGIC && hdr->command_id == cmd;
        if (sane) {
            args = total - (raw - words) - sizeof(CmifInHeader);
            if (args > 0x40) { args = 0x40; }
        }
        const u8 *a = reinterpret_cast<const u8 *>(raw + sizeof(CmifInHeader));
        LogLine("   program=%016llx cmd %u (ProxyNameExchange): %zu argument byte(s)%s; data words %u, pid %s",
                static_cast<unsigned long long>(ci.program_id.value), cmd, args, sane ? "" : " (UNPARSED)",
                req.meta.num_data_words, req.meta.send_pid ? "sent" : "none");
        if (sane && args != 0) {
            char hex[3 * 0x40 + 1] = {};
            for (size_t i = 0; i < args; ++i) { std::snprintf(hex + 3 * i, 4, "%02x ", a[i]); }
            LogLine("   args: %s", hex);
        }
        if (!sane) {
            LogLine("   not wrapping: forwarding untouched is no longer possible here - using GetDisplayService instead");
        }

        ::Service disp_svc = {};
        ::Result rc = 0;
        if (sane) {
            SfDispatchParams disp = {};
            disp.out_num_objects = 1;
            disp.out_objects     = std::addressof(disp_svc);
            rc = serviceDispatchImpl(fwd, cmd, a, static_cast<u32>(args), nullptr, 0, disp);
        } else {
            const u32 policy = cmd == 3 ? 1 : 0;
            rc = serviceDispatchIn(fwd, plain_cmd, policy,
                                   .out_num_objects = 1, .out_objects = std::addressof(disp_svc));
        }
        if (R_FAILED(rc)) {
            LogLine("   fwd FAILED rc=0x%x", rc);
            R_RETURN(::ams::Result(rc));
        }
        WrapDisplayService(disp_svc, ci, out);
        LogMark("GetDisplayServiceWithProxyNameExchange:done");
        R_SUCCEED();
    }

    Result ViRootMitm::GetDisplayService(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out, u32 mode) {
        R_RETURN(ForwardGetDisplayService(m_forward_service.get(), 0, mode, m_client_info, out));
    }

    void ViRootMitm::Wrap(::Service disp_svc, sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> &out) {
        WrapDisplayService(disp_svc, m_client_info, out);
    }

    Result ViRootMitm::GetDisplayServiceWithProxyNameExchange(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out) {
        alignas(0x10) u8 msg[0x100];
        std::memcpy(msg, armGetTls(), sizeof(msg));
        R_RETURN(ForwardProxyNameExchange(m_forward_service.get(), 1, msg, m_client_info, out, 0));
    }

    Result ViManagerRootMitm::GetDisplayService(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out, u32 mode) {
        R_RETURN(ForwardGetDisplayService(m_forward_service.get(), 2, mode, m_client_info, out));
    }

    Result ViManagerRootMitm::GetDisplayServiceWithProxyNameExchange(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out) {
        alignas(0x10) u8 msg[0x100];
        std::memcpy(msg, armGetTls(), sizeof(msg));
        R_RETURN(ForwardProxyNameExchange(m_forward_service.get(), 3, msg, m_client_info, out, 2));
    }

    Result ViSystemRootMitm::GetDisplayService(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out, u32 mode) {
        R_RETURN(ForwardGetDisplayService(m_forward_service.get(), 1, mode, m_client_info, out));
    }

    Result ViSystemRootMitm::GetDisplayServiceWithProxyNameExchange(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out) {
        alignas(0x10) u8 msg[0x100];
        std::memcpy(msg, armGetTls(), sizeof(msg));
        R_RETURN(ForwardProxyNameExchange(m_forward_service.get(), 3, msg, m_client_info, out, 1));
    }

}
