#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    struct NvNativeHandle {
        u32 unk0;
        s32 num_fds;
        s32 num_ints;
    };
    static_assert(sizeof(NvNativeHandle) == 0xC);

    struct NvSurfaceRaw {
        u32 width;
        u32 height;
        u64 color_format;
        u32 layout;
        u32 pitch;
        u32 unused_nvmap;
        u32 offset;
        u32 kind;
        u32 block_height_log2;
        u32 scan;
        u32 second_field_offset;
        u64 flags;
        u64 size;
        u32 unk[6];
    };
    static_assert(sizeof(NvSurfaceRaw) == 0x58);
    static_assert(__builtin_offsetof(NvSurfaceRaw, layout) == 0x10);
    static_assert(__builtin_offsetof(NvSurfaceRaw, offset) == 0x1C);
    static_assert(__builtin_offsetof(NvSurfaceRaw, kind)   == 0x20);
    static_assert(__builtin_offsetof(NvSurfaceRaw, size)   == 0x38);

    struct NvGraphicBufferRaw {
        NvNativeHandle header;
        s32 unk0;
        s32 nvmap_id;
        u32 unk2;
        u32 magic;
        u32 pid;
        u32 type;
        u32 usage;
        u32 format;
        u32 ext_format;
        u32 stride;
        u32 total_size;
        u32 num_planes;
        u32 unk12;
        NvSurfaceRaw planes[3];
        u64 unused;
    };
    static_assert(sizeof(NvGraphicBufferRaw) == 0x150);
    static_assert(__builtin_offsetof(NvGraphicBufferRaw, nvmap_id) == 0x10);
    static_assert(__builtin_offsetof(NvGraphicBufferRaw, magic)    == 0x18);
    static_assert(__builtin_offsetof(NvGraphicBufferRaw, planes)   == 0x40);

    constexpr u32 NvGraphicBufferMagic = 0xDAFFCAFF;

    const NvGraphicBufferRaw *FindGraphicBuffer(const void *parcel, size_t size);

    void LogGraphicBuffer(const char *tag, const NvGraphicBufferRaw *gb);

}
