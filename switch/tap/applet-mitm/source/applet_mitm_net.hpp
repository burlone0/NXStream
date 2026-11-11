#pragma once
#include <stratosphere.hpp>
#include <atomic>

namespace ams::mitm::applet {

    extern bool g_net_armed;
    extern std::atomic<bool> g_tx_net;
    extern std::atomic<bool> g_net_wired;

    void StartNet();

    bool NetClientPresent();

    u32 NetClientSerial();

    u32 NetIp();

    bool NetSendBuffer(const void *buf, size_t len);
    bool NetPostAsync(const void *buf, size_t len);
    bool NetWaitAsync(size_t *out_sent);

    void NetDropClient(const char *why);

    void NetLogStats(const char *who);

    u32 NetBusyPercent();

}
