#pragma once
#include <winsock2.h>
#include <windows.h>
#include <cstdint>
#include <atomic>
#include <functional>
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

    // Called once when Android sends a codec-ready ACK (touch packet type 8).
    void SetAckCallback(std::function<void()> cb) { ack_cb_ = std::move(cb); }

    // Enable extended-display coordinate mapping for touch injection.
    // rect is the monitor's desktop coordinates (from DXGI_OUTPUT_DESC).
    void SetExtendedMonitor(RECT rect) { extended_mode_ = true; mon_rect_ = rect; }

private:
    void UdpLoop();
    void TcpAcceptLoop();
    void TcpClientLoop(SOCKET client);
    void ProcessPacket(const uint8_t* buf, int len);
    void ApplyTouchEvent(uint8_t type, float nx, float ny) const;
    void InjectUnicodeChar(uint32_t codepoint) const;
    void InjectVirtualKey(uint16_t vk, bool key_down) const;

    std::function<void()> ack_cb_;
    SOCKET            sock_         = INVALID_SOCKET;
    std::thread       thread_;
    std::atomic<bool> running_{false};
    bool              tcp_mode_     = false;
    bool              extended_mode_= false;
    RECT              mon_rect_     = {};
};
