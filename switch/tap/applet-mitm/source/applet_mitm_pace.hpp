#pragma once
#include <cstdint>
#include <initializer_list>

namespace pace {

    constexpr std::uint64_t FrameNs = 16666667;
    constexpr std::uint32_t MaxDivisor = 4;

    inline std::uint32_t MinDivisor(std::uint32_t max_fps) {
        if (max_fps == 0) { return 1; }
        std::uint32_t d = (60 + max_fps - 1) / max_fps;
        if (d < 1) { d = 1; }
        if (d > MaxDivisor) { d = MaxDivisor; }
        return d;
    }

    inline std::uint64_t IntervalNs(std::uint32_t divisor) {
        return static_cast<std::uint64_t>(divisor) * FrameNs;
    }

    inline std::uint32_t Divisor(std::uint32_t max_fps, std::uint64_t work_ns, std::uint32_t current) {
        const std::uint32_t lo = MinDivisor(max_fps);
        std::uint32_t candidate = MaxDivisor;
        for (std::uint32_t d = lo; d <= MaxDivisor; ++d) {
            if (work_ns + 2000000ULL <= IntervalNs(d)) { candidate = d; break; }
        }
        if (current < lo || candidate >= current) { return candidate; }
        if (work_ns + 5000000ULL <= IntervalNs(candidate)) { return candidate; }
        return current < MaxDivisor ? current : MaxDivisor;
    }

}
