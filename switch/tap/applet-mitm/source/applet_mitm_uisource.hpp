#pragma once
#include <stratosphere.hpp>
#include "applet_mitm_nv.hpp"

namespace ams::mitm::applet {

    extern bool g_ui_stream_armed;

    bool UiSourceFind(u64 *out_pid, u64 *out_base, GameSurface *out_geo);

    bool UiSourceStart(::ams::svc::Handle dbg, u64 pid, u64 base);
    void UiSourceStop();
    bool UiSourceIdle();

    void UiSourceLogStats(const char *who);

}
