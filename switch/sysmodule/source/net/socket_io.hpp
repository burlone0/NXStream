#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "net/channel.hpp"

namespace net {

class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket() { reset(); }
    Socket(Socket&& o) noexcept : fd_(o.release()) {}
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) reset(o.release());
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }
    int release() {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void reset(int fd = -1);

private:
    int fd_ = -1;
};

IoStatus recvExact(int fd, void* buf, size_t size, const std::atomic<bool>& abort, int idleTimeoutMs);
bool sendAll(int fd, const void* buf, size_t size, const std::atomic<bool>& abort, int stallTimeoutMs);

class SocketChannel : public Channel {
public:
    SocketChannel(int fd, const std::atomic<bool>& abort) : sock_(fd), abort_(abort) {}

    bool sendAll(const void* buf, size_t size, int stallTimeoutMs) override;
    IoStatus recvExact(void* buf, size_t size, int idleTimeoutMs) override;
    void kick() override;
    void close() override { sock_.reset(); }
    const char* name() const override { return "tcp"; }

private:
    Socket sock_;
    const std::atomic<bool>& abort_;
};
}
