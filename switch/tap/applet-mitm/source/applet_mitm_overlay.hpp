#pragma once
#include <stratosphere.hpp>
#include <atomic>

namespace ams::mitm::applet {

    constexpr u64 TeslaProgramId = UINT64_C(0x420000000007E51A);
    extern std::atomic<bool> g_ovl_vi_mitm;
    extern std::atomic<u32> g_tesla_txns, g_tesla_presents;
    void OvlNoteTeslaTransaction(u32 code, const void *in, size_t in_size);

    void OverlayArm(bool on);
    bool OverlaysArmed();

    void OverlayAreaUp(u8 *cpu, u32 dev, size_t size, u8 *cached, size_t cached_size);
    void OverlayAreaDown();
    void OverlaySetCalibrated(bool ok);

    struct OverlayPlane {
        u32 dev;
        u32 w, h;
        u32 fmt;
        u32 x, y;
        u32 dw, dh;
    };
    u32 OverlayPlanes(OverlayPlane *out, u32 max);

}
