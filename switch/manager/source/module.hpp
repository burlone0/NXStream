#pragma once

#include <cstdint>
#include <string>

namespace module {
inline constexpr uint64_t kProgramId = 0x4200000000004E53;

struct Settings {
    bool enabled = true;
    bool gameCapture = true;
    bool audio = true;
    bool screenCapture = true;
    int screenFps = 0;
    int screenKbps = 0;
    bool usb = false;
    bool native = false;

    static Settings load();
    bool save() const;
};

struct ProbeResult {
    uint32_t id = 0;
    std::string state;
    std::string reason;
    int step = 0;
    int run = 0;
    int pick = -1;
    int capacityKbps = 0;
    int sendP99 = -1;
    int ackP95 = -1;
    bool valid = false;
};

inline constexpr uint64_t kTapProgramId = 0x4200000000004E54;
bool nativeFilesPresent();
bool refreshTapConfig(bool usb, bool& changed);
bool setNative(Settings& settings, bool native);
inline constexpr uint32_t kUsbBitrate = 30000000;
bool setTransport(Settings& settings, bool usb, uint32_t currentBitrate, uint32_t& newBitrate);
bool requestProbe(const Settings& settings, uint32_t& id);
const char* lastWriteError();
ProbeResult readProbeResult();

enum class ViewerState { Unknown, No, Yes };

struct Status {
    bool installed = false;
    bool running = false;
    bool autostart = false;
    bool nativeFlags = false;
    bool nativeRunning = false;
    ViewerState viewer = ViewerState::Unknown;
    std::string ip;
};

inline constexpr const char* kBitrateKey = "continuous_recording_video_bit_rate";
inline constexpr const char* kFpsKey = "continuous_recording_fps";
inline constexpr uint32_t kDefaultBitrate = 5000000;
inline constexpr uint32_t kDefaultFps = 30;
inline constexpr const char* kKeyframeKey = "continuous_recording_key_frame_count";
inline constexpr uint32_t kDefaultKeyframes = 15;

uint32_t recorderSetting(const char* key, uint32_t fallback);
bool setRecorderSettings(uint32_t bitrate, uint32_t fps, uint32_t keyframes);

bool init();
void exit();

Status query();
bool setAutostart(bool enabled);
bool launch();
}
