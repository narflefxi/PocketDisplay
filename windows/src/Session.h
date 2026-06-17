#pragma once

#include "Protocol.h"
#include "ScreenCapture.h"
#include "TouchReceiver.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// ── Transport abstraction ─────────────────────────────────────────────────────
// Implementation: DirectSocketStreamer (TCP, used for both USB and WiFi),
// defined in main.cpp.  Session only calls SendFrame / Close.
struct IStreamer {
    virtual bool SendFrame(const uint8_t* data, size_t size,
                           uint32_t frame_id, uint8_t flags) = 0;
    virtual void UpdateTarget(const std::string& /*ip*/) {}
    virtual void Close() = 0;
    virtual ~IStreamer() = default;
};

// Forward-declared so Session.h doesn't pull in Encoder.h / HwEncoder.h.
// Fully defined in Session.cpp.
struct IEncoderImpl;

// ── Session ───────────────────────────────────────────────────────────────────
// Owns one streaming session: capture → encode → send.
// Created per-HELLO, torn down on disconnect or new HELLO.
//
// SCREEN CAPTURE LIFECYCLE:
// By default, Session owns its ScreenCapture and creates/destroys it per-session.
// For process-lifetime capture (reconnect without DuplicateOutput), pass an
// external ScreenCapture pointer via cfg.external_capture. In this mode:
//   - Session does NOT call Initialize() on the external capture (caller must)
//   - Session does NOT call Release() on the external capture during Stop()
//   - The external capture outlives the Session and is reused across reconnects
class Session {
public:
    struct Config {
        int      adapter_idx  = 0;    // DXGI adapter (from PickMonitor)
        int      output_idx   = 0;    // DXGI output  (from PickMonitor)
        int      monitor_num  = 1;    // 1-based (logging only)
        bool     extend_mode  = false;
        bool     hw_enc       = true; // Media Foundation encoder (HW preferred, SW fallback)
        int      android_w    = 0;    // Android screen dims from HELLO
        int      android_h    = 0;
        int      bitrate_kbps = 30000;
        int      target_fps   = 60;
        bool     usb_mode     = false;  // TCP touch (USB) vs UDP touch (WiFi)
        uint16_t touch_port   = 7778;
        std::unique_ptr<IStreamer> streamer;  // owned; Close() called on teardown
        // External capture (process-lifetime) — if set, Session borrows this
        // instead of creating its own. Session will NOT call Initialize() or
        // Release() on the external capture.
        ScreenCapture* external_capture = nullptr;
    };

    explicit Session(Config cfg);
    ~Session();  // defined in Session.cpp (IEncoderImpl is incomplete here)

    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    // Initialize capture + encoder; start worker threads.
    // Returns false if capture or encoder init fails (caller should discard).
    bool Start();

    // Signal all threads to stop and join them; idempotent.
    void Stop();

    bool IsRunning() const { return running_.load(); }

    // Live stats readable from any thread after Start().
    int GetFps()  const { return fps_.load();  }
    int GetKbps() const { return kbps_.load(); }

private:
    void StreamLoop();
    void ResendLoop();
    void CursorLoop();
    static void WriteCrashLog(const std::string& msg);

    Config      cfg_;
    ScreenCapture                 capture_;      // Owned capture (unused if external set)
    ScreenCapture*                capture_ptr_ = nullptr;  // Points to capture_ or external
    std::unique_ptr<IEncoderImpl> encoder_;
    TouchReceiver                 touch_;

    std::atomic<bool>  running_       {false};
    std::atomic<bool>  android_ready_ {false};

    // Per-session ID to detect stale ACKs from previous sessions during reconnect.
    // Each new Session gets a unique ID; ACK callback validates it matches.
    uint64_t session_id_ = 0;

    std::thread stream_thread_;
    std::thread resend_thread_;
    std::thread cursor_thread_;

    // Stream state for resize hysteresis
    std::atomic<int>  stream_w_{0}, stream_h_{0};
    int  pending_resize_w_    = 0;
    int  pending_resize_h_    = 0;
    int  resize_stable_count_ = 0;

    std::atomic<int>  fps_{0};
    std::atomic<int>  kbps_{0};
};
