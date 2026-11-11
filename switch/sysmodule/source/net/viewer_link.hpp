#pragma once

#include <switch.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/config.hpp"
#include "net/channel.hpp"
#include "protocol.hpp"

class UsbChannel;

class ViewerLink {
public:
    bool start(uint16_t port, std::string consoleName);

    bool connected() const { return active_.load(); }

    bool needsKeyframe() const { return needsKeyframe_.load(); }
    void keyframeSent() { needsKeyframe_ = false; }

    bool probing() const { return probing_.load(); }
    void setProbing(bool on);
    bool sendProbe(uint32_t seq, bool reply, uint32_t payloadSize);

    uint32_t sessionSerial() const { return sessionSerial_.load(); }
    bool sendAppInfo(const uint8_t* payload, size_t size);

    bool send(proto::MsgType type, uint8_t flags, uint64_t timestamp, const void* part1, size_t size1,
              const void* part2 = nullptr, size_t size2 = 0);

private:
    static constexpr int kHelloTimeoutMs = 5000;
    static constexpr int kIdleTimeoutMs = 6000;
    static constexpr int kSendStallMs = 3000;

    struct Counter {
        uint64_t messages = 0;
        uint64_t bytes = 0;
    };

    void serverLoop();
    void usbStep(UsbChannel& usb);
    void serve(net::Channel& ch, settings::Transport transport);
    bool handshake(net::Channel& ch);
    void drop(net::Channel& ch);
    void kick();
    bool sendRaw(net::Channel& ch, const void* data, size_t size);
    void logTotals();

    uint16_t port_ = 0;
    std::string consoleName_;
    Mutex sendMutex_;
    std::atomic<bool> active_{false};
    std::atomic<net::Channel*> ch_{nullptr};
    std::atomic<bool> needsKeyframe_{true};
    std::atomic<bool> abort_{false};
    std::atomic<bool> probing_{false};
    std::atomic<uint32_t> sessionSerial_{0};

    Counter sent_[256];
    uint64_t sessionStartMs_ = 0, slowestSendMs_ = 0;
    uint64_t pings_ = 0, pongsSkipped_ = 0, keyframeRequests_ = 0;
};
