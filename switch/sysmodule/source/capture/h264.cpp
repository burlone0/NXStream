#include "capture/h264.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "core/bytes.hpp"

namespace h264 {
namespace {
constexpr uint8_t kStartCode[] = {0, 0, 0, 1};
constexpr uint8_t kSavedMagic[4] = {'N', 'S', 'P', 'S'};
constexpr uint32_t kSavedVersion = 1;
constexpr size_t kSavedMaxNal = 256;

size_t startCodeAt(const uint8_t* p, size_t left) {
    if (left >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return 4;
    if (left >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) return 3;
    return 0;
}

template <typename Fn>
void forEachNal(const uint8_t* data, size_t size, Fn&& fn) {
    size_t i = 0;
    size_t nalStart = SIZE_MAX;
    while (i < size) {
        const size_t sc = startCodeAt(data + i, size - i);
        if (sc == 0) {
            ++i;
            continue;
        }
        if (nalStart != SIZE_MAX) fn(data + nalStart, i - nalStart);
        i += sc;
        nalStart = i;
    }
    if (nalStart != SIZE_MAX && nalStart < size) fn(data + nalStart, size - nalStart);
}
}

bool normalizeAnnexB(uint8_t* data, size_t size) {
    if (startCodeAt(data, size) != 0) return true;

    size_t p = 0;
    while (p + 4 <= size) {
        const uint32_t len = bytes::rd32be(data + p);
        if (len == 0 || len > size - p - 4) return false;
        p += 4 + len;
    }
    if (p != size) return false;

    for (p = 0; p < size;) {
        const uint32_t len = bytes::rd32be(data + p);
        for (int i = 0; i < 4; ++i) data[p + i] = kStartCode[i];
        p += 4 + len;
    }
    return true;
}

FrameInfo ParameterSets::scan(const uint8_t* data, size_t size) {
    FrameInfo info;
    forEachNal(data, size, [&](const uint8_t* nal, size_t len) {
        if (len == 0) return;
        switch (nal[0] & 0x1F) {
            case kNalIdr:
                info.idr = true;
                break;
            case kNalSps:
                info.hasSps = true;
                if (sps_.size() != len || !std::equal(nal, nal + len, sps_.begin())) {
                    sps_.assign(nal, nal + len);
                    info.changed = true;
                    dirty_ = true;
                }
                break;
            case kNalPps:
                info.hasPps = true;
                if (pps_.size() != len || !std::equal(nal, nal + len, pps_.begin())) {
                    pps_.assign(nal, nal + len);
                    info.changed = true;
                    dirty_ = true;
                }
                break;
            default:
                break;
        }
    });
    return info;
}

const std::vector<uint8_t>& ParameterSets::annexB() {
    if (dirty_) {
        combined_.clear();
        combined_.insert(combined_.end(), kStartCode, kStartCode + 4);
        combined_.insert(combined_.end(), sps_.begin(), sps_.end());
        combined_.insert(combined_.end(), kStartCode, kStartCode + 4);
        combined_.insert(combined_.end(), pps_.begin(), pps_.end());
        dirty_ = false;
    }
    return combined_;
}

void ParameterSets::seed(std::vector<uint8_t> sps, std::vector<uint8_t> pps) {
    sps_ = std::move(sps);
    pps_ = std::move(pps);
    dirty_ = true;
}

std::vector<uint8_t> encodeSaved(uint32_t fps, uint32_t bitrate, const std::vector<uint8_t>& sps,
                                 const std::vector<uint8_t>& pps) {
    std::vector<uint8_t> out(24 + sps.size() + pps.size());
    uint8_t* p = out.data();
    std::memcpy(p, kSavedMagic, 4);
    bytes::wr32(p + 4, kSavedVersion);
    bytes::wr32(p + 8, fps);
    bytes::wr32(p + 12, bitrate);
    bytes::wr32(p + 16, static_cast<uint32_t>(sps.size()));
    std::memcpy(p + 20, sps.data(), sps.size());
    bytes::wr32(p + 20 + sps.size(), static_cast<uint32_t>(pps.size()));
    std::memcpy(p + 24 + sps.size(), pps.data(), pps.size());
    return out;
}

const char* decodeSaved(const uint8_t* data, size_t size, uint32_t fps, uint32_t bitrate, std::vector<uint8_t>& sps,
                        std::vector<uint8_t>& pps) {
    if (size < 24 || std::memcmp(data, kSavedMagic, 4) != 0) return "bad magic or size";
    if (bytes::rd32(data + 4) != kSavedVersion) return "unknown version";
    if (bytes::rd32(data + 8) != fps || bytes::rd32(data + 12) != bitrate) return "recorder fps/bitrate differ";
    const uint32_t spsLen = bytes::rd32(data + 16);
    if (spsLen == 0 || spsLen > kSavedMaxNal || size < 24 + size_t(spsLen)) return "truncated SPS";
    const uint32_t ppsLen = bytes::rd32(data + 20 + spsLen);
    if (ppsLen == 0 || ppsLen > kSavedMaxNal || size != 24 + size_t(spsLen) + ppsLen) return "truncated PPS";
    sps.assign(data + 20, data + 20 + spsLen);
    pps.assign(data + 24 + spsLen, data + 24 + spsLen + ppsLen);
    return nullptr;
}
}
