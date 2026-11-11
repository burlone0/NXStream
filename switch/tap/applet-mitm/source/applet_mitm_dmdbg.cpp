#include "applet_mitm_dmdbg.hpp"
#include "applet_mitm_sm.hpp"
#include "applet_mitm_log.hpp"
#include <cstring>

namespace ams::mitm::applet {

    u8 *VicBounceBuffer();
    size_t VicBounceBufferSize();

    namespace {

        constinit ::Service g_cht = {};
        constinit bool g_cht_init = false;
        constinit bool g_cht_ok = false;

        constinit os::SdkMutex g_cht_lock;

        bool ChtService() {
            if (!g_cht_init) {
                g_cht_init = true;
                SmUse sm_cht;
                const ::Result rc = smGetService(std::addressof(g_cht), "dmnt:cht");
                g_cht_ok = R_SUCCEEDED(rc);
                LogLine("dmdbg: smGetService(dmnt:cht) rc=0x%x %s", rc,
                        g_cht_ok ? "OPEN" : "missing - direct debug only");
            }
            return g_cht_ok;
        }

        bool ChtHasProcess(bool *out) {
            u8 tmp = 0;
            const ::Result rc = serviceDispatchOut(std::addressof(g_cht), 65000, tmp);
            if (R_SUCCEEDED(rc) && out != nullptr) { *out = (tmp & 1) != 0; }
            return R_SUCCEEDED(rc);
        }

        ::Result ChtRead(u64 va, void *dst, size_t len) {
            const struct { u64 address; u64 size; } in = { va, len };
            return serviceDispatchIn(std::addressof(g_cht), 65102, in,
                .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
                .buffers = { { dst, len } },
            );
        }

        ::Result ChtQuery(u64 addr, svc::MemoryInfo *mi) {
            return serviceDispatchInOut(std::addressof(g_cht), 65104, addr, *mi);
        }

    }

    constinit std::atomic<u32> g_app_dbg{0};
    constinit std::atomic<u64> g_app_dbg_pid{0};
    constinit os::SdkMutex g_app_dbg_lock;

    bool DmAttach(u64 pid, svc::Handle *out_dbg) {
        const Result ra = svc::DebugActiveProcess(out_dbg, pid);
        if (R_SUCCEEDED(ra)) {
            LogLine("dmdbg: direct DebugActiveProcess(pid=%llu) ok",
                    static_cast<unsigned long long>(pid));
            g_app_dbg_pid.store(pid, std::memory_order_relaxed);
            g_app_dbg.store(static_cast<u32>(*out_dbg), std::memory_order_release);
            return true;
        }
        if (ChtService()) {
            std::scoped_lock lk(g_cht_lock);
            bool has = false;
            if (ChtHasProcess(std::addressof(has)) && has) {
                *out_dbg = kDmntDbg;
                LogLine("dmdbg: direct rc=0x%x, dmnt already holds pid %llu - reading through dmnt:cht (cheats keep working)",
                        ra.GetValue(), static_cast<unsigned long long>(pid));
                return true;
            }
        }
        LogLine("dmdbg: no attach to pid %llu (direct rc=0x%x, dmnt has no process)",
                static_cast<unsigned long long>(pid), ra.GetValue());
        return false;
    }

    bool DbgRead(svc::Handle dbg, void *dst, u64 va, size_t len) {
        if (dst == nullptr || len == 0) { return false; }
        if (DbgIsDmnt(dbg)) {
            std::scoped_lock lk(g_cht_lock);
            u8 *out = static_cast<u8 *>(dst);
            u8 *bounce = VicBounceBuffer();
            const size_t piece = VicBounceBufferSize();
            if (bounce == nullptr) { return false; }
            for (size_t off = 0; off < len; off += piece) {
                const size_t n = (len - off < piece) ? (len - off) : piece;
                if (R_FAILED(ChtRead(va + off, bounce, n))) { return false; }
                std::memcpy(out + off, bounce, n);
            }
            return true;
        }
        return R_SUCCEEDED(svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(dst), dbg, va, len));
    }

    bool DbgQuery(svc::Handle dbg, svc::MemoryInfo *mi, svc::PageInfo *pi, u64 addr) {
        if (mi == nullptr) { return false; }
        if (DbgIsDmnt(dbg)) {
            std::scoped_lock lk(g_cht_lock);
            if (R_FAILED(ChtQuery(addr, mi))) { return false; }
            if (pi != nullptr) { *pi = {}; }
            return true;
        }
        return R_SUCCEEDED(svc::QueryDebugProcessMemory(mi, pi, dbg, addr));
    }

    Result DbgResume(svc::Handle dbg, u32 *out_nev) {
        if (DbgIsDmnt(dbg)) {
            if (out_nev != nullptr) { *out_nev = 0; }
            return ResultSuccess();
        }
        svc::DebugEventInfo ev;
        u32 nev = 0;
        while (nev < 256 && R_SUCCEEDED(svc::GetDebugEvent(std::addressof(ev), dbg))) { ++nev; }
        const Result rc = svc::ContinueDebugEvent(dbg,
                              svc::ContinueFlag_ExceptionHandled | svc::ContinueFlag_ContinueAll,
                              nullptr, 0);
        if (out_nev != nullptr) { *out_nev = nev; }
        return rc;
    }

    void DbgDetach(svc::Handle dbg) {
        if (DbgIsDmnt(dbg)) { return; }
        if (dbg != svc::InvalidHandle) {
            u32 expected = static_cast<u32>(dbg);
            if (g_app_dbg.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
                std::scoped_lock lk(g_app_dbg_lock);
            }
            svc::CloseHandle(dbg);
        }
    }

}
