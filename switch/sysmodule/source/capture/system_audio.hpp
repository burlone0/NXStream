#pragma once

#include <atomic>
#include <cstdint>

class ViewerLink;

class SystemAudio {
public:
    explicit SystemAudio(ViewerLink& link) : link_(link) {}

    bool start();
    bool active() const;

private:
    void loop();
    bool record();
    bool open();
    void close();

    ViewerLink& link_;
    std::atomic<uint64_t> lastBufferMs_{0};
};
