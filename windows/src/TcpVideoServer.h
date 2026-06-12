#pragma once

#include "Protocol.h"
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

// TCP server on PC. Used for USB streaming (Android→127.0.0.1 via adb reverse)
// and for WiFi HELLO-only connections (mode handshake before UDP streaming).
//
// Framing per message: [4-byte big-endian length L][L bytes payload]
// payload = [uint8 type][data...]
//   type 0 = H.264 access unit (same bytes as UDP assembled frame)  Windows→Android
//   type 1 = codec config (Annex-B SPS+PPS)                         Windows→Android
//   type 2 = stream info (8 bytes: uint32 BE width, uint32 BE height) Windows→Android
//   type 3 = cursor (9 bytes: float BE nx, float BE ny, uint8 type)  Windows→Android
//   type 4 = HELLO handshake (Android→Windows, FIRST message after connect):
//            [version=1][mode 0=mirror/1=extend][w uint32BE][h uint32BE] = 11 bytes
//
// HELLO replaces the old plain-text "POCKETDISPLAY_MODE:..." line.
// Connections that close before sending a valid HELLO are treated as TCP probes
// and discarded without touching client_sock_, mode_value_, or reconnect_cb_.
// WaitForMode() blocks until a valid HELLO is received or timeout (→ Mirror).

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

    // Called each time Android (re)connects — use to reset android_ready.
    void SetReconnectCallback(std::function<void()> cb) { reconnect_cb_ = std::move(cb); }

    // Android screen dimensions from mode handshake (0 if not sent).
    void GetAndroidSize(int& w, int& h) const { w = android_w_; h = android_h_; }

    void Close();

private:
    static bool SendAll(SOCKET s, const uint8_t* buf, int len);
    void        AcceptLoop();

    SOCKET              listen_sock_  = INVALID_SOCKET;
    SOCKET              client_sock_  = INVALID_SOCKET;
    std::mutex          client_mu_;
    std::thread         accept_thread_;
    std::atomic<bool>   running_{false};
    std::function<void()> reconnect_cb_;

    std::mutex              mode_mu_;
    std::condition_variable mode_cv_;
    int                     mode_value_ = -1;  // -1=waiting, 0=mirror, 1=extend
    int                     android_w_  = 0;
    int                     android_h_  = 0;
};
