#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace appinfo {
inline constexpr size_t kMaxName = 128;
inline constexpr size_t kHeaderBytes = 16;
inline constexpr uint8_t kVersion = 1;

constexpr uint64_t baseTid(uint64_t tid) {
    return tid & ~uint64_t(0xFFF);
}

inline size_t clampUtf8(const char* s, size_t maxLen, size_t limit) {
    size_t len = 0;
    while (len < maxLen && s[len] != '\0') ++len;
    if (len <= limit) return len;
    size_t n = limit;
    while (n > 0 && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) --n;
    return n;
}

inline size_t pickName(const char names[16][0x200]) {
    if (names[0][0] != '\0') return 0;
    if (names[1][0] != '\0') return 1;
    for (size_t i = 2; i < 16; ++i)
        if (names[i][0] != '\0') return i;
    return 16;
}

inline size_t encode(uint8_t* out, size_t cap, uint8_t state, uint64_t programId, uint32_t elapsedSec,
                     const char* name, size_t nameLen) {
    const size_t len = name ? clampUtf8(name, nameLen, kMaxName) : 0;
    if (cap < kHeaderBytes + len) return 0;
    out[0] = kVersion;
    out[1] = state;
    out[2] = static_cast<uint8_t>(len);
    out[3] = static_cast<uint8_t>(len >> 8);
    for (int i = 0; i < 8; ++i) out[4 + i] = static_cast<uint8_t>(programId >> (8 * i));
    for (int i = 0; i < 4; ++i) out[12 + i] = static_cast<uint8_t>(elapsedSec >> (8 * i));
    if (len) std::memcpy(out + kHeaderBytes, name, len);
    return kHeaderBytes + len;
}
}
