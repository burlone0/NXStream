#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sft::viewermsg {

    struct PadMsg {
        char magic[4];
        uint32_t flags;
        uint64_t buttons;
        int32_t lx, ly, rx, ry;
    };
    static_assert(sizeof(PadMsg) == 32, "PadMsg is 32 bytes on the wire");

    constexpr uint32_t PadOn = 1;

    constexpr uint64_t PadButtons = 0xFFFFull | (1ull << 18) | (1ull << 19);

    enum class Kind { Skip, Partial, Key, Crop, NoCrop, Lock, Unlock, Bye, Pad };

    struct Msg { Kind kind; size_t len; };

    inline Msg Next(const uint8_t *p, size_t n) {
        if (n < 4) { return { Kind::Partial, 0 }; }
        if (std::memcmp(p, "SFT", 3) != 0) { return { Kind::Skip, 1 }; }
        switch (p[3]) {
            case 'K': return { Kind::Key, 4 };
            case 'C': return { Kind::Crop, 4 };
            case 'D': return { Kind::NoCrop, 4 };
            case 'L': return { Kind::Lock, 4 };
            case 'U': return { Kind::Unlock, 4 };
            case 'Q': return { Kind::Bye, 4 };
            case 'I': return n < sizeof(PadMsg) ? Msg{ Kind::Partial, 0 } : Msg{ Kind::Pad, sizeof(PadMsg) };
            default:  return { Kind::Skip, 1 };
        }
    }

    inline PadMsg ReadPad(const uint8_t *p) {
        PadMsg m;
        std::memcpy(&m, p, sizeof(m));
        return m;
    }

    inline int32_t ClampStick(int32_t v) { return v < -32767 ? -32767 : v > 32767 ? 32767 : v; }

}
