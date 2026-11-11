#include "net/discovery.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <unistd.h>

#include <cerrno>
#include <string>
#include <string_view>
#include <utility>

#include "core/config.hpp"
#include "core/power.hpp"
#include "core/system.hpp"
#include "net/socket_io.hpp"
#include "net/viewer_link.hpp"
#include "protocol.hpp"

bool DiscoveryResponder::start(uint16_t udpPort, uint16_t tcpPort, std::string consoleName) {
    udpPort_ = udpPort;
    tcpPort_ = tcpPort;
    consoleName_ = std::move(consoleName);
    return sys::spawn([this] { run(); }, 0x3B, 0x5000, "discovery");
}

std::string DiscoveryResponder::announcement() const {
    std::string msg = proto::kAnnounceMagic;
    msg += '\n';
    msg += consoleName_;
    msg += '\n';
    msg += std::to_string(tcpPort_);
    msg += '\n';
    msg += link_.connected() ? '1' : '0';
    return msg;
}

void DiscoveryResponder::periodicTasks(int fd) {
    const uint64_t now = sys::nowMs();
    if (now < nextBeaconMs_) return;
    nextBeaconMs_ = now + 2000;

    const int netUp = static_cast<uint32_t>(gethostid()) != 0 ? 1 : 0;
    if (netUp != netUp_) {
        LOG("network: %s", netUp ? "up" : "down");
        netUp_ = netUp;
    }

    if (settings::transport.load() == settings::Transport::Usb || settings::nativeCapture()) return;

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(proto::kBeaconPort);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    const std::string msg = announcement();
    if (sendto(fd, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof to) <= 0 &&
        (++beaconFailures_ == 1 || beaconFailures_ % 30 == 0)) {
        LOG("discovery: beacon send failed, errno %d (failure %u)", errno, static_cast<unsigned>(beaconFailures_));
    }
}

void DiscoveryResponder::run() {
    net::Socket sock;
    bool pausedLogged = false;
    uint32_t netFailures = 0;
    while (true) {
        settings::reloadIfChanged();
        if (power::halted()) {
            if (!pausedLogged) {
                LOG("discovery: paused, closing udp %u", udpPort_);
                pausedLogged = true;
                sock.reset();
            }
            sys::sleepMs(250);
            continue;
        }
        if (pausedLogged) {
            LOG("discovery: resumed, rebinding udp %u", udpPort_);
            pausedLogged = false;
            nextBeaconMs_ = 0;
        }
        if (!sock) {
            net::Socket s(socket(AF_INET, SOCK_DGRAM, 0));
            int one = 1;
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(udpPort_);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            if (s) {
                setsockopt(s.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
                setsockopt(s.get(), SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
            }
            if (!s || bind(s.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
                sys::sleepMs(2000);
                continue;
            }
            LOG("discovery: udp %u bound, beacons to udp %u every 2 s", udpPort_, proto::kBeaconPort);
            sock = std::move(s);
        }

        periodicTasks(sock.get());

        pollfd pfd{sock.get(), POLLIN, 0};
        int pr = poll(&pfd, 1, 250);
        const int pollErrno = errno;
        if (pr < 0 || (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))) {
            if (++netFailures == 1 || netFailures % 30 == 0)
                LOG("discovery: socket broken (poll %d, errno %d, revents 0x%x, failure %u), rebinding in 1 s", pr,
                    pollErrno, static_cast<unsigned>(pfd.revents), static_cast<unsigned>(netFailures));
            sock.reset();
            sys::sleepMs(1000);
            continue;
        }
        if (pr == 0) continue;

        char buf[128];
        sockaddr_in from{};
        socklen_t fromLen = sizeof from;
        ssize_t n = recvfrom(sock.get(), buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n < 0) {
            const int recvErrno = errno;
            if (++netFailures == 1 || netFailures % 30 == 0)
                LOG("discovery: recvfrom failed (errno %d, failure %u), rebinding in 1 s", recvErrno,
                    static_cast<unsigned>(netFailures));
            sock.reset();
            sys::sleepMs(1000);
            continue;
        }
        if (n == 0) continue;
        if (netFailures > 0) {
            LOG("discovery: recovered after %u failures", static_cast<unsigned>(netFailures));
            netFailures = 0;
        }
        if (!std::string_view(buf, static_cast<size_t>(n)).starts_with(proto::kDiscoverMagic)) continue;
        if (settings::nativeCapture() && from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) continue;

        const std::string reply = announcement();
        sendto(sock.get(), reply.data(), reply.size(), 0, reinterpret_cast<sockaddr*>(&from), fromLen);
    }
}
