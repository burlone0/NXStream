#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    constexpr svc::Handle kDmntDbg = static_cast<svc::Handle>(0x444D4E54u);

    inline bool DbgIsDmnt(svc::Handle dbg) { return dbg == kDmntDbg; }

    bool DmAttach(u64 pid, svc::Handle *out_dbg);

    bool DbgRead(svc::Handle dbg, void *dst, u64 va, size_t len);
    bool DbgQuery(svc::Handle dbg, svc::MemoryInfo *mi, svc::PageInfo *pi, u64 addr);

    Result DbgResume(svc::Handle dbg, u32 *out_nev);

    void DbgDetach(svc::Handle dbg);

    extern std::atomic<u32> g_app_dbg;
    extern std::atomic<u64> g_app_dbg_pid;
    extern os::SdkMutex g_app_dbg_lock;

    void DmStatsReset();
    void DmStatsLog(u32 session);

}
