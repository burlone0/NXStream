#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    struct StreamStatus {
        u32 version;
        u32 state;
        u32 enabled;
        u32 width, height;
        u32 fps_x10;
        u32 game_fps_x10;
        u32 sessions;
        u32 shot_enabled;
        u32 shots;
        u32 shot_fails;
        u32 reserved;
        u64 app_tid;
        u32 app_excluded;
        u32 reserved3;
    };
    static_assert(sizeof(StreamStatus) == 64);

    static constexpr u32 kReserved3NeedHeadroom = 64u;

    void LoadStreamEnabled();
    void ReloadReleaseConfig();
    void StartControlService();
    void LoadExcluded();
    void SetExcluded(u64 program_id, bool excluded);
    bool IsExcluded(u64 program_id);

    void HeadroomAskAndWait(u32 need_mb, u32 timeout_ms);
    void HeadroomClear();
    bool HeadroomNeeded();
    u32 HeadroomNeedMb();
    void HeadroomAck(u32 free_kb);

}

#define AMS_NXTAP_CONTROL_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, GetStatus,  (sf::Out<ams::mitm::applet::StreamStatus> out), (out)) \
    AMS_SF_METHOD_INFO(C, H, 1, Result, SetEnabled, (u8 on), (on)) \
    AMS_SF_METHOD_INFO(C, H, 2, Result, ReloadConfig, (), ()) \
    AMS_SF_METHOD_INFO(C, H, 3, Result, SetAppExcluded, (u64 program_id, u8 excluded), (program_id, excluded)) \
    AMS_SF_METHOD_INFO(C, H, 4, Result, SetScreenOff, (u8 on), (on)) \
    AMS_SF_METHOD_INFO(C, H, 10, Result, Donate, (u64 size, sf::CopyHandle &&tmem), (size, std::move(tmem))) \
    AMS_SF_METHOD_INFO(C, H, 11, Result, DonateFailed, (u32 rc), (rc)) \
    AMS_SF_METHOD_INFO(C, H, 12, Result, Donate2, (u64 size, u64 nv_size, sf::CopyHandle &&tmem, sf::CopyHandle &&nv_tmem), (size, nv_size, std::move(tmem), std::move(nv_tmem))) \
    AMS_SF_METHOD_INFO(C, H, 13, Result, GetDonorCap, (sf::Out<u32> out_mb), (out_mb)) \
    AMS_SF_METHOD_INFO(C, H, 15, Result, AckHeadroom, (u32 free_kb), (free_kb))

AMS_SF_DEFINE_INTERFACE(ams::mitm::applet, IStreamControl, AMS_NXTAP_CONTROL_INTERFACE_INFO, 0x5F7A4001)
