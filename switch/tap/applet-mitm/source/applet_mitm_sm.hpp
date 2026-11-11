#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    namespace sm_use {
        inline constinit os::SdkMutex g_lock;
        inline constinit int g_users = 0;
        inline constinit bool g_boot_held = true;
    }

    class SmUse {
        public:
            SmUse() {
                std::scoped_lock lk(sm_use::g_lock);
                m_ok = R_SUCCEEDED(::smInitialize());
                if (m_ok) { m_fresh = ++sm_use::g_users == 1 && !sm_use::g_boot_held; }
            }
            ~SmUse() {
                if (!m_ok) { return; }
                std::scoped_lock lk(sm_use::g_lock);
                if (--sm_use::g_users == 0 && !sm_use::g_boot_held) { static_cast<void>(::smDetachClient()); }
                ::smExit();
            }
            SmUse(const SmUse &) = delete;
            SmUse &operator=(const SmUse &) = delete;
            bool ok() const { return m_ok; }
            bool fresh() const { return m_fresh; }
        private:
            bool m_ok;
            bool m_fresh = false;
    };

    inline Result SmReleaseBoot() {
        std::scoped_lock lk(sm_use::g_lock);
        if (!sm_use::g_boot_held) { R_SUCCEED(); }
        sm_use::g_boot_held = false;
        Result rc = ResultSuccess();
        if (sm_use::g_users == 0) { rc = ::smDetachClient(); }
        ::smExit();
        R_RETURN(rc);
    }

}
