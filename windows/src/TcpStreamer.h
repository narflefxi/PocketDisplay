#pragma once
#include "Protocol.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <string>
#include <vector>

// TCP stream sender for USB/ADB mode.
// Android acts as TCP server on port 7777.
// Set up ADB first: adb forward tcp:7777 tcp:7777
// Then Windows connects to 127.0.0.1:7777.
class TcpStreamer {
public:
    TcpStreamer() = default;
    ~TcpStreamer();

    bool Initialize(const std::string& target_ip,
                    uint16_t port = pocketdisplay::DEFAULT_PORT);
    bool SendFrame(const uint8_t* data, size_t size,
                   uint32_t frame_id, uint8_t flags);
    void Close();

private:
    static bool SendAll(SOCKET s, const uint8_t* buf, int len);
    SOCKET sock_ = INVALID_SOCKET;
};
