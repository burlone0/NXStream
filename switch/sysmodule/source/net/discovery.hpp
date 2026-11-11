#pragma once

#include <cstdint>
#include <string>

class ViewerLink;

class DiscoveryResponder {
public:
    explicit DiscoveryResponder(ViewerLink& link) : link_(link) {}
    bool start(uint16_t udpPort, uint16_t tcpPort, std::string consoleName);

private:
    void run();
    void periodicTasks(int fd);
    std::string announcement() const;

    uint64_t nextBeaconMs_ = 0;
    int netUp_ = -1;
    uint32_t beaconFailures_ = 0;

    ViewerLink& link_;
    uint16_t udpPort_ = 0;
    uint16_t tcpPort_ = 0;
    std::string consoleName_;
};
