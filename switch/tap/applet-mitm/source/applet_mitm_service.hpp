#pragma once
#include <stratosphere.hpp>
#include "applet_mitm_uibinder.hpp"
#include "applet_mitm_uisource.hpp"
#include "applet_mitm_overlay.hpp"

namespace ams::mitm::applet {
    struct DisplayName { char data[0x40]; };
    static_assert(sizeof(DisplayName) == 0x40);
}

#define AMS_VI_BINDER_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, TransactParcel,     (s32 session_id, u32 code, u32 flags, const sf::InBuffer &parcel_in, const sf::OutBuffer &parcel_out), (session_id, code, flags, parcel_in, parcel_out)) \
    AMS_SF_METHOD_INFO(C, H, 3, Result, TransactParcelAuto, (s32 session_id, u32 code, u32 flags, const sf::InAutoSelectBuffer &parcel_in, const sf::OutAutoSelectBuffer &parcel_out), (session_id, code, flags, parcel_in, parcel_out))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IBinderMitm, AMS_VI_BINDER_MITM_INTERFACE_INFO, 0x2AB1E141)

#define AMS_VI_DISPLAYSVC_MITM_INTERFACE_INFO(C, H)                                                                                                                            \
    AMS_SF_METHOD_INFO(C, H, 100,  Result, GetRelayService, (sf::Out<sf::SharedPointer<ams::mitm::applet::IBinderMitm>> out), (out))                                            \
    AMS_SF_METHOD_INFO(C, H, 101,  Result, GetSystemDisplayService, (sf::Out<sf::SharedPointer<ams::mitm::applet::IUiSysDisplayMitm>> out), (out))                              \
    AMS_SF_METHOD_INFO(C, H, 1010, Result, OpenDisplay,     (const ams::mitm::applet::DisplayName &display_name, sf::Out<u64> out_display_id), (display_name, out_display_id))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IViDisplaySvcMitm, AMS_VI_DISPLAYSVC_MITM_INTERFACE_INFO, 0x2AB1E142)

#define AMS_VI_ROOT_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, GetDisplayService, (sf::Out<sf::SharedPointer<ams::mitm::applet::IViDisplaySvcMitm>> out, u32 mode), (out, mode)) \
    AMS_SF_METHOD_INFO(C, H, 1, Result, GetDisplayServiceWithProxyNameExchange, (sf::Out<sf::SharedPointer<ams::mitm::applet::IViDisplaySvcMitm>> out), (out))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IViRootMitm, AMS_VI_ROOT_MITM_INTERFACE_INFO, 0x2AB1E143)

#define AMS_VI_MANAGER_ROOT_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 2, Result, GetDisplayService, (sf::Out<sf::SharedPointer<ams::mitm::applet::IViDisplaySvcMitm>> out, u32 mode), (out, mode)) \
    AMS_SF_METHOD_INFO(C, H, 3, Result, GetDisplayServiceWithProxyNameExchange, (sf::Out<sf::SharedPointer<ams::mitm::applet::IViDisplaySvcMitm>> out), (out))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IViManagerRootMitm, AMS_VI_MANAGER_ROOT_MITM_INTERFACE_INFO, 0x2AB1E144)

#define AMS_VI_SYSTEM_ROOT_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 1, Result, GetDisplayService, (sf::Out<sf::SharedPointer<ams::mitm::applet::IViDisplaySvcMitm>> out, u32 mode), (out, mode)) \
    AMS_SF_METHOD_INFO(C, H, 3, Result, GetDisplayServiceWithProxyNameExchange, (sf::Out<sf::SharedPointer<ams::mitm::applet::IViDisplaySvcMitm>> out), (out))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IViSystemRootMitm, AMS_VI_SYSTEM_ROOT_MITM_INTERFACE_INFO, 0x2AB1E147)

namespace ams::mitm::applet {

    class BinderMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &) { return true; }
        public:
            Result TransactParcel(s32 session_id, u32 code, u32 flags, const sf::InBuffer &parcel_in, const sf::OutBuffer &parcel_out);
            Result TransactParcelAuto(s32 session_id, u32 code, u32 flags, const sf::InAutoSelectBuffer &parcel_in, const sf::OutAutoSelectBuffer &parcel_out);
        private:
            Result Transact(s32 session_id, u32 code, u32 flags, const void *in, size_t in_size, size_t out_size);
        public:
    };
    static_assert(IsIBinderMitm<BinderMitm>);

    class ViDisplaySvcMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &) { return true; }
        public:
            Result GetRelayService(sf::Out<sf::SharedPointer<IBinderMitm>> out);
            Result GetSystemDisplayService(sf::Out<sf::SharedPointer<IUiSysDisplayMitm>> out);
            Result OpenDisplay(const DisplayName &display_name, sf::Out<u64> out_display_id);
    };
    static_assert(IsIViDisplaySvcMitm<ViDisplaySvcMitm>);

    class ViRootMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
        public:
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info) {
                return ncm::IsApplicationId(client_info.program_id) && !client_info.override_status.IsHbl();
            }
        public:
            Result GetDisplayService(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out, u32 mode);
            Result GetDisplayServiceWithProxyNameExchange(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out);
        private:
            void Wrap(::Service disp_svc, sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> &out);
    };
    static_assert(IsIViRootMitm<ViRootMitm>);

    void GameBinderTransaction(const sm::MitmProcessInfo &ci, s32 session_id, u32 code, u32 flags, const void *in, size_t in_size, size_t out_size);

    Result ForwardGetDisplayService(::Service *fwd, u32 cmd, u32 mode, const sm::MitmProcessInfo &ci, sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> &out);
    void WrapDisplayService(::Service disp_svc, const sm::MitmProcessInfo &ci, sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> &out);
    Result ForwardProxyNameExchange(::Service *fwd, u32 cmd, const u8 *msg, const sm::MitmProcessInfo &ci, sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> &out, u32 plain_cmd);

    extern std::atomic<u64> g_app_pid;

    inline bool IsRunningApplication(os::ProcessId pid) {
        const u64 ap = g_app_pid.load(std::memory_order_relaxed);
        return ap != 0 && ap == pid.value;
    }
    void StartAppWatch();
    u32 AppInfoSerial();
    u32 AppInfoFormat(char *out, size_t cap, size_t *len);

    Result CreateSessionViaSm(os::NativeHandle *out_server, os::NativeHandle *out_client);
    u32 SessionsMadeViaSm();

    class ViManagerRootMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
        public:
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info) {
                const u64 tid = client_info.program_id.value;
                const bool app_range = tid >= UINT64_C(0x0100000000010000) && (tid >> 56) != 0x42;
                const bool applet = g_ui_vi_mitm.load(std::memory_order_relaxed) && IsLibraryApplet(tid);
                const bool hbmenu = g_ui_stream_armed && tid == UINT64_C(0x010000000000100D) && client_info.override_status.IsHbl();
                const bool tesla = tid == TeslaProgramId && g_ovl_vi_mitm.load(std::memory_order_relaxed);
                return app_range || applet || hbmenu || tesla || IsRunningApplication(client_info.process_id);
            }
        public:
            Result GetDisplayService(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out, u32 mode);
            Result GetDisplayServiceWithProxyNameExchange(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out);
    };
    static_assert(IsIViManagerRootMitm<ViManagerRootMitm>);

    class ViSystemRootMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
        public:
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info) {
                return g_ui_vi_mitm.load(std::memory_order_relaxed) && IsLibraryApplet(client_info.program_id.value);
            }
        public:
            Result GetDisplayService(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out, u32 mode);
            Result GetDisplayServiceWithProxyNameExchange(sf::Out<sf::SharedPointer<IViDisplaySvcMitm>> out);
    };
    static_assert(IsIViSystemRootMitm<ViSystemRootMitm>);

}
