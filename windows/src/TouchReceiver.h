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
    // Protocol v2: Android echoes session_id in bytes [6-7] of the ACK packet.
    // Callback receives the session_id from the ACK; Session validates it matches current.
    void SetAckCallback(std::function<void(uint16_t)> cb) { ack_cb_ = std::move(cb); }

    // Called each time Android connects (or reconnects) on the touch socket.
    void SetConnectCallback(std::function<void()> cb) { connect_cb_ = std::move(cb); }

    // Enable extended-display coordinate mapping for touch injection.
    // rect is the monitor's desktop coordinates (from DXGI_OUTPUT_DESC).
    void SetExtendedMonitor(RECT rect) { SetSessionContext(true, rect); }

    // Update per-session touch-mapping context. Used by the process-lifetime
    // receiver so each new Session can refresh extended/mirror mapping without
    // re-binding the port. Thread-safe.
    void SetSessionContext(bool extended, RECT rect) {
        std::lock_guard<std::mutex> lk(ctx_mu_);
        extended_mode_ = extended;
        mon_rect_      = rect;
    }

private:
    void TcpAcceptLoop();
    void TcpClientLoop(SOCKET client);
    void ProcessPacket(const uint8_t* buf, int len);
    void ApplyTouchEvent(uint8_t type, float nx, float ny) const;
    void InjectUnicodeChar(uint32_t codepoint) const;
    void InjectVirtualKey(uint16_t vk, bool key_down) const;

    std::function<void(uint16_t)> ack_cb_;
    std::function<void()> connect_cb_;
    SOCKET            sock_         = INVALID_SOCKET;
    SOCKET            client_sock_  = INVALID_SOCKET;  // active TCP client
    std::mutex        client_mu_;                       // protects client_sock_
    std::thread       thread_;
    std::atomic<bool> running_{false};
    // Per-session touch-mapping context (extended-display coordinate remap).
    // Protected by ctx_mu_ because the process-lifetime receiver runs across
    // sessions: the accept/client thread reads it while a new Session writes it.
    mutable std::mutex ctx_mu_;
    bool              extended_mode_= false;
    RECT              mon_rect_     = {};
};
