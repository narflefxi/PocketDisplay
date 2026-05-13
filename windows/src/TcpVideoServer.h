#pragma once

#include "Protocol.h"
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

// USB mode: TCP server on PC. Android connects to 127.0.0.1:port on device
// with `adb reverse tcp:port tcp:port`.
//
// Framing per message: [4-byte big-endian length L][L bytes payload]
// payload = [uint8 type][data...]
//   type 0 = H.264 access unit (same bytes as UDP assembled frame)
//   type 1 = codec config (Annex-B SPS+PPS)
//   type 2 = stream info (8 bytes: uint32 BE width, uint32 BE height)
//   type 3 = cursor (8 bytes: float BE nx, float BE ny)

class TcpVideoServer {
public:
    TcpVideoServer() = default;
    ~TcpVideoServer();

    // Starts listen thread; returns false if bind/listen fails.
    bool StartListen(uint16_t port);

    // Maps legacy PDSM SendFrame flags to USB TCP messages. Drops if no client yet.
    bool SendFrame(const uint8_t* data, size_t size, uint32_t frame_id, uint8_t flags);

    void Close();

private:
    static bool SendAll(SOCKET s, const uint8_t* buf, int len);
    void        AcceptLoop();

    SOCKET              listen_sock_  = INVALID_SOCKET;
    SOCKET              client_sock_  = INVALID_SOCKET;
    std::mutex          client_mu_;
    std::thread         accept_thread_;
    std::atomic<bool> running_{false};
};
