#pragma once
#include <mutex>
#include <winsock2.h>
#include <windows.h>
#include <cstdint>
#include <string>
#include <atomic>
#include <functional>
#include <thread>

// Receives touch and keyboard events from the Android app.
// Phase 3: always TCP — Android connects to Windows:7778.
// USB: via adb reverse tcp:7778 tcp:7778 (loopback).
// WiFi: direct TCP to Windows IP:7778.
class TouchReceiver {
public:
    TouchReceiver() = default;
    ~TouchReceiver();

    // Listen as TCP server on the given port.
    bool Start(uint16_t port = 7778);
    void Stop();

    // Called when Android sends a codec-ready ACK (touch packet type 8).
    // Argument is the sender's IP address (empty string in USB/TCP mode).
    void SetAckCallback(std::function<void(const std::string&)> cb) { ack_cb_ = std::move(cb); }

    // Called each time Android connects (or reconnects) on the touch socket.
    void SetConnectCallback(std::function<void()> cb) { connect_cb_ = std::move(cb); }

    // Enable extended-display coordinate mapping for touch injection.
    // rect is the monitor's desktop coordinates (from DXGI_OUTPUT_DESC).
    void SetExtendedMonitor(RECT rect) { extended_mode_ = true; mon_rect_ = rect; }

private:
    void TcpAcceptLoop();
    void TcpClientLoop(SOCKET client);
    void ProcessPacket(const uint8_t* buf, int len);
    void ApplyTouchEvent(uint8_t type, float nx, float ny) const;
    void InjectUnicodeChar(uint32_t codepoint) const;
    void InjectVirtualKey(uint16_t vk, bool key_down) const;

    std::function<void(const std::string&)> ack_cb_;
    std::function<void()> connect_cb_;
    SOCKET            sock_         = INVALID_SOCKET;
    SOCKET            client_sock_  = INVALID_SOCKET;  // active TCP client
    std::mutex        client_mu_;                       // protects client_sock_
    std::thread       thread_;
    std::atomic<bool> running_{false};
    bool              extended_mode_= false;
    RECT              mon_rect_     = {};
};
