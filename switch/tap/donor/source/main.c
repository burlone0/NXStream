#include <switch.h>

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

#define INNER_HEAP_SIZE 0x4000
static char g_inner_heap[INNER_HEAP_SIZE];

void __libnx_initheap(void) {
    extern char *fake_heap_start;
    extern char *fake_heap_end;
    fake_heap_start = g_inner_heap;
    fake_heap_end   = g_inner_heap + sizeof(g_inner_heap);
}

void __appInit(void) {
    if (R_FAILED(smInitialize())) { diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM)); }
}

void __appExit(void) { smExit(); }

void Launcher(void);
bool Running(u64 tid);

#define SFT_TID 0x4200000000004E54ull

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (R_SUCCEEDED(pmdmntInitialize())) {
        const bool at_boot = armTicksToNs(armGetSystemTick()) < 15000000000ull;
        bool sft = Running(SFT_TID);
        for (int i = 0; at_boot && sft && i < 40; ++i) {
            svcSleepThread(250000000ull);
            sft = Running(SFT_TID);
        }
        if (!sft) { Launcher(); }
        pmdmntExit();
        if (at_boot || !sft) { return 0; }
    }
    Service ctl;
    if (R_FAILED(smGetService(&ctl, "nxtap"))) { return 1; }
    smExit();

    u32 cap_mb = 42;
    if (R_FAILED(serviceDispatchOut(&ctl, 13, cap_mb)) || cap_mb < 22 || cap_mb > 42) { cap_mb = 42; }

    static const u32 sizes_mb[] = { 42, 38, 34, 30, 26, 22 };
    void *addr = NULL;
    u64 size = 0;
    for (u32 i = 0; i < sizeof(sizes_mb) / sizeof(sizes_mb[0]); ++i) {
        if (sizes_mb[i] > cap_mb) { continue; }
        if (R_SUCCEEDED(svcSetHeapSize(&addr, (u64)sizes_mb[i] << 20))) { size = (u64)sizes_mb[i] << 20; break; }
    }

    Handle tmem = INVALID_HANDLE, nv_tmem = INVALID_HANDLE;
    const u64 nv_size = 0x40000;
    Result rc = size > nv_size ? svcCreateTransferMemory(&tmem, addr, size - nv_size, Perm_None) : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    if (R_SUCCEEDED(rc)) { rc = svcCreateTransferMemory(&nv_tmem, (u8 *)addr + size - nv_size, nv_size, Perm_None); }
    if (R_SUCCEEDED(rc)) {
        const struct { u64 size, nv_size; } in = { size - nv_size, nv_size };
        const Result drc = serviceDispatchIn(&ctl, 12, in, .in_num_handles = 2, .in_handles = { tmem, nv_tmem });
        svcCloseHandle(tmem);
        svcCloseHandle(nv_tmem);
        if (R_FAILED(drc)) {
            if (R_SUCCEEDED(svcCreateTransferMemory(&tmem, addr, size, Perm_None))) {
                serviceDispatchIn(&ctl, 10, size, .in_num_handles = 1, .in_handles = { tmem });
                svcCloseHandle(tmem);
            }
        }
    } else {
        if (tmem != INVALID_HANDLE) { svcCloseHandle(tmem); }
        const u32 urc = rc;
        serviceDispatchIn(&ctl, 11, urc);
    }
    serviceClose(&ctl);

    if (size != 0) {
        while (R_FAILED(svcSetHeapSize(&addr, 0))) { svcSleepThread(5000000ull); }
    }
    return 0;
}
