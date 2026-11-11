#pragma once
#include <cstdint>

namespace ams::mitm::applet::keyframe {

    constexpr uint32_t UsbGopDefault = 240;
    constexpr uint32_t UsbGopMax = 250;

    constexpr uint32_t Choose(bool net, bool live, bool uvc, uint32_t gop, bool viewer_can_ask, bool wired, uint32_t usb_gop) {
        if (gop == 0) { return 0; }
        if (net) { return viewer_can_ask ? (wired ? 60u : 240u) : gop; }
        const uint32_t u = usb_gop > UsbGopMax ? UsbGopMax : usb_gop;
        return (live && !uvc && viewer_can_ask && u > gop) ? u : gop;
    }

}
