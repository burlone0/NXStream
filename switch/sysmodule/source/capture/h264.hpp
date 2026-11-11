#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace h264 {
enum NalType : uint8_t { kNalIdr = 5, kNalSps = 7, kNalPps = 8 };

bool normalizeAnnexB(uint8_t* data, size_t size);

struct FrameInfo {
    bool idr = false;
    bool hasSps = false;
    bool hasPps = false;
    bool changed = false;
};

std::vector<uint8_t> encodeSaved(uint32_t fps, uint32_t bitrate, const std::vector<uint8_t>& sps,
                                 const std::vector<uint8_t>& pps);
const char* decodeSaved(const uint8_t* data, size_t size, uint32_t fps, uint32_t bitrate, std::vector<uint8_t>& sps,
                        std::vector<uint8_t>& pps);

class ParameterSets {
public:
    FrameInfo scan(const uint8_t* data, size_t size);
    bool ready() const { return !sps_.empty() && !pps_.empty(); }
    const std::vector<uint8_t>& annexB();
    const std::vector<uint8_t>& sps() const { return sps_; }
    const std::vector<uint8_t>& pps() const { return pps_; }
    void seed(std::vector<uint8_t> sps, std::vector<uint8_t> pps);

private:
    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;
    std::vector<uint8_t> combined_;
    bool dirty_ = true;
};
}
