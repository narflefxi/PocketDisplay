#pragma once
#include "Protocol.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <string>
#include <vector>

class UdpStreamer {
public:
    UdpStreamer() = default;
    ~UdpStreamer();

    bool Initialize(const std::string& target_ip,
                    uint16_t port = pocketdisplay::DEFAULT_PORT);
    bool SendFrame(const uint8_t* data, size_t size,
                   uint32_t frame_id, uint8_t flags);
    void Close();

private:
    SOCKET      sock_ = INVALID_SOCKET;
    sockaddr_in dest_ = {};
};
