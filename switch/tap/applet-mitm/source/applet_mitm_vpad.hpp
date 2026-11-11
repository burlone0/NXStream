#pragma once
#include <stratosphere.hpp>
#include "applet_mitm_viewermsg.hpp"

namespace ams::mitm::applet {

    extern std::atomic<bool> g_vpad_enabled;

    constexpr u16 HdrFlagInput = 128;
    inline u16 VPadHdrFlag() { return g_vpad_enabled.load(std::memory_order_relaxed) ? HdrFlagInput : 0; }

    void VPadFromViewer(const sft::viewermsg::PadMsg &m);
    void VPadTick();
    void VPadRelease(const char *why);

}
