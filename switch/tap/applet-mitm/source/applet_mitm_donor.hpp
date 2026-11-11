#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    bool DonorObtain(uintptr_t *out_addr, size_t *out_size);
    void DonorRelease();
    bool DonorActive();
    bool AppletBusy(u64 *out_mb = nullptr);
    bool SwitchUOpen();
    bool SwitchUMenuClosed();
    bool KeyboardHoldsStream();
    bool DonorCanGrow();

    extern std::atomic<bool> g_applet_needs_memory;
    void StartAppletWatch();
    void PrepareForApplet(u32 applet_id);
    void PrepareForHomeMenu();
    bool WebAppletOpen();
    bool AppletHoldActive();
    bool HomeDetachShouldFire(u64 stall_age_ms, u32 probe_ms, u32 fallback_ms, const char **out_reason);

    void DonorAccept(u64 size, os::NativeHandle tmem);
    void DonorAcceptNv(u64 size, os::NativeHandle tmem);
    bool DonorTakeNvTmem(os::NativeHandle *out, u64 *out_size);
    void DonorFailed(u32 rc);

    u32 DonorCapMb();

}
