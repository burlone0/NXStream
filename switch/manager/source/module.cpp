#include "module.hpp"

#include <switch.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace module {
namespace {
constexpr const char* kContentsDir = "sdmc:/atmosphere/contents/4200000000004E53";
constexpr const char* kExefs = "sdmc:/atmosphere/contents/4200000000004E53/exefs.nsp";
constexpr const char* kFlagsDir = "sdmc:/atmosphere/contents/4200000000004E53/flags";
constexpr const char* kBootFlag = "sdmc:/atmosphere/contents/4200000000004E53/flags/boot2.flag";
constexpr const char* kNativeContentsDirs[2] = {"sdmc:/atmosphere/contents/4200000000004E54",
                                                "sdmc:/atmosphere/contents/4200000000004E55"};
constexpr const char* kNativeFlagsDirs[2] = {"sdmc:/atmosphere/contents/4200000000004E54/flags",
                                             "sdmc:/atmosphere/contents/4200000000004E55/flags"};
constexpr const char* kNativeBootFlags[2] = {"sdmc:/atmosphere/contents/4200000000004E54/flags/boot2.flag",
                                             "sdmc:/atmosphere/contents/4200000000004E55/flags/boot2.flag"};
constexpr const char* kConfigDir = "sdmc:/config/nxstream";
constexpr const char* kConfigPath = "sdmc:/config/nxstream/config.ini";
constexpr const char* kTapConfigDir = "sdmc:/config/nxstream/tap";
constexpr const char* kTapConfigPath = "sdmc:/config/nxstream/tap/config.ini";
constexpr const char* kProbeResultPath = "sdmc:/config/nxstream/probe_result.ini";
constexpr const char* kSystemSettingsPath = "sdmc:/atmosphere/config/system_settings.ini";

constexpr uint16_t kDiscoveryPort = 48501;
constexpr char kDiscoverMessage[] = "NSTREAM_DISCOVER_V2";
constexpr char kAnnounceMagic[] = "NSTREAM_HERE_V2";
constexpr auto kReplyTimeout = std::chrono::seconds(3);

bool g_pmdmnt = false;
bool g_pmshell = false;
bool g_socketReady = false;
int g_sock = -1;
bool g_haveReply = false;
bool g_replyBusy = false;
std::chrono::steady_clock::time_point g_lastReply;

bool exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

std::string stripped(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> readLines(const char* path) {
    std::vector<std::string> lines;
    FILE* f = std::fopen(path, "r");
    if (!f) return lines;
    char buf[256];
    std::string line;
    while (std::fgets(buf, sizeof buf, f)) {
        line += buf;
        if (line.back() != '\n') continue;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        lines.push_back(std::move(line));
        line.clear();
    }
    if (!line.empty()) lines.push_back(std::move(line));
    std::fclose(f);
    return lines;
}

char g_lastWriteError[32] = "";

void setWriteError(const char* op, int err) { std::snprintf(g_lastWriteError, sizeof g_lastWriteError, "%s %d", op, err); }

bool writeAtomic(const char* path, const std::string& content) {
    g_lastWriteError[0] = '\0';
    const std::string tmp = std::string(path) + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) {
        setWriteError("open", errno);
        return false;
    }
    bool ok = std::fwrite(content.data(), 1, content.size(), f) == content.size();
    if (!ok) setWriteError("write", errno);
    const bool closed = std::fclose(f) == 0;
    if (ok && !closed) {
        setWriteError("close", errno);
        ok = false;
    }
    if (!ok) {
        std::remove(tmp.c_str());
        return false;
    }
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (attempt > 0) svcSleepThread(50'000'000ll);
        if (std::remove(path) != 0) {
            const int err = errno;
            if (exists(path)) {
                setWriteError("remove", err);
                continue;
            }
        }
        if (std::rename(tmp.c_str(), path) == 0) {
            g_lastWriteError[0] = '\0';
            return true;
        }
        setWriteError("rename", errno);
    }
    if (exists(path)) std::remove(tmp.c_str());
    return false;
}

void restoreInterrupted(const char* path) {
    const std::string tmp = std::string(path) + ".tmp";
    if (!exists(path) && exists(tmp.c_str())) std::rename(tmp.c_str(), path);
}

template <typename Fn>
void readKeyValues(const char* path, Fn&& fn) {
    for (const std::string& line : readLines(path)) {
        const std::string s = stripped(line);
        if (s.empty() || s[0] == ';' || s[0] == '#' || s[0] == '[') continue;
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string value = s.substr(eq + 1);
        value.resize(std::min(value.size(), value.find(';')));
        fn(stripped(s.substr(0, eq)).c_str(), stripped(value).c_str());
    }
}

bool isKeyLine(const std::string& line, const char* key) {
    const std::string s = stripped(line);
    const size_t eq = s.find('=');
    return eq != std::string::npos && stripped(s.substr(0, eq)) == key;
}

void applyRecorderSetting(std::vector<std::string>& lines, const char* key, uint32_t number, uint32_t fallback) {
    char value[96];
    std::snprintf(value, sizeof value, "%s = u32!0x%X", key, static_cast<unsigned>(number));
    const bool remove = number == fallback;

    bool inSection = false, sectionFound = false, done = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string s = stripped(lines[i]);
        if (!s.empty() && s[0] == '[') {
            if (inSection && !done && !remove) {
                size_t at = i;
                while (at > 0 && stripped(lines[at - 1]).empty()) --at;
                lines.insert(lines.begin() + static_cast<long>(at), value);
                done = true;
                ++i;
            }
            inSection = s == "[am.debug]";
            sectionFound = sectionFound || inSection;
            continue;
        }
        if (inSection && isKeyLine(s, key)) {
            if (remove) {
                lines.erase(lines.begin() + static_cast<long>(i));
                --i;
            } else {
                lines[i] = value;
            }
            done = true;
        }
    }
    if (!done && !remove) {
        if (!sectionFound) {
            if (!lines.empty() && !stripped(lines.back()).empty()) lines.push_back("");
            lines.push_back("[am.debug]");
        }
        lines.push_back(value);
    }
}

void setKeyLine(std::vector<std::string>& lines, const char* key, const std::string& value) {
    const std::string line = std::string(key) + " = " + value;
    for (std::string& l : lines) {
        if (isKeyLine(l, key)) {
            l = line;
            return;
        }
    }
    lines.push_back(line);
}

bool writeTapConfig(bool usb, bool* changed = nullptr) {
    mkdir("sdmc:/config", 0777);
    mkdir(kConfigDir, 0777);
    mkdir(kTapConfigDir, 0777);
    std::vector<std::string> lines = readLines(kTapConfigPath);
    const std::vector<std::string> before = lines;
    setKeyLine(lines, "home_menu", "1");
    setKeyLine(lines, "force_docked", "1");
    setKeyLine(lines, "max_resolution", "1080");
    setKeyLine(lines, "memory_boost", "1");
    setKeyLine(lines, "mid_collect", "1");
    setKeyLine(lines, "max_fps", "30");
    setKeyLine(lines, "qp", usb ? "16" : "22");
    if (changed) *changed = lines != before;
    if (lines == before) return true;
    std::string content;
    for (const std::string& l : lines) content += l + "\n";
    return writeAtomic(kTapConfigPath, content);
}
}

bool refreshTapConfig(bool usb, bool& changed) {
    changed = false;
    return writeTapConfig(usb, &changed);
}

Settings Settings::load() {
    Settings s;
    readKeyValues(kConfigPath, [&](const char* key, const char* value) {
        const bool on = std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0;
        if (std::strcmp(key, "enabled") == 0) s.enabled = on;
        else if (std::strcmp(key, "game_capture") == 0) s.gameCapture = on;
        else if (std::strcmp(key, "audio") == 0) s.audio = on;
        else if (std::strcmp(key, "screen_capture") == 0) s.screenCapture = on;
        else if (std::strcmp(key, "screen_fps") == 0) s.screenFps = std::clamp(std::atoi(value), 0, 60);
        else if (std::strcmp(key, "screen_kbps") == 0) s.screenKbps = std::clamp(std::atoi(value), 0, 100000);
        else if (std::strcmp(key, "transport") == 0) s.usb = std::strcmp(value, "usb") == 0;
        else if (std::strcmp(key, "capture") == 0) s.native = std::strcmp(value, "native") == 0;
    });
    return s;
}

namespace {
struct Extras {
    uint32_t probeRequest = 0;
    uint32_t netBitrate = 0;
    uint32_t netKeyframes = 0;
};

Extras readExtras() {
    Extras e;
    readKeyValues(kConfigPath, [&](const char* key, const char* value) {
        const auto number = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
        if (std::strcmp(key, "probe_request") == 0) e.probeRequest = number;
        else if (std::strcmp(key, "net_recorder_bitrate") == 0) e.netBitrate = number;
        else if (std::strcmp(key, "net_keyframes") == 0) e.netKeyframes = number;
    });
    return e;
}

bool writeConfig(const Settings& s, const Extras& e) {
    mkdir("sdmc:/config", 0777);
    mkdir(kConfigDir, 0777);
    char content[1024];
    size_t n = static_cast<size_t>(std::snprintf(content, sizeof content,
                          "[nxstream]\n"
                          "enabled=%d\n"
                          "game_capture=%d\n"
                          "audio=%d\n"
                          "screen_capture=%d\n"
                          "screen_fps=%d\n"
                          "screen_kbps=%d\n"
                          "transport=%s\n"
                          "capture=%s\n",
                          s.enabled, s.gameCapture, s.audio, s.screenCapture, s.screenFps, s.screenKbps,
                          s.usb ? "usb" : "network", s.native ? "native" : "grc"));
    auto add = [&](const char* key, uint32_t value) {
        if (value > 0) n += static_cast<size_t>(std::snprintf(content + n, sizeof content - n, "%s=%u\n", key,
                                                              static_cast<unsigned>(value)));
    };
    add("probe_request", e.probeRequest);
    add("net_recorder_bitrate", e.netBitrate);
    add("net_keyframes", e.netKeyframes);
    return writeAtomic(kConfigPath, content);
}
}

bool Settings::save() const { return writeConfig(*this, readExtras()); }

bool setTransport(Settings& settings, bool usb, uint32_t currentBitrate, uint32_t& newBitrate) {
    Extras e = readExtras();
    Settings next = settings;
    next.usb = usb;
    if (usb) {
        if (e.netBitrate == 0) e.netBitrate = currentBitrate;
        newBitrate = kUsbBitrate;
    } else {
        newBitrate = e.netBitrate > 0 ? e.netBitrate : 10000000;
        e.netBitrate = 0;
        e.netKeyframes = 0;
    }
    if (settings.native && !writeTapConfig(usb)) return false;
    if (!writeConfig(next, e)) return false;
    settings = next;
    return true;
}

bool nativeFilesPresent() {
    return exists("sdmc:/atmosphere/contents/4200000000004E54/exefs.nsp") &&
           exists("sdmc:/atmosphere/contents/4200000000004E55/exefs.nsp");
}

bool setNative(Settings& settings, bool native) {
    if (native) {
        mkdir("sdmc:/atmosphere", 0777);
        mkdir("sdmc:/atmosphere/contents", 0777);
        mkdir(kNativeContentsDirs[1], 0777);
        mkdir(kNativeFlagsDirs[1], 0777);
        FILE* f = std::fopen(kNativeBootFlags[1], "w");
        if (!f) {
            setWriteError("flag", errno);
            return false;
        }
        std::fclose(f);
        if (!writeTapConfig(settings.usb)) return false;
    } else {
        for (int i = 0; i < 2; ++i) {
            if (std::remove(kNativeBootFlags[i]) != 0 && exists(kNativeBootFlags[i])) {
                setWriteError("flag", errno);
                return false;
            }
        }
    }
    Settings next = settings;
    next.native = native;
    if (!writeConfig(next, readExtras())) return false;
    settings = next;
    return true;
}

bool requestProbe(const Settings& settings, uint32_t& id) {
    Extras e = readExtras();
    id = e.probeRequest + 1;
    if (id == 0) id = 1;
    e.probeRequest = id;
    return writeConfig(settings, e);
}

const char* lastWriteError() { return g_lastWriteError; }

ProbeResult readProbeResult() {
    ProbeResult r;
    readKeyValues(kProbeResultPath, [&](const char* key, const char* value) {
        r.valid = true;
        if (std::strcmp(key, "id") == 0) r.id = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
        else if (std::strcmp(key, "state") == 0) r.state = value;
        else if (std::strcmp(key, "reason") == 0) r.reason = value;
        else if (std::strcmp(key, "step") == 0) r.step = std::atoi(value);
        else if (std::strcmp(key, "run") == 0) r.run = std::atoi(value);
        else if (std::strcmp(key, "pick") == 0) r.pick = std::atoi(value);
        else if (std::strcmp(key, "capacity_kbps") == 0) r.capacityKbps = std::atoi(value);
        else if (std::strcmp(key, "send_p99") == 0) r.sendP99 = std::atoi(value);
        else if (std::strcmp(key, "ack_p95") == 0) r.ackP95 = std::atoi(value);
    });
    return r;
}

uint32_t recorderSetting(const char* key, uint32_t fallback) {
    bool inSection = false;
    for (const std::string& line : readLines(kSystemSettingsPath)) {
        const std::string s = stripped(line);
        if (!s.empty() && s[0] == '[') inSection = s == "[am.debug]";
        else if (inSection && isKeyLine(s, key)) {
            const size_t bang = s.find('!');
            const size_t from = bang == std::string::npos ? s.find('=') + 1 : bang + 1;
            const unsigned long v = std::strtoul(s.c_str() + from, nullptr, 0);
            if (v > 0) return static_cast<uint32_t>(v);
        }
    }
    return fallback;
}

bool setRecorderSettings(uint32_t bitrate, uint32_t fps, uint32_t keyframes) {
    std::vector<std::string> lines = readLines(kSystemSettingsPath);
    applyRecorderSetting(lines, kBitrateKey, bitrate, kDefaultBitrate);
    applyRecorderSetting(lines, kFpsKey, fps, kDefaultFps);
    applyRecorderSetting(lines, kKeyframeKey, keyframes, kDefaultKeyframes);
    std::string content;
    for (const std::string& line : lines) content += line + "\n";
    mkdir("sdmc:/atmosphere", 0777);
    mkdir("sdmc:/atmosphere/config", 0777);
    return writeAtomic(kSystemSettingsPath, content);
}

bool init() {
    struct stat st;
    if (stat(kConfigDir, &st) != 0 && stat("sdmc:/config/nstream", &st) == 0 &&
        std::rename("sdmc:/config/nstream", kConfigDir) != 0) {
        mkdir(kConfigDir, 0777);
        for (const char* name : {"config.ini", "h264params.bin"}) {
            const std::string from = std::string("sdmc:/config/nstream/") + name;
            const std::string to = std::string(kConfigDir) + "/" + name;
            FILE* in = std::fopen(from.c_str(), "rb");
            if (!in) continue;
            FILE* out = std::fopen(to.c_str(), "wb");
            char buf[0x400];
            size_t n;
            while (out && (n = std::fread(buf, 1, sizeof buf, in)) > 0) std::fwrite(buf, 1, n, out);
            if (out) std::fclose(out);
            std::fclose(in);
        }
    }
    restoreInterrupted(kConfigPath);
    restoreInterrupted(kSystemSettingsPath);
    g_socketReady = R_SUCCEEDED(socketInitializeDefault());
    if (g_socketReady) {
        g_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (g_sock >= 0) fcntl(g_sock, F_SETFL, fcntl(g_sock, F_GETFL, 0) | O_NONBLOCK);
    }
    g_pmdmnt = R_SUCCEEDED(pmdmntInitialize());
    g_pmshell = R_SUCCEEDED(pmshellInitialize());
    return g_pmdmnt;
}

void exit() {
    if (g_sock >= 0) close(g_sock);
    g_sock = -1;
    if (g_socketReady) socketExit();
    g_socketReady = false;
    if (g_pmshell) pmshellExit();
    if (g_pmdmnt) pmdmntExit();
}

Status query() {
    Status st;
    st.installed = exists(kExefs);
    st.autostart = exists(kBootFlag);
    u64 pid = 0;
    st.nativeFlags = exists(kNativeBootFlags[0]) || exists(kNativeBootFlags[1]);
    u64 tapPid = 0;
    st.nativeRunning = g_pmdmnt && R_SUCCEEDED(pmdmntGetProcessId(&tapPid, kTapProgramId)) && tapPid != 0;
    st.running = g_pmdmnt && R_SUCCEEDED(pmdmntGetProcessId(&pid, kProgramId)) && pid != 0;

    if (g_sock >= 0) {
        char buf[256];
        ssize_t n;
        while ((n = recv(g_sock, buf, sizeof buf, 0)) > 0) {
            if (n < static_cast<ssize_t>(sizeof kAnnounceMagic - 1) ||
                std::memcmp(buf, kAnnounceMagic, sizeof kAnnounceMagic - 1) != 0)
                continue;
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) --n;
            g_replyBusy = buf[n - 1] == '1';
            g_lastReply = std::chrono::steady_clock::now();
            g_haveReply = true;
        }
        if (st.running) {
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(kDiscoveryPort);
            to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            sendto(g_sock, kDiscoverMessage, sizeof kDiscoverMessage - 1, 0, reinterpret_cast<sockaddr*>(&to),
                   sizeof to);
        }
    }
    if (st.running && g_haveReply && std::chrono::steady_clock::now() - g_lastReply <= kReplyTimeout)
        st.viewer = g_replyBusy ? ViewerState::Yes : ViewerState::No;

    const auto ip = static_cast<uint32_t>(gethostid());
    const auto* b = reinterpret_cast<const uint8_t*>(&ip);
    if (ip != 0 && !(b[0] == 127 || (b[3] == 127 && b[0] == 1))) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        st.ip = buf;
    }
    return st;
}

bool setAutostart(bool enabled) {
    if (!exists(kContentsDir)) return false;
    if (!enabled) return std::remove(kBootFlag) == 0 || !exists(kBootFlag);
    mkdir(kFlagsDir, 0777);
    FILE* f = std::fopen(kBootFlag, "w");
    if (!f) return false;
    std::fclose(f);
    return true;
}

bool launch() {
    if (!g_pmshell || !exists(kExefs)) return false;
    u64 pid = 0;
    if (g_pmdmnt && R_SUCCEEDED(pmdmntGetProcessId(&pid, kProgramId)) && pid != 0) return true;
    const NcmProgramLocation location{kProgramId, NcmStorageId_None, {}};
    return R_SUCCEEDED(pmshellLaunchProgram(0, &location, &pid));
}
}
