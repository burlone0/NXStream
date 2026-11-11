#pragma once

#include <atomic>
#include <cstdint>

namespace settings {
inline constexpr const char* kPath = "sdmc:/config/nxstream/config.ini";

enum class Transport : uint8_t { Network, Usb };

inline std::atomic<Transport> transport{Transport::Network};
inline const char* transportName(Transport t) { return t == Transport::Usb ? "usb" : "network"; }

inline std::atomic<bool> enabled{true};
inline std::atomic<bool> gameCapture{true};
inline std::atomic<bool> audio{true};
inline std::atomic<bool> screenCapture{true};
inline std::atomic<uint32_t> recorderFps{30};
inline std::atomic<uint32_t> recorderBitrate{5000000};
inline std::atomic<uint32_t> screenFps{0};
inline std::atomic<uint32_t> screenKbps{0};
inline std::atomic<uint32_t> probeRequest{0};

bool nativeCapture();
uint32_t screenFpsLimit();
uint32_t screenBytesPerSecond();

void reloadIfChanged();
}
