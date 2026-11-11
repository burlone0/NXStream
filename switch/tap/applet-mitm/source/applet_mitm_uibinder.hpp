#pragma once
#include <stratosphere.hpp>
#include <atomic>

#ifndef SFT_UI_PROBE
#define SFT_UI_PROBE 0
#endif

#define AMS_UI_BINDER_MITM_INTERFACE_INFO(C, H)                                                                                                                                                                      \
    AMS_SF_METHOD_INFO(C, H, 0, Result, TransactParcel,     (s32 binder_id, u32 code, u32 flags, const sf::InBuffer &parcel_in, const sf::OutBuffer &parcel_out), (binder_id, code, flags, parcel_in, parcel_out))            \
    AMS_SF_METHOD_INFO(C, H, 1, Result, AdjustRefcount,     (s32 binder_id, s32 add, s32 type), (binder_id, add, type))                                                                                                     \
    AMS_SF_METHOD_INFO(C, H, 2, Result, GetNativeHandle,    (s32 binder_id, u32 what), (binder_id, what))                                                                                                                   \
    AMS_SF_METHOD_INFO(C, H, 3, Result, TransactParcelAuto, (s32 binder_id, u32 code, u32 flags, const sf::InAutoSelectBuffer &parcel_in, const sf::OutAutoSelectBuffer &parcel_out), (binder_id, code, flags, parcel_in, parcel_out))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IUiBinderMitm, AMS_UI_BINDER_MITM_INTERFACE_INFO, 0x2AB1E145)

namespace ams::mitm::applet {
    struct SharedAcquireOut {
        u8  fence[0x24];
        s32 slot_indexes[4];
        u32 pad;
        u64 slot;
    };
    static_assert(sizeof(SharedAcquireOut) == 0x40);
    struct SharedPresentIn {
        u8  fence[0x24];
        s32 crop[4];
        u32 transform;
        s32 swap_interval;
        u32 pad;
        u64 layer;
        u64 slot;
    };
    static_assert(sizeof(SharedPresentIn) == 0x50);
    static_assert(__builtin_offsetof(SharedPresentIn, layer) == 0x40);
}

#define AMS_UI_SYSDISP_MITM_INTERFACE_INFO(C, H)                                                                                                                   \
    AMS_SF_METHOD_INFO(C, H, 8251, Result, CloseSharedLayer,                    (u64 layer), (layer))                                                            \
    AMS_SF_METHOD_INFO(C, H, 8252, Result, ConnectSharedLayer,                  (u64 layer), (layer))                                                            \
    AMS_SF_METHOD_INFO(C, H, 8253, Result, DisconnectSharedLayer,               (u64 layer), (layer))                                                            \
    AMS_SF_METHOD_INFO(C, H, 8254, Result, AcquireSharedFrameBuffer,            (sf::Out<ams::mitm::applet::SharedAcquireOut> out, u64 layer), (out, layer))   \
    AMS_SF_METHOD_INFO(C, H, 8255, Result, PresentSharedFrameBuffer,            (const ams::mitm::applet::SharedPresentIn &in), (in))                           \
    AMS_SF_METHOD_INFO(C, H, 8256, Result, GetSharedFrameBufferAcquirableEvent, (u64 layer), (layer))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IUiSysDisplayMitm, AMS_UI_SYSDISP_MITM_INTERFACE_INFO, 0x2AB1E146)

namespace ams::mitm::applet {

    inline bool IsUiProgram(u64 tid) { return tid >= UINT64_C(0x0100000000001000) && tid < UINT64_C(0x0100000000002000); }

    inline bool IsLibraryApplet(u64 tid) {
        return IsUiProgram(tid) && tid != UINT64_C(0x0100000000001000) && tid != UINT64_C(0x010000000000100C) && tid != UINT64_C(0x010000000000100D);
    }

    extern std::atomic<bool> g_ui_vi_mitm;

    class UiSysDisplayMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &) { return true; }
        public:
            Result CloseSharedLayer(u64 layer);
            Result ConnectSharedLayer(u64 layer);
            Result DisconnectSharedLayer(u64 layer);
            Result AcquireSharedFrameBuffer(sf::Out<SharedAcquireOut> out, u64 layer);
            Result PresentSharedFrameBuffer(const SharedPresentIn &in);
            Result GetSharedFrameBufferAcquirableEvent(u64 layer);
    };
    static_assert(IsIUiSysDisplayMitm<UiSysDisplayMitm>);

    struct SharedPresentState {
        std::atomic<u32> count;
        std::atomic<s32> slot;
        std::atomic<u64> layer;
        std::atomic<u64> tick;
        std::atomic<u32> acquires;
        std::atomic<s32> acquired_slot;
    };
    extern SharedPresentState g_shared_present;

    void UiNoteTransaction(u64 tid, s32 id, u32 code, const void *in, size_t in_size);

    extern std::atomic<bool> g_ui_dispdrv_probe;
    extern bool g_ui_stream_armed;

    class UiBinderMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info) {
                const u64 tid = client_info.program_id.value;
                if (g_ui_dispdrv_probe.load(std::memory_order_relaxed) && IsUiProgram(tid)) { return true; }
                return g_ui_stream_armed && tid == UINT64_C(0x010000000000100D) && !client_info.override_status.IsHbl();
            }
        public:
            Result TransactParcel(s32 binder_id, u32 code, u32 flags, const sf::InBuffer &parcel_in, const sf::OutBuffer &parcel_out);
            Result AdjustRefcount(s32 binder_id, s32 add, s32 type);
            Result GetNativeHandle(s32 binder_id, u32 what);
            Result TransactParcelAuto(s32 binder_id, u32 code, u32 flags, const sf::InAutoSelectBuffer &parcel_in, const sf::OutAutoSelectBuffer &parcel_out);
        private:
            Result Transact(u32 cmd, s32 binder_id, u32 code, u32 flags, const void *in, size_t in_size, void *out, size_t out_size);
    };
    static_assert(IsIUiBinderMitm<UiBinderMitm>);

    bool UiProbeOn();
    bool UiProbeWants(const char *word);

    void UiProbeSetDispdrv(const ::Service &s);

    struct UiBinderInfo {
        s32 id;
        u64 tid;
        u32 txns, queues, dequeues, requests;
        s32 last_slot;
        u32 slots_seen;
        s32 crop[4];
        u32 transform;
        u64 last_queue_tick;
    };
    u32 UiBinderSnapshot(UiBinderInfo *out, u32 max);

    s32 ParseParcelSlot(const u8 *p, size_t sz);
    bool ParseParcelQueueInput(const u8 *p, size_t sz, u32 *n, u64 out[4], u32 *transform, s32 crop[4]);

}
