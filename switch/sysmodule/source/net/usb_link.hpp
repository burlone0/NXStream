#pragma once

#include <switch.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "net/channel.hpp"

class UsbChannel : public net::Channel {
public:
    UsbChannel(const std::atomic<bool>& abort, std::string serial) : abort_(abort), serial_(std::move(serial)) {}

    bool open();
    void shutdown();
    bool isOpen() const { return open_.load(); }
    bool broken() const { return broken_.load(); }
    bool waitConfigured(int sliceMs);
    bool waitReadable(int sliceMs);

    bool sendAll(const void* buf, size_t size, int stallTimeoutMs) override;
    net::IoStatus recvExact(void* buf, size_t size, int idleTimeoutMs) override;
    void kick() override;
    void close() override;
    const char* name() const override { return "usb"; }

private:
    enum class Pump { Ok, Timeout, Lost, Aborted, Fatal };

    enum class Urb { Pending, Done, Failed };

    bool step(const char* what, Result rc);
    void fail(const char* what, Result rc);
    bool configured();
    bool usable() const;
    bool idleStop();
    bool stopRequested();
    Result poll(UsbDsEndpoint* ep, u32 urb, Urb* state, UsbDsReportEntry* entry);
    bool writeAll(const uint8_t* p, size_t size, int stallTimeoutMs, bool final);
    bool finishIn(u32 requested, int stallTimeoutMs, bool final);
    void sendBye();
    bool drainIn(int budgetMs);
    bool drainOut(int budgetMs);
    bool cancelAndDrainIn();
    Pump pump(int waitMs, bool session);
    Result postRead();
    void markStuck();
    void flush();
    void releaseEndpoints();

    const std::atomic<bool>& abort_;
    std::string serial_;
    std::atomic<bool> open_{false};
    std::atomic<bool> broken_{false};
    std::atomic<bool> kicked_{false};
    std::atomic<bool> stuck_{false};
    std::atomic<int> lastState_{-1};
    std::atomic<uint32_t> maxPacket_{0x200};
    bool initialized_ = false;
    uint32_t openFailures_ = 0;
    std::atomic<uint32_t> stateFailures_{0};
    uint32_t readErrors_ = 0;
    std::atomic<uint32_t> strayReports_{0};
    Mutex inMutex_{};
    Mutex outMutex_{};

    UsbDsInterface* iface_ = nullptr;
    UsbDsEndpoint* in_ = nullptr;
    UsbDsEndpoint* out_ = nullptr;

    bool inPending_ = false;
    u32 inUrb_ = 0;
    bool outPending_ = false;
    u32 outUrb_ = 0;
    uint32_t ringHead_ = 0;
    uint32_t ringTail_ = 0;
    uint64_t sentBytes_ = 0;
    uint64_t receivedBytes_ = 0;
};
