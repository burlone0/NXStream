#pragma once

#include <cstddef>

namespace net {
enum class IoStatus { Ok, Closed, Timeout, Aborted, Error };
const char* toString(IoStatus s);

class Channel {
public:
    virtual ~Channel() = default;
    virtual bool sendAll(const void* buf, size_t size, int stallTimeoutMs) = 0;
    virtual IoStatus recvExact(void* buf, size_t size, int idleTimeoutMs) = 0;
    virtual void kick() = 0;
    virtual void close() = 0;
    virtual const char* name() const = 0;
};
}
