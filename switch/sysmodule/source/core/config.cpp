#include "core/config.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/system.hpp"

namespace {
bool parseBool(const char* v) {
    return std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0 || std::strcmp(v, "on") == 0;
}

char* trim(char* s) {
    while (*s == ' ' || *s == '\t') ++s;
    char* end = s + std::strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) *--end = '\0';
    return s;
}
}

namespace settings {
namespace {
bool g_bootNative = false;
}

bool nativeCapture() { return g_bootNative; }

void reloadIfChanged() {
    static bool bootRead = false;
    static bool changeLogged = false;
    static bool applied = false;
    static std::string last;

    char raw[2048];
    size_t len = 0;
    if (FILE* f = std::fopen(kPath, "r")) {
        len = std::fread(raw, 1, sizeof raw, f);
        std::fclose(f);
    } else if (applied) {
        return;
    }
    const std::string content(raw, len);
    if (applied && content == last) return;
    applied = true;
    last = content;

    bool en = true, game = true, aud = true, screen = true;
    uint32_t fps = 0, kbps = 0, probeReq = 0;
    Transport tr = Transport::Network;
    bool native = false;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t end = content.find('\n', pos);
        if (end == std::string::npos) end = content.size();
        const size_t lineLen = end - pos;
        char line[128];
        const bool fits = lineLen < sizeof line;
        if (fits) {
            std::memcpy(line, content.data() + pos, lineLen);
            line[lineLen] = '\0';
        }
        pos = end + 1;
        if (!fits) continue;

        char* s = trim(line);
        if (*s == '\0' || *s == ';' || *s == '#' || *s == '[') continue;
        char* eq = std::strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = trim(s);
        char* value = eq + 1;
        if (char* comment = std::strchr(value, ';')) *comment = '\0';
        value = trim(value);

        if (std::strcmp(key, "enabled") == 0) en = parseBool(value);
        else if (std::strcmp(key, "game_capture") == 0) game = parseBool(value);
        else if (std::strcmp(key, "audio") == 0) aud = parseBool(value);
        else if (std::strcmp(key, "screen_capture") == 0) screen = parseBool(value);
        else if (std::strcmp(key, "screen_fps") == 0) fps = static_cast<uint32_t>(std::clamp(std::atoi(value), 0, 60));
        else if (std::strcmp(key, "screen_kbps") == 0)
            kbps = static_cast<uint32_t>(std::clamp(std::atoi(value), 0, 100000));
        else if (std::strcmp(key, "transport") == 0) {
            if (std::strcmp(value, "usb") == 0) tr = Transport::Usb;
            else if (std::strcmp(value, "network") == 0) tr = Transport::Network;
            else tr = transport.load();
        } else if (std::strcmp(key, "capture") == 0) native = std::strcmp(value, "native") == 0;
        else if (std::strcmp(key, "probe_request") == 0) probeReq = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
    }

    if (!bootRead) {
        bootRead = true;
        g_bootNative = native;
    } else if (native != g_bootNative && !changeLogged) {
        changeLogged = true;
        LOG("capture: change applies after restart");
    }
    enabled = en;
    gameCapture = game;
    audio = aud;
    screenCapture = screen;
    screenFps = fps;
    screenKbps = kbps;
    probeRequest = probeReq;
    transport = tr;
    LOG("config: transport=%s enabled=%d game_capture=%d audio=%d screen_capture=%d screen_fps=%u (limit %u) "
        "screen_kbps=%u (limit %u kbit/s)",
        transportName(tr), en, game, aud, screen, static_cast<unsigned>(fps), static_cast<unsigned>(screenFpsLimit()),
        static_cast<unsigned>(kbps), static_cast<unsigned>(screenBytesPerSecond() * 8 / 1000));
}

uint32_t screenFpsLimit() {
    const uint32_t fps = screenFps.load();
    return fps ? fps : recorderFps.load();
}

uint32_t screenBytesPerSecond() {
    if (transport.load() == Transport::Usb) return 100000u * 1000 / 8;
    const uint32_t kbps = screenKbps.load();
    if (kbps) return kbps * 1000 / 8;
    return std::max<uint32_t>(recorderBitrate.load() / 2 * 3, 10000000) / 8;
}
}
