#include <switch.h>

#include <string>

#include "capture/game_capture.hpp"
#include "capture/screen_capture.hpp"
#include "capture/system_audio.hpp"
#include "core/app_watch.hpp"
#include "core/config.hpp"
#include "core/power.hpp"
#include "core/system.hpp"
#include "net/discovery.hpp"
#include "net/link_probe.hpp"
#include "net/viewer_link.hpp"
#include "protocol.hpp"

#ifndef NXSTREAM_VERSION
#define NXSTREAM_VERSION "dev"
#endif
#ifndef NXSTREAM_CONTENT_ID
#define NXSTREAM_CONTENT_ID "dev"
#endif

extern "C" {
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

constexpr size_t kHeapSize = 0x200000;

void __libnx_initheap(void) {
    alignas(0x1000) static char heap[kHeapSize];
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    fake_heap_start = heap;
    fake_heap_end = heap + sizeof heap;
}

void __appInit(void) {
    if (R_FAILED(smInitialize())) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    if (R_FAILED(fsInitialize())) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
    fsdevMountSdmc();
}

void __appExit(void) {
    fsdevUnmountAll();
    fsExit();
    smExit();
}
}

namespace {
constexpr uint64_t kStatusEveryMs = 30000;

SocketInitConfig socketConfig() {
    SocketInitConfig cfg = *socketGetDefaultInitConfig();
    cfg.tcp_tx_buf_size = 0x20000;
    cfg.tcp_tx_buf_max_size = 0x40000;
    cfg.tcp_rx_buf_size = 0x2000;
    cfg.tcp_rx_buf_max_size = 0;
    cfg.udp_tx_buf_size = 0x2400;
    cfg.udp_rx_buf_size = 0x2400;
    cfg.sb_efficiency = 2;
    cfg.num_bsd_sessions = 4;
    return cfg;
}

std::string consoleName() {
    std::string name = "Nintendo Switch";
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysDeviceNickName nick{};
        if (R_SUCCEEDED(setsysGetDeviceNickname(&nick)) && nick.nickname[0] != '\0') name = nick.nickname;
        setsysExit();
    }
    return name;
}

const char* hardwareName(u64 type) {
    switch (type) {
        case 0: return "Icosa (Erista, original)";
        case 1: return "Copper";
        case 2: return "Hoag (Lite)";
        case 3: return "Iowa (Mariko)";
        case 4: return "Calcio";
        case 5: return "Aula (OLED)";
        default: return "unknown";
    }
}

void logPlatform() {
    LOG("NXStream sysmodule %s (built " __DATE__ " " __TIME__ "), title id 0x%016llx", NXSTREAM_VERSION,
        0x4200000000004E53ull);
    LOG("content id %s", NXSTREAM_CONTENT_ID);
    LOG("firmware %u.%u.%u, uptime at start %llu ms", (hosversionGet() >> 16) & 0xFF, (hosversionGet() >> 8) & 0xFF,
        hosversionGet() & 0xFF, static_cast<unsigned long long>(sys::nowMs()));
    if (R_SUCCEEDED(splInitialize())) {
        u64 hw = 0, ams = 0, emummc = 0, hash = 0;
        if (R_SUCCEEDED(splGetConfig(SplConfigItem_HardwareType, &hw)))
            LOG("hardware: %s (type %u)", hardwareName(hw), static_cast<unsigned>(hw));
        if (R_SUCCEEDED(splGetConfig(static_cast<SplConfigItem>(65000), &ams)))
            LOG("atmosphere %u.%u.%u, target firmware %u.%u.%u", static_cast<unsigned>((ams >> 56) & 0xFF),
                static_cast<unsigned>((ams >> 48) & 0xFF), static_cast<unsigned>((ams >> 40) & 0xFF),
                static_cast<unsigned>((ams >> 24) & 0xFF), static_cast<unsigned>((ams >> 16) & 0xFF),
                static_cast<unsigned>((ams >> 8) & 0xFF));
        if (R_SUCCEEDED(splGetConfig(static_cast<SplConfigItem>(65003), &hash)))
            LOG("atmosphere commit %08llx", static_cast<unsigned long long>(hash));
        if (R_SUCCEEDED(splGetConfig(static_cast<SplConfigItem>(65007), &emummc)))
            LOG("emummc: %s", emummc ? "yes" : "no (sysmmc)");
        splExit();
    }
}

void logRecorderSettings() {
    if (R_FAILED(setsysInitialize())) return;
    const char* keys[] = {"continuous_recording_fps", "continuous_recording_video_bit_rate",
                          "continuous_recording_key_frame_count"};
    u32 values[3] = {};
    for (int i = 0; i < 3; ++i) {
        u64 size = 0;
        const Result rc = setsysGetSettingsItemValue("am.debug", keys[i], &values[i], sizeof values[i], &size);
        if (R_FAILED(rc)) LOG("recorder setting am.debug!%s unreadable (0x%x)", keys[i], rc);
        else if (values[i] == 0) LOG("recorder setting am.debug!%s = firmware default", keys[i]);
        else LOG("recorder setting am.debug!%s = %u", keys[i], values[i]);
    }
    setsysExit();
    if (values[0]) settings::recorderFps = values[0];
    if (values[1]) settings::recorderBitrate = values[1];
    LOG("recorder: %u fps, %u kbit/s", static_cast<unsigned>(settings::recorderFps.load()),
        static_cast<unsigned>(settings::recorderBitrate.load() / 1000));
}

u32 clockMhz(PcvModuleId module) {
    ClkrstSession session;
    if (R_FAILED(clkrstOpenSession(&session, module, 3))) return 0;
    u32 hz = 0;
    clkrstGetClockRate(&session, &hz);
    clkrstCloseSession(&session);
    return hz / 1000000;
}

int temperature(u32 location) {
    TsSession session;
    if (R_FAILED(tsOpenSession(&session, location))) return -1;
    float c = -1;
    tsSessionGetTemperature(&session, &c);
    tsSessionClose(&session);
    return static_cast<int>(c);
}

void logStatus(ViewerLink& link) {
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);

    static bool clkrst = R_SUCCEEDED(clkrstInitialize());
    static bool psm = R_SUCCEEDED(psmInitialize());
    static bool apm = R_SUCCEEDED(apmInitialize());
    static bool ts = R_SUCCEEDED(tsInitialize());

    u32 battery = 0;
    PsmChargerType charger = PsmChargerType_Unconnected;
    if (psm) {
        psmGetBatteryChargePercentage(&battery);
        psmGetChargerType(&charger);
    }
    ApmPerformanceMode mode = ApmPerformanceMode_Invalid;
    if (apm) apmGetPerformanceMode(&mode);

    LOG("status: mem %llu/%llu KB | %s | cpu %u MHz gpu %u MHz emc %u MHz | soc %d C pcb %d C | "
        "battery %u%% charger %d | viewer %s | link %s",
        static_cast<unsigned long long>(used / 1024), static_cast<unsigned long long>(total / 1024),
        mode == ApmPerformanceMode_Boost ? "docked" : mode == ApmPerformanceMode_Normal ? "handheld" : "mode ?",
        clkrst ? clockMhz(PcvModuleId_CpuBus) : 0, clkrst ? clockMhz(PcvModuleId_GPU) : 0,
        clkrst ? clockMhz(PcvModuleId_EMC) : 0, ts ? temperature(TsDeviceCode_LocationExternal) : -1,
        ts ? temperature(TsDeviceCode_LocationInternal) : -1, battery, static_cast<int>(charger),
        link.connected() ? "yes" : "no", settings::transportName(settings::transport.load()));
}
}

int main(int, char**) {
    sys::logOpen();
    logPlatform();

    if (sys::nowMs() < 60000) {
        LOG("early boot: waiting 15 s for HOME and the network");
        sys::sleepMs(15000);
    }

    SocketInitConfig cfg = socketConfig();
    uint32_t attempts = 1;
    while (R_FAILED(socketInitialize(&cfg))) {
        LOG("socketInitialize failed (attempt %u), retrying in 5 s", static_cast<unsigned>(attempts++));
        sys::sleepMs(5000);
    }

    logRecorderSettings();
    settings::reloadIfChanged();

    const bool native = settings::nativeCapture();
    static ViewerLink link;
    static SystemAudio systemAudio(link);
    static GameCapture game(link, systemAudio);
    static ScreenCapture screen(link, game);
    static DiscoveryResponder discovery(link);

    const std::string name = consoleName();

    power::start();
    discovery.start(proto::kDiscoveryPort, proto::kTcpPort, name);
    if (native) {
        LOG("capture: native module selected, NXStream capture idle");
        while (true) {
            sys::sleepMs(kStatusEveryMs);
            logStatus(link);
        }
    }
    link.start(proto::kTcpPort, name);
    systemAudio.start();
    game.start();
    screen.start();
    linkprobe::start(link);
    appwatch::start(link, screen);

    while (true) {
        sys::sleepMs(kStatusEveryMs);
        logStatus(link);
    }
}
