#include "core/power.hpp"

#include <switch.h>

#include "core/system.hpp"

namespace {
constexpr u32 kModuleId = 0xBD;
constexpr uint64_t kIdleWaitMs = 1000;
constexpr uint64_t kIdlePollMs = 10;

const char* stateName(PscPmState state) {
    switch (state) {
        case PscPmState_Awake: return "Awake";
        case PscPmState_ReadyAwaken: return "ReadyAwaken";
        case PscPmState_ReadySleep: return "ReadySleep";
        case PscPmState_ReadySleepCritical: return "ReadySleepCritical";
        case PscPmState_ReadyAwakenCritical: return "ReadyAwakenCritical";
        case PscPmState_ReadyShutdown: return "ReadyShutdown";
        default: return "unknown";
    }
}

void waitCaptureIdle() {
    const uint64_t start = sys::nowMs();
    while (!power::captureIdle() && sys::nowMs() - start < kIdleWaitMs) sys::sleepMs(kIdlePollMs);
    LOG("power: capture idle=%d after %llu ms (audrec busy=%d, caps:sc busy=%d)", power::captureIdle() ? 1 : 0,
        static_cast<unsigned long long>(sys::nowMs() - start), power::audrecBusy.load() ? 1 : 0,
        power::screenBusy.load() ? 1 : 0);
}

void run() {
    Result rc = pscmInitialize();
    if (R_FAILED(rc)) {
        LOG("power: psc:m unavailable 0x%x, no power notifications", rc);
        return;
    }
    const u32 dependencies[1] = {static_cast<u32>(PscPmModuleId_Fs)};
    PscPmModule module;
    rc = pscmGetPmModule(&module, static_cast<PscPmModuleId>(kModuleId), dependencies, 1, true);
    if (R_FAILED(rc)) {
        LOG("PSC id 0xbd taken (MissionControl installed?) 0x%x, no power notifications", rc);
        pscmExit();
        return;
    }
    LOG("power: PSC module 0x%x registered, depends on Fs", kModuleId);

    while (true) {
        eventWait(&module.event, UINT64_MAX);
        PscPmState state;
        u32 flags = 0;
        rc = pscPmModuleGetRequest(&module, &state, &flags);
        if (R_FAILED(rc)) {
            LOG("power: get request failed 0x%x", rc);
            sys::sleepMs(100);
            continue;
        }
        LOG("power: request %s (%u), flags 0x%x", stateName(state), static_cast<unsigned>(state), flags);
        if (state == PscPmState_ReadySleep || state == PscPmState_ReadyShutdown) {
            power::sleeping = true;
            waitCaptureIdle();
        } else if (state == PscPmState_Awake) {
            power::sleeping = false;
        }
        rc = pscPmModuleAcknowledge(&module, state);
        if (R_FAILED(rc)) LOG("power: acknowledge %s failed 0x%x", stateName(state), rc);
    }
}
}

bool power::start() {
    return sys::spawn(run, 0x2D, 0x4000, "power");
}
