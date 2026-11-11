#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    struct DebugPumpStats {
        u32 wakes;
        u32 events;
        u32 created;
        u32 exited;
        u32 other;
        u32 faults;
        u32 continues;
        u32 busy;
        u32 failed;
        u32 last_fail_rc;
        u32 per_core[4];
        u64 max_hold_ns;
        bool gone;
    };

    void DebugPumpStart(::ams::svc::Handle dbg);

    void DebugPumpStop();

    bool DebugPumpTargetGone();

    void DebugPumpGetStats(DebugPumpStats *out);

    void DebugPumpLogStats(const char *who);

}
