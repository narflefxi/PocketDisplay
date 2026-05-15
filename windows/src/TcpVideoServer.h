#pragma once

#include "Protocol.h"
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <condition_variable>
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
//   type 3 = cursor (9 bytes: float BE nx, float BE ny, uint8 cursor_type)
//
// Protocol extension: Android sends "POCKETDISPLAY_MODE:mirror\n" or
// "POCKETDISPLAY_MODE:extend\n" as a plain-text line immediately after
// connecting, before the framed video stream begins. WaitForMode() blocks
// until that line is received or the timeout expires (defaults to Mirror).

class TcpVideoServer {
public:
    TcpVideoServer() = default;
    ~TcpVideoServer();

    // Starts listen thread; returns false if bind/listen fails.
    bool StartListen(uint16_t port);

    // Blocks until Android connects and sends mode, or timeout_ms elapses.
    // Returns true if mode was received; extend_out=false → Mirror default.
    bool WaitForMode(int timeout_ms, bool& extend_out);

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
    std::atomic<bool>   running_{false};

    std::mutex              mode_mu_;
    std::condition_variable mode_cv_;
    int                     mode_value_ = -1;  // -1=waiting, 0=mirror, 1=extend
};
