#pragma once

#include <cstddef>
#include <cstdint>

#include "core/bytes.hpp"

namespace proto {
inline constexpr uint16_t kTcpPort = 48500;
inline constexpr uint16_t kDiscoveryPort = 48501;
inline constexpr uint16_t kBeaconPort = 48502;
inline constexpr uint32_t kVersion = 2;

inline constexpr uint16_t kUsbVendorId = 0x18D1;
inline constexpr uint16_t kUsbProductId = 0x4EE0;
inline constexpr uint16_t kUsbBcdDevice = 0x0100;
inline constexpr char kUsbManufacturer[] = "NXStream";
inline constexpr char kUsbProduct[] = "NXStream";
inline constexpr char kUsbSerial[] = "NXStream";
inline constexpr uint8_t kUsbEndpointIn = 0x81;
inline constexpr uint8_t kUsbEndpointOut = 0x01;

inline constexpr char kDiscoverMagic[] = "NSTREAM_DISCOVER_V2";
inline constexpr char kAnnounceMagic[] = "NSTREAM_HERE_V2";

enum class MsgType : uint8_t {
    Hello           = 0x01,
    Ping            = 0x06,
    Bye             = 0x07,
    KeyframeRequest = 0x08,
    ProbeAck        = 0x09,

    Welcome   = 0x80,
    Pong      = 0x81,
    VideoH264 = 0x83,
    VideoJpeg = 0x84,
    Audio     = 0x85,
    Probe     = 0x87,
    AppInfo   = 0x88,
};

inline constexpr uint8_t kFlagKeyframe = 0x01;
inline constexpr uint8_t kFlagProbeReply = 0x02;

inline constexpr size_t kHeaderSize = 8;

inline void encodeHeader(uint8_t* out, MsgType type, uint8_t flags, uint32_t size) {
    out[0] = static_cast<uint8_t>(type);
    out[1] = flags;
    bytes::wr16(out + 2, 0);
    bytes::wr32(out + 4, size);
}

struct Header {
    MsgType type;
    uint8_t flags;
    uint32_t size;
};

inline Header decodeHeader(const uint8_t* in) {
    return Header{static_cast<MsgType>(in[0]), in[1], bytes::rd32(in + 4)};
}
}
