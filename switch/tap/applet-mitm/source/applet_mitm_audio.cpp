#include "applet_mitm_audio.hpp"
#include "applet_mitm_sm.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_service.hpp"
#include "applet_mitm_nv.hpp"

namespace ams::mitm::applet {

    constinit std::atomic<bool> g_audio_enabled{false};
    constinit std::atomic<bool> g_audio_active{false};
    constinit std::atomic<u32>  g_audio_state{0};

    namespace {

        constexpr u32 GrcdNotInitialized = 0x3E8D4;
        constinit bool g_begin_called = false;

        constexpr size_t RingSize = AudioRingBytes;
        constinit std::atomic<u8 *> g_ring{nullptr};
        constinit std::atomic<u32> g_ring_users{0};
        constinit std::atomic<size_t> g_head{0};
        constinit std::atomic<size_t> g_tail{0};

        constexpr size_t ChunkSize = 0x1000;
        alignas(0x1000) constinit u8 g_chunk[ChunkSize];

        alignas(os::ThreadStackAlignment) constinit u8 g_audio_stack[8_KB];
        constinit os::ThreadType g_audio_thread;

        constinit std::atomic_flag g_push_lock = ATOMIC_FLAG_INIT;
        constinit std::atomic<u64> g_grc_data_ns{0};
        constinit std::atomic<bool> g_rec_running{false};
        constinit std::atomic<u64> g_rec_sound_ns{0};
        constinit std::atomic<u64> g_audout_ns{0};
        constinit std::atomic<bool> g_home_running{false};

        void Push(const u8 *p, size_t n) {
            while (g_push_lock.test_and_set(std::memory_order_acquire)) { os::SleepThread(TimeSpan::FromMicroSeconds(50)); }
            ON_SCOPE_EXIT { g_push_lock.clear(std::memory_order_release); };
            g_ring_users.fetch_add(1);
            ON_SCOPE_EXIT { g_ring_users.fetch_sub(1); };
            u8 *const ring = g_ring.load();
            if (ring == nullptr) { return; }
            size_t head = g_head.load(std::memory_order_relaxed);
            const size_t tail = g_tail.load(std::memory_order_acquire);
            if (n > RingSize - 4) { return; }
            if (head - tail + n > RingSize) { return; }
            for (size_t i = 0; i < n; ) {
                const size_t at = (head + i) % RingSize;
                const size_t k = (RingSize - at) < (n - i) ? (RingSize - at) : (n - i);
                std::memcpy(ring + at, p + i, k);
                i += k;
            }
            g_head.store(head + n, std::memory_order_release);
        }

        ::Result Transfer(::Service *srv, u32 *size) {
            struct { u32 num_frames; u32 data_size; u64 start_timestamp; } out = {};
            const u32 stream = 1;
            const ::Result rc = serviceDispatchInOut(srv, 2, stream, out,
                .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
                .buffers = { { g_chunk, ChunkSize } },
            );
            *size = R_SUCCEEDED(rc) ? out.data_size : 0;
            return rc;
        }

        void AudioThread(void *) {
            ::Service srv = {};
            bool open = false;
            u32 errors = 0;
            for (;;) {
                if (!g_audio_enabled.load(std::memory_order_relaxed)) {
                    os::SleepThread(TimeSpan::FromMilliSeconds(300));
                    continue;
                }
                if (!g_begin_called) {
                    const u32 st = g_live_state.load(std::memory_order_relaxed);
                    if (st != LiveState_WaitGame && st != LiveState_Streaming) {
                        os::SleepThread(TimeSpan::FromMilliSeconds(300));
                        continue;
                    }
                }
                if (!open) {
                    SmUse sm_grc;
                    const ::Result rc = smGetService(&srv, "grc:d");
                    if (R_FAILED(rc)) {
                        g_audio_state.store(2, std::memory_order_relaxed);
                        LogLine("audio: grc:d unavailable rc=0x%x - no game audio", rc);
                        os::SleepThread(TimeSpan::FromSeconds(10));
                        continue;
                    }
                    open = true;
                    LogLine("audio: grc:d open");
                }
                u32 n = 0;
                ::Result rc = Transfer(&srv, &n);
                if (rc == GrcdNotInitialized) {
                    if (g_begin_called) { os::SleepThread(TimeSpan::FromSeconds(1)); continue; }
                    g_begin_called = true;
                    rc = serviceDispatch(&srv, 1);
                    LogLine("audio: grc:d Begin rc=0x%x", rc);
                    if (R_FAILED(rc)) {
                        g_audio_state.store(3, std::memory_order_relaxed);
                        LogLine("audio: the recorder would not start - no game audio this boot");
                        for (;;) { os::SleepThread(TimeSpan::FromSeconds(3600)); }
                    }
                    continue;
                }
                if (R_FAILED(rc)) {
                    if (++errors <= 5 || errors % 100 == 0) { LogLine("audio: transfer rc=0x%x (%u)", rc, errors); }
                    os::SleepThread(TimeSpan::FromMilliSeconds(200));
                    continue;
                }
                g_begin_called = true;
                if (g_audio_state.load(std::memory_order_relaxed) != 1) {
                    g_audio_state.store(1, std::memory_order_relaxed);
                    LogLine("audio: game audio flowing (%u B chunks)", n);
                }
                if (n != 0) { g_grc_data_ns.store(armTicksToNs(armGetSystemTick()), std::memory_order_relaxed); }
                if (g_audio_active.load(std::memory_order_relaxed) && n != 0 && !g_rec_running.load(std::memory_order_relaxed) && !g_home_running.load(std::memory_order_relaxed)) { Push(g_chunk, n & ~3u); }
            }
        }

        constexpr size_t RecCount = 4;
        constexpr size_t RecSize  = 0x1000;
        alignas(0x1000) constinit u8 g_rec_bufs[RecCount][RecSize];

        struct RecBuffer {
            u64 released_ns;
            u64 next_buffer_ptr;
            u64 sample_buffer_ptr;
            u64 sample_buffer_capacity;
            u64 data_size;
            u64 data_offset;
        };
        constinit RecBuffer g_recbuf[RecCount] = {};

        alignas(os::ThreadStackAlignment) constinit u8 g_rec_stack[8_KB];
        constinit os::ThreadType g_rec_thread;

        alignas(0x1000) constinit u8 g_home_bufs[RecCount][RecSize];
        constinit RecBuffer g_home_recbuf[RecCount] = {};
        alignas(os::ThreadStackAlignment) constinit u8 g_home_stack[8_KB];
        constinit os::ThreadType g_home_thread;

        struct RecSet {
            RecBuffer *desc;
            u8 (*pcm)[RecSize];
        };
        constexpr RecSet GameRec = { g_recbuf, g_rec_bufs };
        constexpr RecSet HomeRec = { g_home_recbuf, g_home_bufs };

        ::Result RecAppend(::Service *srv, const RecSet &set, size_t i) {
            set.desc[i] = {};
            set.desc[i].sample_buffer_ptr = reinterpret_cast<u64>(set.pcm[i]);
            set.desc[i].sample_buffer_capacity = RecSize;
            set.desc[i].data_size = RecSize;
            const u64 client = reinterpret_cast<u64>(std::addressof(set.desc[i]));
            return serviceDispatchIn(srv, 8, client,
                .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_In },
                .buffers = { { std::addressof(set.desc[i]), sizeof(RecBuffer) } },
            );
        }

        template<typename F>
        bool RecRelease(::Service *srv, const RecSet &set, F f) {
            u64 ptrs[RecCount] = {};
            struct { u32 count; u32 pad; u64 released; } out = {};
            const ::Result rc = serviceDispatchOut(srv, 9, out,
                .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out },
                .buffers = { { ptrs, sizeof(ptrs) } },
            );
            if (R_FAILED(rc)) { LogLine("audio: recorder buffers rc=0x%x - stopping it", rc); return false; }
            for (u32 k = 0; k < out.count && k < RecCount; ++k) {
                size_t i = RecCount;
                for (size_t j = 0; j < RecCount; ++j) { if (ptrs[k] == reinterpret_cast<u64>(std::addressof(set.desc[j]))) { i = j; } }
                if (i == RecCount) { continue; }
                const u64 d = set.desc[i].data_size;
                f(set.pcm[i], (d != 0 && d <= RecSize) ? static_cast<size_t>(d) : RecSize);
                static_cast<void>(RecAppend(srv, set, i));
            }
            return true;
        }

        u32 Peak(const u8 *pcm, size_t n) {
            const s16 *smp = reinterpret_cast<const s16 *>(pcm);
            u32 peak = 0;
            for (size_t q = 0; q < n / 2; ++q) { const u32 a = static_cast<u32>(smp[q] < 0 ? -static_cast<s32>(smp[q]) : smp[q]); if (a > peak) { peak = a; } }
            return peak;
        }

        bool RecOpen(::Service *srv, ::Event *ev, const RecSet &set, u64 aruid) {
            ::Service mgr = {};
            ::Result rc = 0;
            {
                SmUse sm;
                rc = smGetService(&mgr, "audrec:u");
            }
            if (R_FAILED(rc)) { LogLine("audio: audrec:u unavailable rc=0x%x - no recorder sound", rc); return false; }
            const struct { u32 sample_rate; u32 channel_count; u64 aruid; } in = { 48000, 2, aruid };
            struct { u32 sample_rate; u32 channel_count; u32 sample_format; u32 state; } out = {};
            rc = serviceDispatchInOut(&mgr, 0, in, out,
                .in_num_handles = 1,
                .in_handles = { CUR_PROCESS_HANDLE },
                .out_num_objects = 1,
                .out_objects = srv,
            );
            serviceClose(&mgr);
            if (R_FAILED(rc)) { LogLine("audio: the final output recorder (aruid %llu) did not open rc=0x%x", static_cast<unsigned long long>(aruid), rc); return false; }
            LogLine("audio: final output recorder (aruid %llu): %u Hz, %u channels, format %u", static_cast<unsigned long long>(aruid), out.sample_rate, out.channel_count, out.sample_format);
            if (out.sample_rate != 48000 || out.channel_count != 2) {
                LogLine("audio: the final output recorder gives %u Hz, %u channels, format %u - not the stream's 48 kHz stereo s16", out.sample_rate, out.channel_count, out.sample_format);
                serviceClose(srv);
                return false;
            }
            Handle h = INVALID_HANDLE;
            rc = serviceDispatch(srv, 4, .out_handle_attrs = { SfOutHandleAttr_HipcCopy }, .out_handles = &h);
            if (R_FAILED(rc)) { LogLine("audio: recorder event rc=0x%x", rc); serviceClose(srv); return false; }
            eventLoadRemote(ev, h, true);
            for (size_t i = 0; i < RecCount; ++i) { static_cast<void>(RecAppend(srv, set, i)); }
            rc = serviceDispatch(srv, 1);
            if (R_FAILED(rc)) { LogLine("audio: recorder start rc=0x%x", rc); eventClose(ev); serviceClose(srv); return false; }
            return true;
        }

        int Milli(float v) { return static_cast<int>(v * 1000.0f + (v < 0 ? -0.5f : 0.5f)); }

        void RecVolume(u64 pid) {
            ::Service aud = {};
            ::Result rc = 0;
            {
                SmUse sm;
                rc = smGetService(&aud, "aud:a");
            }
            if (R_FAILED(rc)) { LogLine("audio: aud:a unavailable rc=0x%x", rc); return; }
            float rec = -1.0f, master = -1.0f;
            const ::Result r1 = serviceDispatchInOut(&aud, 8, pid, rec);
            const ::Result r2 = serviceDispatchInOut(&aud, 4, pid, master);
            LogLine("audio: the game's record volume %d/1000 (rc=0x%x), master volume %d/1000 (rc=0x%x)", Milli(rec), r1, Milli(master), r2);
            if (R_SUCCEEDED(r1) && rec < 0.999f) {
                const struct { float vol; u32 pad; u64 aruid; u64 fade; } in = { 1.0f, 0, pid, 0 };
                const ::Result r3 = serviceDispatchIn(&aud, 9, in);
                float now = -1.0f;
                static_cast<void>(serviceDispatchInOut(&aud, 8, pid, now));
                LogLine("audio: record volume raised to full (rc=0x%x) - now %d/1000", r3, Milli(now));
            }
            serviceClose(&aud);
        }

        void RecClose(::Service *srv, ::Event *ev) {
            static_cast<void>(serviceDispatch(srv, 2));
            eventClose(ev);
            serviceClose(srv);
        }

        void RecThread(void *) {
            u64 app_seen = 0, app_since = 0;
            u32 failures = 0;
            for (;;) {
                os::SleepThread(TimeSpan::FromMilliSeconds(500));
                const u64 app = g_app_pid.load(std::memory_order_relaxed);
                const u64 now = armTicksToNs(armGetSystemTick());
                if (app != app_seen) { app_seen = app; app_since = now; failures = 0; }
                if (!g_audio_enabled.load(std::memory_order_relaxed) || app == 0 || failures >= 3) { continue; }
                const u64 grc = g_grc_data_ns.load(std::memory_order_relaxed);
                const bool hb = g_vim_program.load(std::memory_order_relaxed) == g_app_tid.load(std::memory_order_relaxed);
                const u64 wait = hb ? UINT64_C(3000000000) : UINT64_C(15000000000);
                if (now - app_since < wait || (grc != 0 && now - grc < wait)) { continue; }
                if (g_app_tid.load(std::memory_order_relaxed) == 0) { continue; }
                if (!hb && !g_audio_active.load(std::memory_order_relaxed)) { continue; }
                {
                    const u64 lp = g_app_present_tick.load(std::memory_order_relaxed);
                    if (lp == 0 || now - armTicksToNs(lp) > UINT64_C(1000000000)) { continue; }
                }

                ::Service srv = {};
                ::Event ev = {};
                if (!RecOpen(&srv, &ev, GameRec, 0)) { ++failures; os::SleepThread(TimeSpan::FromSeconds(5)); continue; }
                LogLine("audio: grc:d has no sound for this game (pid %llu) - recording its mix through the final output recorder", static_cast<unsigned long long>(app));
                RecVolume(app);
                g_rec_running.store(true, std::memory_order_relaxed);
                g_audio_state.store(1, std::memory_order_relaxed);
                u64 bytes = 0;
                u32 peak = 0, windows = 0;
                u64 win_t0 = armTicksToNs(armGetSystemTick());
                for (;;) {
                    if (g_app_pid.load(std::memory_order_relaxed) != app || !g_audio_enabled.load(std::memory_order_relaxed)) { break; }
                    const u64 g = g_grc_data_ns.load(std::memory_order_relaxed);
                    if (g != 0 && g > app_since && armTicksToNs(armGetSystemTick()) - g < UINT64_C(500000000)) { LogLine("audio: grc:d has sound again - the recorder stops"); break; }
                    if (R_FAILED(eventWait(&ev, 200000000))) { continue; }
                    const bool ok = RecRelease(&srv, GameRec, [&](const u8 *pcm, size_t n) {
                        if (windows < 6) { const u32 p = Peak(pcm, n); if (p > peak) { peak = p; } }
                        bytes += n;
                        {
                            const u64 *w = reinterpret_cast<const u64 *>(pcm);
                            for (size_t q = 0; q < n / 8; ++q) { if (w[q] != 0) { g_rec_sound_ns.store(armTicksToNs(armGetSystemTick()), std::memory_order_relaxed); break; } }
                        }
                        const bool audout = armTicksToNs(armGetSystemTick()) - g_audout_ns.load(std::memory_order_relaxed) < UINT64_C(500000000);
                        if (g_audio_active.load(std::memory_order_relaxed) && !audout && !g_home_running.load(std::memory_order_relaxed)) { Push(pcm, n & ~static_cast<size_t>(3)); }
                    });
                    if (!ok) { ++failures; break; }
                    if (windows < 6 && armTicksToNs(armGetSystemTick()) - win_t0 >= UINT64_C(10000000000)) {
                        ++windows;
                        LogLine("audio: recorder, %u0 s in: %llu KB so far, loudest sample %u of 32767%s", windows, static_cast<unsigned long long>(bytes >> 10), peak,
                                peak == 0 ? " (silence)" : "");
                        peak = 0;
                        win_t0 = armTicksToNs(armGetSystemTick());
                    }
                }
                g_rec_running.store(false, std::memory_order_relaxed);
                RecClose(&srv, &ev);
                LogLine("audio: final output recorder closed after %llu KB", static_cast<unsigned long long>(bytes >> 10));
            }
        }

        constexpr u64 QlaunchId = UINT64_C(0x0100000000001000);

        bool HomeWanted() {
            return g_audio_enabled.load(std::memory_order_relaxed) && g_audio_active.load(std::memory_order_relaxed) &&
                   g_ui_session_active.load(std::memory_order_relaxed);
        }

        void HomeVolumes(u64 pid) {
            ::Service aud = {};
            ::Result rc = 0;
            {
                SmUse sm;
                rc = smGetService(&aud, "aud:a");
            }
            if (R_FAILED(rc)) { return; }
            float rec = -1.0f, master = -1.0f;
            const ::Result r1 = serviceDispatchInOut(&aud, 8, pid, rec);
            const ::Result r2 = serviceDispatchInOut(&aud, 4, pid, master);
            serviceClose(&aud);
            LogLine("audio: qlaunch's record volume %d/1000 (rc=0x%x), master volume %d/1000 (rc=0x%x)", Milli(rec), r1, Milli(master), r2);
        }

        void HomeThread(void *) {
            u32 failures = 0;
            for (;;) {
                os::SleepThread(TimeSpan::FromMilliSeconds(200));
                if (failures >= 3 || !HomeWanted()) { continue; }
                os::ProcessId pid{0};
                if (R_FAILED(::ams::pm::dmnt::GetProcessId(std::addressof(pid), ncm::ProgramId{QlaunchId})) || pid.value == 0) {
                    if (++failures >= 3) { LogLine("audio: qlaunch's process id not found - no HOME menu sound this boot"); }
                    os::SleepThread(TimeSpan::FromSeconds(5));
                    continue;
                }
                ::Service srv = {};
                ::Event ev = {};
                if (!RecOpen(&srv, &ev, HomeRec, pid.value)) {
                    if (++failures >= 3) { LogLine("audio: the HOME menu's recorder did not open 3 times - no HOME menu sound this boot"); }
                    os::SleepThread(TimeSpan::FromSeconds(5));
                    continue;
                }
                failures = 0;
                LogLine("audio: the HOME menu streams - recording qlaunch's mix (aruid %llu)", static_cast<unsigned long long>(pid.value));
                HomeVolumes(pid.value);
                g_home_running.store(true, std::memory_order_relaxed);
                u64 bytes = 0;
                u32 peak = 0, windows = 0, loud = 0;
                u64 win_t0 = armTicksToNs(armGetSystemTick());
                while (HomeWanted()) {
                    if (R_FAILED(eventWait(&ev, 200000000))) { continue; }
                    const bool ok = RecRelease(&srv, HomeRec, [&](const u8 *pcm, size_t n) {
                        const u32 p = Peak(pcm, n);
                        if (p > peak) { peak = p; }
                        if (p != 0) { ++loud; }
                        bytes += n;
                        Push(pcm, n & ~static_cast<size_t>(3));
                    });
                    if (!ok) { ++failures; break; }
                    if (windows < 6 && armTicksToNs(armGetSystemTick()) - win_t0 >= UINT64_C(10000000000)) {
                        ++windows;
                        LogLine("audio: HOME menu recorder, %u0 s in: %llu KB so far, loudest sample %u of 32767, %u buffers with sound%s", windows,
                                static_cast<unsigned long long>(bytes >> 10), peak, loud, peak == 0 ? " (silence)" : "");
                        peak = 0;
                        loud = 0;
                        win_t0 = armTicksToNs(armGetSystemTick());
                    }
                }
                g_home_running.store(false, std::memory_order_relaxed);
                RecClose(&srv, &ev);
                LogLine("audio: the HOME menu's recorder closed after %llu KB", static_cast<unsigned long long>(bytes >> 10));
            }
        }

    }

    void StartAudio() {
        SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_audio_thread), AudioThread, nullptr,
                                        g_audio_stack, sizeof(g_audio_stack),
                                        ThreadBasePriority()));
        os::SetThreadNamePointer(std::addressof(g_audio_thread), "applet-mitm.Audio");
        os::StartThread(std::addressof(g_audio_thread));
        NoteThread(std::addressof(g_audio_thread));
        SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_rec_thread), RecThread, nullptr,
                                        g_rec_stack, sizeof(g_rec_stack),
                                        ThreadBasePriority()));
        os::SetThreadNamePointer(std::addressof(g_rec_thread), "applet-mitm.AudRec");
        os::StartThread(std::addressof(g_rec_thread));
        NoteThread(std::addressof(g_rec_thread));
        SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_home_thread), HomeThread, nullptr,
                                        g_home_stack, sizeof(g_home_stack),
                                        ThreadBasePriority()));
        os::SetThreadNamePointer(std::addressof(g_home_thread), "applet-mitm.AudHome");
        os::StartThread(std::addressof(g_home_thread));
        NoteThread(std::addressof(g_home_thread));
    }

    void AudioAttachRing(u8 *ring) {
        AudioFlush();
        g_ring.store(ring);
    }

    void AudioDetachRing() {
        g_ring.store(nullptr);
        while (g_ring_users.load() != 0) { os::SleepThread(TimeSpan::FromMilliSeconds(1)); }
    }

    size_t AudioTake(u8 *dst, size_t max) {
        u8 *const ring = g_ring.load();
        if (ring == nullptr) { return 0; }
        const size_t head = g_head.load(std::memory_order_acquire);
        size_t tail = g_tail.load(std::memory_order_relaxed);
        size_t n = head - tail;
        if (n > max) { n = max; }
        n &= ~static_cast<size_t>(3);
        for (size_t i = 0; i < n; ) {
            const size_t at = (tail + i) % RingSize;
            const size_t k = (RingSize - at) < (n - i) ? (RingSize - at) : (n - i);
            std::memcpy(dst + i, ring + at, k);
            i += k;
        }
        g_tail.store(tail + n, std::memory_order_release);
        return n;
    }

    bool AudioOutWanted() {
        if (!g_audio_active.load(std::memory_order_relaxed) || g_home_running.load(std::memory_order_relaxed)) { return false; }
        const u64 now = armTicksToNs(armGetSystemTick());
        const u64 grc = g_grc_data_ns.load(std::memory_order_relaxed);
        if (grc != 0 && now - grc < UINT64_C(3000000000)) { return false; }
        const u64 rec = g_rec_sound_ns.load(std::memory_order_relaxed);
        if (rec != 0 && now - rec < UINT64_C(2000000000)) { return false; }
        return true;
    }

    void AudioOutFeed(const u8 *pcm, size_t n, u32 channels) {
        static bool said = false;
        if (!said) { said = true; LogLine("audio: the game's own audout sound goes into the stream (%u channel(s)) - grc:d and the recorder have none", channels); }
        g_audout_ns.store(armTicksToNs(armGetSystemTick()), std::memory_order_relaxed);
        if (channels == 2) { Push(pcm, n & ~static_cast<size_t>(3)); return; }
        if (channels == 0 || channels > 8) { return; }
        alignas(8) u8 st[1024];
        const size_t frame = static_cast<size_t>(channels) * 2;
        size_t o = 0;
        for (size_t i = 0; i + frame <= n; i += frame) {
            const u8 *f = pcm + i;
            if (channels == 1) { std::memcpy(st + o, f, 2); std::memcpy(st + o + 2, f, 2); }
            else { std::memcpy(st + o, f, 4); }
            o += 4;
            if (o == sizeof(st)) { Push(st, o); o = 0; }
        }
        if (o != 0) { Push(st, o); }
    }

    void AudioFlush() {
        g_tail.store(g_head.load(std::memory_order_acquire), std::memory_order_release);
    }

}
