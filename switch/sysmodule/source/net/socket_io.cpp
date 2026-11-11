#include "net/socket_io.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

#include "core/power.hpp"
#include "core/system.hpp"

namespace net {
namespace {
constexpr int kPollSliceMs = 100;
}

const char* toString(IoStatus s) {
    switch (s) {
        case IoStatus::Ok: return "ok";
        case IoStatus::Closed: return "closed";
        case IoStatus::Timeout: return "timeout";
        case IoStatus::Aborted: return "aborted";
        case IoStatus::Error: return "error";
    }
    return "?";
}

void Socket::reset(int fd) {
    if (fd_ >= 0) {
        shutdown(fd_, SHUT_RDWR);
        close(fd_);
    }
    fd_ = fd;
}

IoStatus recvExact(int fd, void* buf, size_t size, const std::atomic<bool>& abort, int idleTimeoutMs) {
    auto* p = static_cast<uint8_t*>(buf);
    uint64_t deadline = sys::nowMs() + static_cast<uint64_t>(idleTimeoutMs);
    while (size > 0) {
        if (abort.load(std::memory_order_relaxed)) return IoStatus::Aborted;
        if (sys::nowMs() >= deadline) return IoStatus::Timeout;

        pollfd pfd{fd, POLLIN, 0};
        int pr = poll(&pfd, 1, kPollSliceMs);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return IoStatus::Error;
        }
        if (pr == 0) continue;

        ssize_t n = recv(fd, p, size, 0);
        if (n == 0) return IoStatus::Closed;
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                sys::sleepMs(1);
                continue;
            }
            return IoStatus::Error;
        }
        p += n;
        size -= static_cast<size_t>(n);
        deadline = sys::nowMs() + static_cast<uint64_t>(idleTimeoutMs);
    }
    return IoStatus::Ok;
}

bool sendAll(int fd, const void* buf, size_t size, const std::atomic<bool>& abort, int stallTimeoutMs) {
    const auto* p = static_cast<const uint8_t*>(buf);
    int stalledMs = 0;
    while (size > 0) {
        if (abort.load(std::memory_order_relaxed) || power::sleeping.load()) return false;

        pollfd pfd{fd, POLLOUT, 0};
        const int pr = poll(&pfd, 1, kPollSliceMs);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (pr == 0 || !(pfd.revents & POLLOUT)) {
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
            stalledMs += kPollSliceMs;
            if (stalledMs >= stallTimeoutMs) return false;
            continue;
        }

        const ssize_t n = send(fd, p, size, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                sys::sleepMs(1);
                if (++stalledMs >= stallTimeoutMs) return false;
                continue;
            }
            return false;
        }
        p += n;
        size -= static_cast<size_t>(n);
        stalledMs = 0;
    }
    return true;
}

bool SocketChannel::sendAll(const void* buf, size_t size, int stallTimeoutMs) {
    return net::sendAll(sock_.get(), buf, size, abort_, stallTimeoutMs);
}

IoStatus SocketChannel::recvExact(void* buf, size_t size, int idleTimeoutMs) {
    return net::recvExact(sock_.get(), buf, size, abort_, idleTimeoutMs);
}

void SocketChannel::kick() {
    if (sock_) shutdown(sock_.get(), SHUT_RDWR);
}
}
