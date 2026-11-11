#include "net/viewer_link.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "core/config.hpp"
#include "core/power.hpp"
#include "core/system.hpp"
#include "net/link_probe.hpp"
#include "net/socket_io.hpp"
#include "net/usb_link.hpp"

using bytes::rd32;

namespace {
bool onChargerOrDock() {
    static bool psmReady = false;
    static bool apmReady = false;
    static uint64_t lastTryMs = 0;
    static unsigned failures = 0;
    if (!psmReady || !apmReady) {
        const uint64_t now = sys::nowMs();
        if (lastTryMs != 0 && now - lastTryMs < 30000) return false;
        lastTryMs = now;
        if (!psmReady) psmReady = R_SUCCEEDED(psmInitialize());
        if (!apmReady) apmReady = R_SUCCEEDED(apmInitialize());
        if (!psmReady || !apmReady) {
            if (++failures == 1 || failures % 20 == 0)
                LOG("usb: power source query unavailable (psm %d, apm %d, failure %u)", psmReady ? 1 : 0,
                    apmReady ? 1 : 0, failures);
        }
    }
    if (psmReady) {
        PsmChargerType charger = PsmChargerType_Unconnected;
        if (R_SUCCEEDED(psmGetChargerType(&charger)) && charger == PsmChargerType_EnoughPower) return true;
    }
    if (apmReady) {
        ApmPerformanceMode mode = ApmPerformanceMode_Invalid;
        if (R_SUCCEEDED(apmGetPerformanceMode(&mode)) && mode == ApmPerformanceMode_Boost) return true;
    }
    return false;
}

std::string usbSerial(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (c >= 0x20 && c <= 0x7E) out.push_back(c);
    }
    const size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos) return proto::kUsbSerial;
    const size_t last = out.find_last_not_of(' ');
    out = out.substr(first, last - first + 1);
    if (out.size() > 63) {
        out.resize(63);
        const size_t end = out.find_last_not_of(' ');
        out.resize(end + 1);
    }
    return out;
}

int tuneSocket(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int sndbuf = 128 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
    int actual = 0;
    socklen_t actualLen = sizeof actual;
    getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &actual, &actualLen);
    return actual;
}

const char* typeName(uint8_t type) {
    switch (static_cast<proto::MsgType>(type)) {
        case proto::MsgType::Welcome: return "welcome";
        case proto::MsgType::Pong: return "pong";
        case proto::MsgType::VideoH264: return "h264";
        case proto::MsgType::VideoJpeg: return "jpeg";
        case proto::MsgType::Audio: return "audio";
        case proto::MsgType::Probe: return "probe";
        case proto::MsgType::AppInfo: return "appinfo";
        default: return "other";
    }
}
}

bool ViewerLink::start(uint16_t port, std::string consoleName) {
    port_ = port;
    consoleName_ = std::move(consoleName);
    mutexInit(&sendMutex_);
    return sys::spawn([this] { serverLoop(); }, 0x2C, 0x6000, "server");
}

void ViewerLink::serverLoop() {
    net::Socket listener;
    UsbChannel usb(abort_, usbSerial(consoleName_));
    uint32_t bindFailures = 0;
    uint32_t netFailures = 0;
    bool pausedLogged = false;
    settings::Transport current = settings::transport.load();
    LOG("server: transport %s", settings::transportName(current));
    while (true) {
        const settings::Transport wanted = settings::transport.load();
        if (wanted != current) {
            LOG("server: transport changed from %s to %s", settings::transportName(current),
                settings::transportName(wanted));
            listener.reset();
            usb.shutdown();
            bindFailures = 0;
            netFailures = 0;
            current = wanted;
        }
        if (power::halted()) {
            if (!pausedLogged) {
                LOG("server: paused, closing %s", settings::transportName(current));
                pausedLogged = true;
            }
            listener.reset();
            usb.shutdown();
            sys::sleepMs(250);
            continue;
        }
        if (pausedLogged) {
            LOG("server: resumed, rebuilding %s", settings::transportName(current));
            pausedLogged = false;
        }
        if (current == settings::Transport::Usb) {
            usbStep(usb);
            continue;
        }
        if (!listener) {
            net::Socket s(socket(AF_INET, SOCK_STREAM, 0));
            int one = 1;
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port_);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            if (s) setsockopt(s.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            if (!s || bind(s.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(s.get(), 1) != 0) {
                if (++bindFailures == 1 || bindFailures % 30 == 0)
                    LOG("server: tcp %u not ready yet (errno %d, attempt %u)", port_, errno,
                        static_cast<unsigned>(bindFailures));
                sys::sleepMs(2000);
                continue;
            }
            LOG("server: listening on tcp %u after %u attempts", port_, static_cast<unsigned>(bindFailures + 1));
            bindFailures = 0;
            listener = std::move(s);
        }

        pollfd pfd{listener.get(), POLLIN, 0};
        int pr = poll(&pfd, 1, 500);
        const int pollErrno = errno;
        if (pr < 0 || (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))) {
            if (++netFailures == 1 || netFailures % 30 == 0)
                LOG("server: listener broken (poll %d, errno %d, revents 0x%x, failure %u), rebuilding in 1 s", pr,
                    pollErrno, static_cast<unsigned>(pfd.revents), static_cast<unsigned>(netFailures));
            listener.reset();
            sys::sleepMs(1000);
            continue;
        }
        if (pr == 0) continue;

        int fd = accept(listener.get(), nullptr, nullptr);
        if (fd < 0) {
            if (++netFailures == 1 || netFailures % 30 == 0)
                LOG("server: accept failed, errno %d (failure %u), rebuilding listener in 1 s", errno,
                    static_cast<unsigned>(netFailures));
            listener.reset();
            sys::sleepMs(1000);
            continue;
        }
        if (netFailures > 0) {
            LOG("server: recovered after %u failures", static_cast<unsigned>(netFailures));
            netFailures = 0;
        }
        net::SocketChannel client(fd, abort_);
        LOG("server: connection accepted, nodelay on, sndbuf %d", tuneSocket(fd));
        serve(client, settings::Transport::Network);
    }
}

void ViewerLink::usbStep(UsbChannel& usb) {
    if (power::sleeping.load()) {
        if (usb.isOpen()) LOG("usb: console is sleeping, releasing usb:ds");
        usb.shutdown();
        sys::sleepMs(250);
        return;
    }
    static bool heldOff = false;
    if (onChargerOrDock()) {
        if (!heldOff) {
            LOG("usb: on a charger or dock, usb:ds released until a PC is connected");
            heldOff = true;
        }
        usb.shutdown();
        sys::sleepMs(1000);
        return;
    }
    if (heldOff) {
        LOG("usb: power source changed, enabling usb:ds");
        heldOff = false;
    }
    if (!usb.isOpen() && !usb.open()) {
        sys::sleepMs(2000);
        return;
    }
    if (usb.broken()) {
        LOG("usb: link broken, resetting usb:ds in 2 s");
        usb.shutdown();
        sys::sleepMs(2000);
        return;
    }
    if (!usb.waitConfigured(250) || !usb.waitReadable(250)) return;

    serve(usb, settings::Transport::Usb);
    usb.close();
    sys::sleepMs(500);
}

void ViewerLink::kick() {
    abort_ = true;
    sys::Lock lock(sendMutex_);
    net::Channel* ch = ch_.load();
    if (!ch) return;
    LOG("server: dropping the viewer on request");
    ch->kick();
}

bool ViewerLink::sendRaw(net::Channel& ch, const void* data, size_t size) {
    return size == 0 || ch.sendAll(data, size, kSendStallMs);
}

bool ViewerLink::handshake(net::Channel& ch) {
    uint8_t hdr[proto::kHeaderSize];
    net::IoStatus st = ch.recvExact(hdr, sizeof hdr, kHelloTimeoutMs);
    if (st != net::IoStatus::Ok) {
        LOG("server: no hello (%s)", net::toString(st));
        return false;
    }
    const proto::Header h = proto::decodeHeader(hdr);
    if (h.type != proto::MsgType::Hello || h.size < 4 || h.size > 1024) {
        static uint64_t lastLogMs = 0;
        static unsigned suppressed = 0;
        const uint64_t now = sys::nowMs();
        if (lastLogMs == 0 || now - lastLogMs >= 30000) {
            LOG("server: first message is type 0x%02x size %u, not a hello: not an NXStream viewer (%u similar "
                "suppressed)",
                static_cast<unsigned>(h.type), static_cast<unsigned>(h.size), suppressed);
            lastLogMs = now;
            suppressed = 0;
        } else {
            ++suppressed;
        }
        return false;
    }

    std::vector<uint8_t> payload(h.size);
    if (ch.recvExact(payload.data(), h.size, kHelloTimeoutMs) != net::IoStatus::Ok) return false;
    const uint32_t version = rd32(payload.data());

    if (version == proto::kVersion) {
        active_ = true;
        needsKeyframe_ = true;
    }

    std::vector<uint8_t> msg(proto::kHeaderSize + 8 + consoleName_.size());
    proto::encodeHeader(msg.data(), proto::MsgType::Welcome, 0, static_cast<uint32_t>(msg.size() - proto::kHeaderSize));
    bytes::wr32(msg.data() + 8, proto::kVersion);
    bytes::wr32(msg.data() + 12, hosversionGet());
    std::memcpy(msg.data() + 16, consoleName_.data(), consoleName_.size());
    if (!sendRaw(ch, msg.data(), msg.size())) {
        LOG("server: welcome not delivered");
        if (version == proto::kVersion) active_ = false;
        return false;
    }

    if (version != proto::kVersion) {
        LOG("server: viewer speaks protocol %u, expected %u", version, proto::kVersion);
        return false;
    }
    return true;
}

void ViewerLink::serve(net::Channel& ch, settings::Transport transport) {
    abort_ = false;
    const uint64_t acceptedMs = sys::nowMs();
    if (transport == settings::Transport::Usb) LOG("server: usb host talking, waiting for hello");
    if (!handshake(ch)) return;

    {
        sys::Lock lock(sendMutex_);
        for (auto& c : sent_) c = Counter{};
        sessionStartMs_ = sys::nowMs();
        slowestSendMs_ = pings_ = pongsSkipped_ = keyframeRequests_ = 0;
    }
    LOG("server: viewer connected over %s (protocol %u, handshake %llu ms)", ch.name(), proto::kVersion,
        static_cast<unsigned long long>(sys::nowMs() - acceptedMs));
    ch_ = &ch;
    ++sessionSerial_;

    net::IoStatus status;
    uint8_t hdr[proto::kHeaderSize];
    uint8_t payload[64];
    unsigned ignored = 0;
    while (true) {
        if (power::halted() || settings::transport.load() != transport ||
            (transport == settings::Transport::Usb && power::sleeping.load())) {
            status = net::IoStatus::Aborted;
            break;
        }
        status = ch.recvExact(hdr, sizeof hdr, kIdleTimeoutMs);
        if (status != net::IoStatus::Ok) break;
        const proto::Header h = proto::decodeHeader(hdr);
        if (h.size > sizeof payload) {
            LOG("server: message 0x%02x of %u bytes from the viewer is too big", static_cast<unsigned>(h.type),
                static_cast<unsigned>(h.size));
            status = net::IoStatus::Error;
            break;
        }
        if (h.size > 0 && (status = ch.recvExact(payload, h.size, kIdleTimeoutMs)) != net::IoStatus::Ok)
            break;

        if (h.type == proto::MsgType::Bye) {
            LOG("server: viewer said bye");
            break;
        }
        if (h.type == proto::MsgType::KeyframeRequest) {
            ++keyframeRequests_;
            needsKeyframe_ = true;
            continue;
        }
        if (h.type == proto::MsgType::ProbeAck) {
            if (h.size == 4) linkprobe::onAck(rd32(payload));
            continue;
        }
        if (h.type == proto::MsgType::Ping && h.size >= 8) {
            ++pings_;
            uint8_t pong[proto::kHeaderSize + 8];
            proto::encodeHeader(pong, proto::MsgType::Pong, 0, 8);
            std::memcpy(pong + proto::kHeaderSize, payload, 8);
            if (mutexTryLock(&sendMutex_)) {
                const bool ok = sendRaw(ch, pong, sizeof pong);
                Counter& c = sent_[static_cast<uint8_t>(proto::MsgType::Pong)];
                ++c.messages;
                c.bytes += sizeof pong;
                mutexUnlock(&sendMutex_);
                if (!ok) break;
            } else {
                ++pongsSkipped_;
            }
            continue;
        }
        if (++ignored <= 5)
            LOG("server: ignored message 0x%02x (%u bytes)", static_cast<unsigned>(h.type),
                static_cast<unsigned>(h.size));
    }

    LOG("server: viewer disconnected (%s)", net::toString(status));
    drop(ch);
}

void ViewerLink::drop(net::Channel& ch) {
    abort_ = true;
    ch.kick();
    {
        sys::Lock lock(sendMutex_);
        ch_ = nullptr;
        logTotals();
    }
    probing_ = false;
    active_ = false;
}

void ViewerLink::logTotals() {
    const uint64_t ms = std::max<uint64_t>(sys::nowMs() - sessionStartMs_, 1);
    uint64_t total = 0;
    for (const auto& c : sent_) total += c.bytes;
    LOG("server: session %llu s, %llu KB sent, average %llu kbit/s, %llu pings (%llu pongs skipped while busy), "
        "%llu keyframe requests, slowest send %llu ms",
        static_cast<unsigned long long>(ms / 1000), static_cast<unsigned long long>(total / 1024),
        static_cast<unsigned long long>(total * 8 / ms), static_cast<unsigned long long>(pings_),
        static_cast<unsigned long long>(pongsSkipped_), static_cast<unsigned long long>(keyframeRequests_),
        static_cast<unsigned long long>(slowestSendMs_));
    for (int t = 0; t < 256; ++t) {
        const Counter& c = sent_[t];
        if (c.messages == 0) continue;
        LOG("server:   %-7s %8llu msgs %8llu KB  avg %6llu B  %4llu.%01llu/s", typeName(static_cast<uint8_t>(t)),
            static_cast<unsigned long long>(c.messages), static_cast<unsigned long long>(c.bytes / 1024),
            static_cast<unsigned long long>(c.bytes / c.messages),
            static_cast<unsigned long long>(c.messages * 1000 / ms),
            static_cast<unsigned long long>(c.messages * 10000 / ms % 10));
    }
}

void ViewerLink::setProbing(bool on) {
    probing_ = on;
    if (on) {
        sys::Lock lock(sendMutex_);
    } else {
        needsKeyframe_ = true;
    }
}

bool ViewerLink::sendProbe(uint32_t seq, bool reply, uint32_t payloadSize) {
    static uint8_t zeros[16 * 1024];
    if (!active_.load() || !ch_.load() || power::sleeping.load()) return false;
    if (power::halted() || power::captureHalted()) {
        if (power::halted()) kick();
        return false;
    }
    payloadSize = std::max<uint32_t>(payloadSize, 4);
    sys::Lock lock(sendMutex_);
    net::Channel* ch = ch_.load();
    if (!ch) return false;

    uint8_t head[proto::kHeaderSize + 4];
    proto::encodeHeader(head, proto::MsgType::Probe, reply ? proto::kFlagProbeReply : 0, payloadSize);
    bytes::wr32(head + proto::kHeaderSize, seq);

    const uint64_t start = sys::nowMs();
    bool ok = sendRaw(*ch, head, sizeof head);
    for (uint32_t left = payloadSize - 4; ok && left > 0;) {
        const size_t n = std::min<size_t>(left, sizeof zeros);
        ok = sendRaw(*ch, zeros, n);
        left -= static_cast<uint32_t>(n);
    }
    const uint64_t took = sys::nowMs() - start;
    slowestSendMs_ = std::max(slowestSendMs_, took);
    Counter& c = sent_[static_cast<uint8_t>(proto::MsgType::Probe)];
    ++c.messages;
    c.bytes += proto::kHeaderSize + payloadSize;
    if (!ok) {
        LOG("server: send of probe (%u bytes) failed after %llu ms (%s, errno %d), dropping viewer",
            static_cast<unsigned>(payloadSize), static_cast<unsigned long long>(took), ch->name(), errno);
        ch->kick();
        abort_ = true;
    }
    return ok;
}

bool ViewerLink::sendAppInfo(const uint8_t* payload, size_t size) {
    if (!active_.load() || !ch_.load() || power::captureHalted()) return false;
    sys::Lock lock(sendMutex_);
    net::Channel* ch = ch_.load();
    if (!ch) return false;

    uint8_t head[proto::kHeaderSize];
    proto::encodeHeader(head, proto::MsgType::AppInfo, 0, static_cast<uint32_t>(size));
    const uint64_t start = sys::nowMs();
    const bool ok = sendRaw(*ch, head, sizeof head) && sendRaw(*ch, payload, size);
    const uint64_t took = sys::nowMs() - start;
    slowestSendMs_ = std::max(slowestSendMs_, took);
    Counter& c = sent_[static_cast<uint8_t>(proto::MsgType::AppInfo)];
    ++c.messages;
    c.bytes += sizeof head + size;
    if (!ok) {
        LOG("server: send of appinfo (%zu bytes) failed after %llu ms (%s, errno %d), dropping viewer", size,
            static_cast<unsigned long long>(took), ch->name(), errno);
        ch->kick();
        abort_ = true;
    }
    return ok;
}

bool ViewerLink::send(proto::MsgType type, uint8_t flags, uint64_t timestamp, const void* part1, size_t size1,
                      const void* part2, size_t size2) {
    if (!active_.load() || !ch_.load() || power::sleeping.load()) return false;
    if (probing_.load()) return true;
    if (power::halted()) {
        kick();
        return false;
    }
    sys::Lock lock(sendMutex_);
    net::Channel* ch = ch_.load();
    if (!ch) return false;
    if (probing_.load()) return true;

    uint8_t head[proto::kHeaderSize + 8];
    proto::encodeHeader(head, type, flags, static_cast<uint32_t>(8 + size1 + size2));
    bytes::wr64(head + proto::kHeaderSize, timestamp);

    const uint64_t start = sys::nowMs();
    const bool ok = sendRaw(*ch, head, sizeof head) && sendRaw(*ch, part1, size1) && sendRaw(*ch, part2, size2);
    const uint64_t took = sys::nowMs() - start;
    slowestSendMs_ = std::max(slowestSendMs_, took);
    Counter& c = sent_[static_cast<uint8_t>(type)];
    ++c.messages;
    c.bytes += sizeof head + size1 + size2;
    if (!ok) {
        LOG("server: send of %s (%zu bytes) failed after %llu ms (%s, errno %d), dropping viewer",
            typeName(static_cast<uint8_t>(type)), size1 + size2, static_cast<unsigned long long>(took), ch->name(),
            errno);
        ch->kick();
        abort_ = true;
    }
    return ok;
}
