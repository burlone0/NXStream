#pragma once
#include <atomic>
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    extern bool g_clk_armed;

    constexpr u32 PcvModule_VIC   = 0x4000002F;
    constexpr u32 PcvModule_NVENC = 0x40000030;
    constexpr u32 PcvModule_NVJPG = 0x40000031;
    constexpr u32 PcvModule_NVDEC = 0x40000032;

    extern std::atomic<bool> g_memory_boost;
    void MemoryBoost(bool on);

    bool ClockRateOf(u32 pcv_module, u32 *out_hz);

    void ClockSurvey(const char *label);

    bool ClocksHoldForEngines(const char *who);

    u32 ClockEnsure(u32 pcv_module, const char *who);

    void ClockWatchStop();

    void StartClockProbe(bool keep_holding);

}
