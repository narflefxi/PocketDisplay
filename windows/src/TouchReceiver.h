#pragma once
#include <winsock2.h>
#include <cstdint>
#include <atomic>
#include <thread>

// Receives touch and keyboard events from the Android app.
// UDP mode (WiFi): Android sends datagrams to Windows:7778.
// TCP mode (USB):  Android connects to Windows:7778 (via adb reverse tcp:7778 tcp:7778).
class TouchReceiver {
public:
    TouchReceiver() = default;
    ~TouchReceiver();

    // tcp_mode=true: listen as TCP server (USB/ADB).
    // tcp_mode=false: listen as UDP server (WiFi).
    bool Start(uint16_t port = 7778, bool tcp_mode = false);
    void Stop();

private:
    void UdpLoop();
    void TcpAcceptLoop();
    void TcpClientLoop(SOCKET client);
    void ProcessPacket(const uint8_t* buf, int len);
    void ApplyTouchEvent(uint8_t type, float nx, float ny) const;
    void InjectUnicodeChar(uint32_t codepoint) const;
    void InjectVirtualKey(uint16_t vk, bool key_down) const;

    SOCKET            sock_     = INVALID_SOCKET;
    std::thread       thread_;
    std::atomic<bool> running_{false};
    bool              tcp_mode_ = false;
};
