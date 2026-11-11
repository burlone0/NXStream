#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    extern bool g_nvjpg_armed;
    extern u32  g_nvjpg_quality;
    extern u32  g_nvjpg_attempts;

    struct NvjpgLayout {
        u32 param_off, status_off, bits_off, bits_size;
        u32 luma_off, chroma_off, w, h, quality;
    };

    void NvjpgFillParams(u8 *base, const NvjpgLayout &L, u32 memory_mode, u32 input_type);

    u32 NvjpgBuildCmdbuf(u32 *w, u32 param_addr, u32 status_addr,
                         u32 bits_addr, u32 luma_addr, u32 chroma_u_addr, u32 chroma_v_addr);

    u32 NvjpgWriteContainer(u8 *dst, u32 w, u32 h, u32 quality, const u8 *scan, u32 scan_len);

}
