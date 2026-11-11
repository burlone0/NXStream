#include "applet_mitm_log.hpp"
#include "applet_mitm_nv.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace ams::mitm::applet {

    namespace {
        constinit s32 g_thread_base_prio = 21;
    }
    void SetThreadBasePriority(s32 prio) { g_thread_base_prio = prio; }
    s32 ThreadBasePriority() { return g_thread_base_prio; }

    constinit Stats g_stats = {};

    namespace {

        const char *LogPath  = "sdmc:/applet-mitm.log";
        const char *LastPath = "sdmc:/applet-mitm.last";
        constexpr s64 ReleaseLogCap = 4 * 1024 * 1024;
        constinit os::SdkMutex g_lock;
        constinit bool g_ready = false;
        constinit bool g_defer = false;
        constinit u64 g_defer_tick = 0;
        constinit char g_defer_buf[8192];
        constinit size_t g_defer_len = 0;

        void AppendToLog(const char *data, size_t n) {
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), LogPath, fs::OpenMode_Write | fs::OpenMode_AllowAppend))) { return; }
            s64 size = 0;
            static_cast<void>(fs::GetFileSize(std::addressof(size), f));
            if (g_release_mode && size > ReleaseLogCap) { fs::CloseFile(f); return; }
            static_cast<void>(fs::WriteFile(f, size, data, n, fs::WriteOption::Flush));
            fs::CloseFile(f);
        }

        void WriteWhole(const char *path, const char *data, size_t len) {
            fs::DeleteFile(path);
            if (R_FAILED(fs::CreateFile(path, static_cast<s64>(len)))) { return; }
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), path, fs::OpenMode_Write))) { return; }
            static_cast<void>(fs::WriteFile(f, 0, data, len, fs::WriteOption::Flush));
            fs::CloseFile(f);
        }
        constinit u32 g_held_dropped = 0;

        bool StreamQuiet() { return g_release_mode && g_streaming_now.load(std::memory_order_relaxed); }
        bool GameQuiet() { return g_release_mode && g_app_pid.load(std::memory_order_relaxed) != 0; }
        bool Quiet() { return StreamQuiet() || GameQuiet(); }
        constexpr u64 GameFlushNs = UINT64_C(60000000000);
        constinit u64 g_held_tick = 0;

        void DropOldest(size_t n) {
            size_t cut = 0;
            while (g_defer_len - cut + n > sizeof(g_defer_buf) && cut < g_defer_len) {
                const char *nl = static_cast<const char *>(std::memchr(g_defer_buf + cut, '\n', g_defer_len - cut));
                cut = nl != nullptr ? static_cast<size_t>(nl - g_defer_buf) + 1 : g_defer_len;
                ++g_held_dropped;
            }
            std::memmove(g_defer_buf, g_defer_buf + cut, g_defer_len - cut);
            g_defer_len -= cut;
        }

        void WriteHeld() {
            if (g_held_dropped != 0) {
                char b[96];
                const int n = std::snprintf(b, sizeof(b), "(%u older lines from the stream not kept - the buffer was full)\n", g_held_dropped);
                if (n > 0) { AppendToLog(b, static_cast<size_t>(n)); }
                g_held_dropped = 0;
            }
            if (g_defer_len != 0) { AppendToLog(g_defer_buf, g_defer_len); }
            g_defer_len = 0;
        }

    }

    constinit bool g_release_mode = false;
    constinit u32 g_arm_probe_rc = 0;

    void LogInit() {
        std::scoped_lock lk(g_lock);
        if (R_FAILED(fs::MountSdCard("sdmc"))) {
            return;
        }
        {
            fs::DirectoryEntryType t;
            const Result r = fs::GetEntryType(std::addressof(t), "sdmc:/applet-mitm.armed");
            g_arm_probe_rc = r.GetValue();
            g_release_mode = R_FAILED(r);
        }
        if (g_release_mode) {
            static_cast<void>(fs::CreateDirectory("sdmc:/config"));
            static_cast<void>(fs::CreateDirectory("sdmc:/config/nxstream"));
            static_cast<void>(fs::CreateDirectory("sdmc:/config/nxstream/tap"));
            LogPath  = "sdmc:/config/nxstream/tap/log.txt";
            LastPath = "sdmc:/config/nxstream/tap/last.txt";
        }
        if (g_release_mode) {
            static_cast<void>(fs::DeleteFile("sdmc:/config/nxstream/tap/log.prev.txt"));
            static_cast<void>(fs::RenameFile(LogPath, "sdmc:/config/nxstream/tap/log.prev.txt"));
            static_cast<void>(fs::DeleteFile("sdmc:/config/nxstream/tap/last.prev.txt"));
            static_cast<void>(fs::RenameFile(LastPath, "sdmc:/config/nxstream/tap/last.prev.txt"));
        }
        fs::DeleteFile(LogPath);
        fs::CreateFile(LogPath, 0);
        g_ready = true;
        WriteWhole(LastPath, "LogInit\n", 8);
    }

    void LogMark(const char *what) {
        {
            std::scoped_lock lk(g_lock);
            if (g_ready && !g_defer && !Quiet()) {
                char b[128];
                const int n = std::snprintf(b, sizeof(b), "%s\n", what);
                if (n > 0) { WriteWhole(LastPath, b, static_cast<size_t>(n)); }
            }
        }
        LogLine("-> %s", what);
    }

    void LogLine(const char *fmt, ...) {
        std::scoped_lock lk(g_lock);
        if (!g_ready) {
            return;
        }

        char line[512];
        const u64 ms = armTicksToNs(armGetSystemTick()) / UINT64_C(1000000);
        int n = std::snprintf(line, sizeof(line), "[%6llu.%03llu] ",
                              static_cast<unsigned long long>(ms / 1000),
                              static_cast<unsigned long long>(ms % 1000));

        std::va_list ap;
        va_start(ap, fmt);
        n += std::vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
        va_end(ap);
        if (n < 0) { return; }
        if (static_cast<size_t>(n) > sizeof(line) - 2) { n = sizeof(line) - 2; }
        line[n++] = '\n';

        if (Quiet()) {
            if (g_defer_len + static_cast<size_t>(n) > sizeof(g_defer_buf)) {
                if (StreamQuiet()) { DropOldest(static_cast<size_t>(n)); } else { WriteHeld(); }
            }
            if (g_defer_len == 0) { g_held_tick = armGetSystemTick(); }
            std::memcpy(g_defer_buf + g_defer_len, line, static_cast<size_t>(n));
            g_defer_len += static_cast<size_t>(n);
            return;
        }
        if (g_defer) {
            const bool full = g_defer_len + static_cast<size_t>(n) > sizeof(g_defer_buf);
            if (!full && armTicksToNs(armGetSystemTick() - g_defer_tick) < UINT64_C(3000000000)) {
                std::memcpy(g_defer_buf + g_defer_len, line, static_cast<size_t>(n));
                g_defer_len += static_cast<size_t>(n);
                return;
            }
            g_defer = false;
        }
        WriteHeld();
        AppendToLog(line, static_cast<size_t>(n));
    }

    void LogDefer(bool on) {
        std::scoped_lock lk(g_lock);
        if (on) {
            if (!g_defer) { g_defer_tick = armGetSystemTick(); }
            g_defer = true;
            return;
        }
        g_defer = false;
        if (g_ready && !Quiet()) { WriteHeld(); }
    }

    void LogFlushHeld() {
        std::scoped_lock lk(g_lock);
        if (g_ready && !g_defer && !Quiet()) { WriteHeld(); }
    }

    bool LogQuiet() { return Quiet(); }

    void LogFlushStale() {
        std::scoped_lock lk(g_lock);
        if (g_ready && !g_defer && !StreamQuiet() && g_defer_len != 0 && armTicksToNs(armGetSystemTick() - g_held_tick) >= GameFlushNs) { WriteHeld(); }
    }

    void LogWriteHeldNow() {
        std::scoped_lock lk(g_lock);
        if (g_ready) { WriteHeld(); }
    }

    namespace {
        constexpr u32 MaxNoted = 32;
        constinit const os::ThreadType *g_noted[MaxNoted] = {};
        constinit u32 g_noted_n = 0;
    }

    void NoteThread(const os::ThreadType *t) {
        std::scoped_lock lk(g_lock);
        for (u32 k = 0; k < g_noted_n; ++k) { if (g_noted[k] == t) { return; } }
        if (g_noted_n < MaxNoted) { g_noted[g_noted_n++] = t; }
    }

    void LogStackUse() {
        u32 n = 0;
        { std::scoped_lock lk(g_lock); n = g_noted_n; }
        for (u32 k = 0; k < n; ++k) {
            const ::Thread *impl = g_noted[k]->thread_impl;
            if (impl == nullptr || impl->stack_mirror == nullptr) { continue; }
            const size_t size = impl->stack_sz;
            const u8 *lo = static_cast<const u8 *>(impl->stack_mirror);
            size_t untouched = 0;
            while (untouched < size && lo[untouched] == 0) { ++untouched; }
            const char *name = os::GetThreadNamePointer(g_noted[k]);
            LogLine("stack: %s used %zu of %zu KB", name != nullptr ? name : "?", (size - untouched + 1023) / 1024, size / 1024);
        }
    }

}
