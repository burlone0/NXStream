#include "core/system.hpp"

#include <sys/stat.h>

#include <cstdarg>
#include <cstdio>
#include <new>
#include <utility>

namespace sys {
namespace {
constexpr const char* kLogDir = "sdmc:/config/nxstream";
constexpr const char* kLogPath = "sdmc:/config/nxstream/log.txt";
constexpr const char* kPreviousLogPath = "sdmc:/config/nxstream/log.prev.txt";
constexpr const char* kOldDir = "sdmc:/config/nstream";
constexpr long kMaxLogBytes = 2 * 1024 * 1024;
constexpr size_t kRingBytes = 0x8000;

Mutex g_logMutex;
CondVar g_logCv;
char g_ring[kRingBytes];
size_t g_head = 0, g_used = 0;
uint64_t g_dropped = 0;
FILE* g_log = nullptr;

struct ThreadStart {
    Thread thread;
    std::function<void()> fn;
};

void threadEntry(void* arg) {
    auto* start = static_cast<ThreadStart*>(arg);
    start->fn();
}

int g_migration = 0;

bool copyFile(const char* from, const char* to) {
    FILE* in = std::fopen(from, "rb");
    if (!in) return false;
    FILE* out = std::fopen(to, "wb");
    if (!out) {
        std::fclose(in);
        return false;
    }
    char buf[0x400];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, in)) > 0) std::fwrite(buf, 1, n, out);
    std::fclose(in);
    std::fclose(out);
    return true;
}

void migrateConfigDir() {
    struct stat st;
    if (stat(kLogDir, &st) == 0 || stat(kOldDir, &st) != 0) return;
    if (std::rename(kOldDir, kLogDir) == 0) {
        g_migration = 1;
        return;
    }
    mkdir(kLogDir, 0777);
    const bool ini = copyFile("sdmc:/config/nstream/config.ini", "sdmc:/config/nxstream/config.ini");
    const bool params = copyFile("sdmc:/config/nstream/h264params.bin", "sdmc:/config/nxstream/h264params.bin");
    g_migration = ini || params ? 2 : 3;
}

void rotate() {
    std::remove(kPreviousLogPath);
    std::rename(kLogPath, kPreviousLogPath);
}

void drain() {
    static char chunk[0x1000];
    if (!g_log) {
        Lock lock(g_logMutex);
        g_used = 0;
        return;
    }
    size_t n;
    uint64_t dropped;
    {
        Lock lock(g_logMutex);
        if (g_used == 0) return;
        const size_t tail = (g_head + kRingBytes - g_used) % kRingBytes;
        n = g_used < sizeof chunk ? g_used : sizeof chunk;
        for (size_t i = 0; i < n; ++i) chunk[i] = g_ring[(tail + i) % kRingBytes];
        g_used -= n;
        dropped = g_dropped;
        g_dropped = 0;
    }
    if (std::ftell(g_log) > kMaxLogBytes) {
        std::fclose(g_log);
        rotate();
        g_log = std::fopen(kLogPath, "w");
        if (!g_log) return;
    }
    if (dropped)
        std::fprintf(g_log, "[log] %llu lines dropped (SD card too slow)\n", static_cast<unsigned long long>(dropped));
    std::fwrite(chunk, 1, n, g_log);
    std::fflush(g_log);
}

void writerLoop() {
    while (true) {
        {
            Lock lock(g_logMutex);
            while (g_used == 0) condvarWait(&g_logCv, &g_logMutex);
        }
        drain();
    }
}
}

void logOpen() {
    mutexInit(&g_logMutex);
    condvarInit(&g_logCv);
    mkdir("sdmc:/config", 0777);
    migrateConfigDir();
    mkdir(kLogDir, 0777);
    rotate();
    g_log = std::fopen(kLogPath, "w");
    spawn(writerLoop, 0x3F, 0x3000, "log-writer");
    if (g_migration == 1) LOG("migrated settings: renamed %s to %s", kOldDir, kLogDir);
    else if (g_migration == 2)
        LOG("migrated settings: copied config.ini and h264params.bin from %s to %s", kOldDir, kLogDir);
    else if (g_migration == 3) LOG("settings migration from %s failed, using defaults", kOldDir);
}

void log(const char* fmt, ...) {
    char line[512];
    const uint64_t ms = nowMs();
    int len = std::snprintf(line, sizeof line, "[%02llu:%02llu:%02llu.%03llu] ",
                            static_cast<unsigned long long>(ms / 3600000),
                            static_cast<unsigned long long>(ms / 60000 % 60),
                            static_cast<unsigned long long>(ms / 1000 % 60),
                            static_cast<unsigned long long>(ms % 1000));
    va_list args;
    va_start(args, fmt);
    const int body = std::vsnprintf(line + len, sizeof line - len - 1, fmt, args);
    va_end(args);
    len += body < 0 ? 0 : body;
    if (len > static_cast<int>(sizeof line) - 2) len = sizeof line - 2;
    line[len++] = '\n';

    Lock lock(g_logMutex);
    if (g_used + static_cast<size_t>(len) > kRingBytes) {
        ++g_dropped;
        return;
    }
    for (int i = 0; i < len; ++i) g_ring[(g_head + i) % kRingBytes] = line[i];
    g_head = (g_head + len) % kRingBytes;
    g_used += len;
    condvarWakeOne(&g_logCv);
}

bool spawn(std::function<void()> fn, int priority, size_t stackSize, const char* name) {
    auto* start = new (std::nothrow) ThreadStart{};
    if (!start) return false;
    start->fn = std::move(fn);
    Result rc = threadCreate(&start->thread, threadEntry, start, nullptr, stackSize, priority, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&start->thread);
    if (R_FAILED(rc)) {
        LOG("thread %s: start failed 0x%x", name, rc);
        delete start;
        return false;
    }
    return true;
}
}
