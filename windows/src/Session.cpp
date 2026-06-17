#include "Session.h"
#include "Encoder.h"
#include "HwEncoder.h"
#include "GuiApp.h"

#include <windows.h>
#include <winsock2.h>
#include <iostream>
#include <cstring>
#include <chrono>
#include <vector>
#include <string>
#include <atomic>

// Static atomic for generating unique session IDs (protocol v2: uint16_t on wire)
static std::atomic<uint16_t> s_next_session_id{1};

// ── Internal encoder abstraction ─────────────────────────────────────────────

struct IEncoderImpl {
    virtual bool Initialize(int w, int h, int fps, int kbps) = 0;
    virtual bool GetConfigPacket(std::vector<uint8_t>& out)  = 0;
    virtual bool EncodeFrame(const uint8_t* bgra,
                              std::vector<uint8_t>& nal_out,
                              bool& is_keyframe)              = 0;
    virtual void Close()                                      = 0;
    virtual ~IEncoderImpl()                                   = default;
};

#ifdef POCKETDISPLAY_ENABLE_X264
struct SwEncoderImpl : IEncoderImpl {
    Encoder enc;
    bool Initialize(int w, int h, int fps, int kbps) override { return enc.Initialize(w, h, fps, kbps); }
    bool GetConfigPacket(std::vector<uint8_t>& o) override    { return enc.GetConfigPacket(o); }
    bool EncodeFrame(const uint8_t* b, std::vector<uint8_t>& o, bool& kf) override {
        return enc.EncodeFrame(b, o, kf);
    }
    void Close() override { enc.Close(); }
};
#endif

struct HwEncoderImpl : IEncoderImpl {
    HwEncoder enc;
    bool Initialize(int w, int h, int fps, int kbps) override { return enc.Initialize(w, h, fps, kbps); }
    bool GetConfigPacket(std::vector<uint8_t>& o) override    { return enc.GetConfigPacket(o); }
    bool EncodeFrame(const uint8_t* b, std::vector<uint8_t>& o, bool& kf) override {
        return enc.EncodeFrame(b, o, kf);
    }
    void Close() override { enc.Close(); }
};

// ── Session ───────────────────────────────────────────────────────────────────

Session::Session(Config cfg) : cfg_(std::move(cfg)) {}

Session::~Session() { Stop(); }

// ── Start ─────────────────────────────────────────────────────────────────────

bool Session::Start() {
    if (running_.load()) return true;

    // ── Screen capture ────────────────────────────────────────────────────────
    // Determine which capture to use: external (process-lifetime) or owned.
    if (cfg_.external_capture) {
        capture_ptr_ = cfg_.external_capture;
        std::cout << "  [Session] Using EXTERNAL capture (adapter=" << cfg_.adapter_idx
                  << " output=" << cfg_.output_idx << ")...\n";
        // Verify external capture is initialized. Caller must initialize before
        // passing to Session; we don't call Initialize() to avoid races.
        if (!capture_ptr_->IsInitialized()) {
            std::cerr << "  [Session] ERROR: External capture not initialized!\n";
            capture_ptr_ = nullptr;
            return false;
        }
        // Verify adapter/output match (or trust caller's PickMonitor selection)
        if (capture_ptr_->GetAdapterIdx() != cfg_.adapter_idx ||
            capture_ptr_->GetOutputIdx() != cfg_.output_idx) {
            std::cout << "  [Session] NOTE: External capture has different adapter/output ("
                      << capture_ptr_->GetAdapterIdx() << "," << capture_ptr_->GetOutputIdx()
                      << ") vs requested (" << cfg_.adapter_idx << "," << cfg_.output_idx << ")\n";
            // This is OK - the caller (main.cpp) decides the output; external
            // capture was initialized to a specific output and we use it.
        }
    } else {
        capture_ptr_ = &capture_;
        std::cout << "  [Session] Initializing capture (adapter=" << cfg_.adapter_idx
                  << " output=" << cfg_.output_idx << ")...\n";
        if (!capture_ptr_->Initialize(cfg_.adapter_idx, cfg_.output_idx)) {
            std::cerr << "  [Session] ERROR: ScreenCapture init failed.\n";
            capture_ptr_ = nullptr;
            return false;
        }
    }
    const int cap_w = capture_ptr_->GetWidth();
    const int cap_h = capture_ptr_->GetHeight();
    std::cout << "  [Session] Capture ready: " << cap_w << "x" << cap_h
              << (cfg_.extend_mode ? "  (extended)" : "") << "\n";
    g_gui.capW.store(cap_w);
    g_gui.capH.store(cap_h);

    // ── Encoder ───────────────────────────────────────────────────────────────
    // Media Foundation encoder (HwEncoder) is the primary encoder for non-GPL builds.
    // It first tries hardware encoders (NVENC/QuickSync/VCE), then falls back to
    // software H.264 MFT - all without GPL code.
    auto mf_enc = std::make_unique<HwEncoderImpl>();
    if (mf_enc->Initialize(cap_w, cap_h, cfg_.target_fps, cfg_.bitrate_kbps)) {
        encoder_ = std::move(mf_enc);
        std::cout << "  [Session] Media Foundation encoder ready.\n";
    } else {
        std::cerr << "  [Session] ERROR: No H.264 encoder available.\n";
        std::cerr << "              Media Foundation could not find a hardware or software encoder.\n";
        std::cerr << "              This system may not support H.264 encoding.\n";
        strncpy_s(g_gui.statusMsg, "ERROR: No H.264 encoder available", 255);
        // Only release owned capture, not external
        if (capture_ptr_ == &capture_) {
            capture_ptr_->Release();
        }
        capture_ptr_ = nullptr;
        return false;
    }

    stream_w_.store(cap_w);
    stream_h_.store(cap_h);
    android_ready_.store(false);

    // Assign unique session ID for stale ACK detection (wraps at 65535, 0 reserved)
    session_id_ = s_next_session_id.fetch_add(1);
    if (session_id_ == 0) session_id_ = s_next_session_id.fetch_add(1);  // skip 0 (reserved)
    std::cout << "  [Session] Session ID " << session_id_ << " starting (android_ready=false).\n";

    // ── Touch receiver ────────────────────────────────────────────────────────
    // Set up ACK callback that validates session_id matches.
    // Protocol v2: Android echoes session_id in ACK packet bytes [6-7].
    // Idempotent: only transitions false->true once per session.
    touch_.SetAckCallback([this](uint16_t ack_session_id) {
        if (ack_session_id != session_id_) {
            // Stale ACK from previous session - discard
            std::cout << "  [Session] Stale ACK from session " << ack_session_id
                      << " ignored (current=" << session_id_ << ").\n";
            return;
        }
        // Idempotent transition: only set true if currently false
        bool expected = false;
        if (android_ready_.compare_exchange_strong(expected, true)) {
            g_gui.connected.store(true);
            strncpy_s(g_gui.statusMsg, "Android ready \u2014 streaming", 255);
            std::cout << "  [Session] ACK received for session " << session_id_
                      << " \u2014 android_ready false->true, streaming starts.\n";
        } else {
            // Duplicate ACK for this session - already ready, log but ignore
            std::cout << "  [Session] Duplicate ACK for session " << ack_session_id
                      << " ignored (already ready).\n";
        }
    });
    touch_.SetConnectCallback([]() {
        std::cout << "  [Session] Touch socket accepted.\n";
    });
    if (!touch_.Start(cfg_.touch_port)) {
        std::cerr << "  [Session] WARNING: touch receiver failed to start (non-fatal)\n";
    }
    if (cfg_.extend_mode) touch_.SetExtendedMonitor(capture_ptr_->GetMonitorRect());

    // ── Start worker threads ──────────────────────────────────────────────────
    running_.store(true);
    g_gui.streaming.store(true);
    strncpy_s(g_gui.statusMsg, "Waiting for Android\u2026", 255);

    resend_thread_ = std::thread(&Session::ResendLoop, this);
    cursor_thread_ = std::thread(&Session::CursorLoop, this);
    stream_thread_ = std::thread(&Session::StreamLoop, this);

    std::cout << "  [Session] Started: mode=" << (cfg_.extend_mode ? "Extended" : "Mirror")
              << "  transport=" << (cfg_.usb_mode ? "USB/TCP" : "WiFi/TCP") << "\n";
    return true;
}

// ── Stop ──────────────────────────────────────────────────────────────────────

void Session::Stop() {
    if (!running_.load() && !stream_thread_.joinable() &&
        !resend_thread_.joinable() && !cursor_thread_.joinable()) return;

    running_.store(false);
    // Close streamer so any blocked send() returns immediately.
    if (cfg_.streamer) cfg_.streamer->Close();

    if (stream_thread_.joinable())  stream_thread_.join();
    if (resend_thread_.joinable())  resend_thread_.join();
    if (cursor_thread_.joinable())  cursor_thread_.join();

    if (encoder_) { encoder_->Close(); encoder_.reset(); }
    // Only release owned capture, not external (process-lifetime capture reused across sessions)
    if (capture_ptr_ == &capture_) {
        capture_ptr_->Release();
    }
    capture_ptr_ = nullptr;
    touch_.Stop();

    g_gui.streaming.store(false);
    g_gui.connected.store(false);
    std::cout << "  [Session] Stopped.\n";
}

// ── Crash log helper ──────────────────────────────────────────────────────────

void Session::WriteCrashLog(const std::string& msg) {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string logPath(exePath);
    const auto sep = logPath.rfind('\\');
    logPath = (sep != std::string::npos) ? logPath.substr(0, sep + 1) : "";
    logPath += "PocketDisplay_crash.log";
    HANDLE hf = CreateFileA(logPath.c_str(), GENERIC_WRITE, 0, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) {
        SetFilePointer(hf, 0, nullptr, FILE_END);
        SYSTEMTIME st = {};
        GetLocalTime(&st);
        char ts[64] = {};
        sprintf_s(ts, sizeof(ts), "[%04d-%02d-%02d %02d:%02d:%02d] ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        const std::string line = std::string(ts) + msg + "\r\n";
        DWORD written = 0;
        WriteFile(hf, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
        CloseHandle(hf);
    }
}

// ── Resend loop (codec config + stream info until ACK; resumes on reconnect) ─

void Session::ResendLoop() {
    while (running_.load()) {
        // Stop resending once android_ready (ACK received). Resume if android_ready
        // is reset (encoder re-init on resolution change, or new session from reconnect).
        if (cfg_.streamer && !android_ready_.load()) {
            // Protocol v2: stream_info is 16 bytes: w(uint32), h(uint32), flags(uint32), session_id(uint16), padding(uint16)
            uint8_t dims[16];
            const uint32_t sw = htonl(static_cast<uint32_t>(stream_w_.load()));
            const uint32_t sh = htonl(static_cast<uint32_t>(stream_h_.load()));
            const uint32_t sf = htonl(cfg_.extend_mode ? 1u : 0u);
            const uint16_t sid = htons(session_id_);
            std::memcpy(dims,      &sw, 4);
            std::memcpy(dims + 4,  &sh, 4);
            std::memcpy(dims + 8,  &sf, 4);
            std::memcpy(dims + 12, &sid, 2);
            std::memset(dims + 14, 0, 2);  // padding
            cfg_.streamer->SendFrame(dims, 16, 0, pocketdisplay::FLAG_STREAM_INFO);

            std::vector<uint8_t> sps_pps;
            if (encoder_ && encoder_->GetConfigPacket(sps_pps) && !sps_pps.empty())
                cfg_.streamer->SendFrame(sps_pps.data(), sps_pps.size(),
                                         0, pocketdisplay::FLAG_CODEC_CONFIG);
        }
        for (int i = 0; i < 20 && running_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// ── Cursor loop ───────────────────────────────────────────────────────────────

void Session::CursorLoop() {
    const HCURSOR c_ibeam    = LoadCursor(NULL, IDC_IBEAM);
    const HCURSOR c_wait     = LoadCursor(NULL, IDC_WAIT);
    const HCURSOR c_cross    = LoadCursor(NULL, IDC_CROSS);
    const HCURSOR c_sizewe   = LoadCursor(NULL, IDC_SIZEWE);
    const HCURSOR c_sizens   = LoadCursor(NULL, IDC_SIZENS);
    const HCURSOR c_sizenwse = LoadCursor(NULL, IDC_SIZENWSE);
    const HCURSOR c_sizenesw = LoadCursor(NULL, IDC_SIZENESW);
    const HCURSOR c_sizeall  = LoadCursor(NULL, IDC_SIZEALL);
    const HCURSOR c_hand     = LoadCursor(NULL, IDC_HAND);
    const HCURSOR c_no       = LoadCursor(NULL, IDC_NO);
    const HCURSOR c_appstart = LoadCursor(NULL, IDC_APPSTARTING);

    const bool extended = cfg_.extend_mode;
    const RECT mon_rect = capture_ptr_->GetMonitorRect();
    const int  screen_w = GetSystemMetrics(SM_CXSCREEN);
    const int  screen_h = GetSystemMetrics(SM_CYSCREEN);

    POINT   last_pos    = {-1, -1};
    uint8_t last_type   = 0xFF;
    bool    last_on_mon = true;  // extended mode: tracks on-monitor state for hide-on-leave

    while (running_.load()) {
        POINT      pos = {};
        CURSORINFO ci  = {};
        ci.cbSize      = sizeof(ci);
        const bool got_pos = GetCursorPos(&pos);
        const bool got_ci  = GetCursorInfo(&ci);

        uint8_t cur_type = 0;
        if (got_ci) {
            const HCURSOR hc = ci.hCursor;
            if      (hc == c_ibeam)    cur_type = 1;
            else if (hc == c_wait)     cur_type = 2;
            else if (hc == c_cross)    cur_type = 3;
            else if (hc == c_sizewe)   cur_type = 4;
            else if (hc == c_sizens)   cur_type = 5;
            else if (hc == c_sizenwse) cur_type = 6;
            else if (hc == c_sizenesw) cur_type = 7;
            else if (hc == c_sizeall)  cur_type = 8;
            else if (hc == c_hand)     cur_type = 9;
            else if (hc == c_no)       cur_type = 10;
            else if (hc == c_appstart) cur_type = 11;
        }

        const bool pos_changed  = got_pos && (pos.x != last_pos.x || pos.y != last_pos.y);
        const bool type_changed = cur_type != last_type;

        if (pos_changed || type_changed) {
            if (pos_changed)  last_pos  = pos;
            if (type_changed) last_type = cur_type;

            bool    should_send = true;
            float   nx = 0.0f, ny = 0.0f;
            uint8_t send_type   = last_type;
            if (extended) {
                const bool on_mon =
                    last_pos.x >= mon_rect.left && last_pos.x < mon_rect.right &&
                    last_pos.y >= mon_rect.top  && last_pos.y < mon_rect.bottom;
                if (on_mon) {
                    last_on_mon = true;
                    const int mw = mon_rect.right  - mon_rect.left;
                    const int mh = mon_rect.bottom - mon_rect.top;
                    nx = static_cast<float>(last_pos.x - mon_rect.left) / mw;
                    ny = static_cast<float>(last_pos.y - mon_rect.top)  / mh;
                } else if (last_on_mon) {
                    // Transition: cursor just left extended region — send one hide packet.
                    last_on_mon = false;
                    send_type   = pocketdisplay::CURSOR_TYPE_HIDDEN;
                    // nx, ny remain 0.0f
                } else {
                    should_send = false;
                }
            } else {
                nx = static_cast<float>(last_pos.x) / screen_w;
                ny = static_cast<float>(last_pos.y) / screen_h;
            }

            if (should_send && cfg_.streamer) {
                uint32_t nx_be, ny_be;
                std::memcpy(&nx_be, &nx, 4); nx_be = htonl(nx_be);
                std::memcpy(&ny_be, &ny, 4); ny_be = htonl(ny_be);
                uint8_t payload[9];
                std::memcpy(payload,     &nx_be, 4);
                std::memcpy(payload + 4, &ny_be, 4);
                payload[8] = send_type;
                cfg_.streamer->SendFrame(payload, 9, 0, pocketdisplay::FLAG_CURSOR_POS);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

// ── Stream loop ───────────────────────────────────────────────────────────────

void Session::StreamLoop() {
    std::vector<uint8_t> bgra_buf, nal_buf;
    uint32_t frame_id    = 1;
    uint64_t frames_sent = 0;
    size_t   bytes_sent  = 0;

    const int  target_fps    = cfg_.target_fps;
    const auto frame_interval = std::chrono::microseconds(1'000'000 / target_fps);
    auto       next_frame     = std::chrono::steady_clock::now();
    const auto start_time     = next_frame;

    // Track first capture after android_ready becomes true (one-time log per session)
    bool capture_started_logged = false;
    const uint64_t my_session_id = session_id_;

    try {
    while (running_.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_frame)
            std::this_thread::sleep_for(next_frame - now);
        next_frame += frame_interval;

        if (!android_ready_.load()) {
            capture_started_logged = false;  // Reset in case android_ready toggles
            continue;
        }

        // One-time log: first time we start capturing for this session
        if (!capture_started_logged) {
            std::cout << "[PIPE] StreamLoop: android_ready=true for session " << my_session_id
                      << ", starting capture.\n";
            capture_started_logged = true;
        }

        int w = 0, h = 0;
        static std::atomic<int> capture_count{0};
        int cap_n = capture_count.fetch_add(1) + 1;
        if (cap_n <= 5) {
            std::cout << "[PIPE] captured frame " << cap_n << " (" << w << "x" << h << ")\n";
        }

        if (!capture_ptr_->CaptureFrame(bgra_buf, w, h)) {
            if (cap_n <= 5) std::cerr << "[PIPE] CaptureFrame failed\n";
            continue;
        }
        // Log actual dimensions after capture
        if (cap_n <= 5) {
            std::cout << "[PIPE] captured frame " << cap_n << " actual (" << w << "x" << h << ")\n";
        }

        const int cur_sw = stream_w_.load();
        const int cur_sh = stream_h_.load();

        if (w != cur_sw || h != cur_sh) {
            // Accumulate consecutive frames at new size (3-frame hysteresis).
            if (w == pending_resize_w_ && h == pending_resize_h_) {
                ++resize_stable_count_;
            } else {
                pending_resize_w_    = w;
                pending_resize_h_    = h;
                resize_stable_count_ = 1;
                std::cout << "  [Session] Resolution candidate: "
                          << cur_sw << "x" << cur_sh
                          << " -> " << w << "x" << h << " (1/3)\n";
            }
            if (resize_stable_count_ >= 3) {
                std::cout << "  [Session] Resolution stable \u2014 re-init encoder: "
                          << cur_sw << "x" << cur_sh
                          << " -> " << w << "x" << h << "\n";
                encoder_->Close();
                if (!encoder_->Initialize(w, h, target_fps, cfg_.bitrate_kbps)) {
                    std::cerr << "  [Session] Encoder re-init failed\n";
                    pending_resize_w_    = 0;
                    pending_resize_h_    = 0;
                    resize_stable_count_ = 0;
                    continue;
                }
                stream_w_.store(w);
                stream_h_.store(h);
                android_ready_.store(false);  // force codec-config resend
                std::cout << "  [Session] Resolution changed " << session_id_
                          << " - android_ready reset to false (waiting for new ACK).\n";
                pending_resize_w_    = 0;
                pending_resize_h_    = 0;
                resize_stable_count_ = 0;
            }
            continue;
        } else if (resize_stable_count_ > 0) {
            resize_stable_count_ = 0;
            pending_resize_w_    = 0;
            pending_resize_h_    = 0;
        }

        bool is_keyframe = false;
        static std::atomic<int> encode_call_count{0};
        int ec_n = encode_call_count.fetch_add(1) + 1;
        if (ec_n <= 5) {
            std::cout << "[PIPE] calling EncodeFrame " << ec_n << "\n";
        }

        if (!encoder_->EncodeFrame(bgra_buf.data(), nal_buf, is_keyframe)) {
            if (ec_n <= 5) std::cerr << "[PIPE] EncodeFrame returned FALSE\n";
            continue;
        }

        if (ec_n <= 5) {
            std::cout << "[PIPE] EncodeFrame returned nal=" << nal_buf.size()
                      << " keyframe=" << (is_keyframe ? "y" : "n") << "\n";
        }

        if (nal_buf.empty()) {
            if (ec_n <= 5) std::cout << "[PIPE] nal_buf empty (pipelining)\n";
            continue;
        }

        const uint8_t flags = is_keyframe ? pocketdisplay::FLAG_KEYFRAME
                                           : pocketdisplay::FLAG_NONE;
        bool send_ok = true;
        if (cfg_.streamer) {
            send_ok = cfg_.streamer->SendFrame(nal_buf.data(), nal_buf.size(), frame_id++, flags);
            static std::atomic<int> send_count{0};
            int send_n = send_count.fetch_add(1) + 1;
            if (send_n <= 5 || !send_ok) {
                std::cout << "[PIPE] sent " << nal_buf.size() << " bytes to socket"
                          << " (frame " << send_n << ")\n";
            }
        }
        if (!send_ok) {
            static std::atomic<int> send_error_count{0};
            int err_n = send_error_count.fetch_add(1) + 1;
            std::cerr << "[PIPE] ERROR send failed (" << err_n << " total errors)\n";
            std::cout << "  [Session] SendFrame failed \u2014 client disconnected.\n";
            running_.store(false);
            break;
        }

        ++frames_sent;
        bytes_sent += nal_buf.size();

        const double elapsed_s = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_time).count();
        if (elapsed_s >= 1.0 &&
            frames_sent % static_cast<uint64_t>(target_fps * 5) == 0) {
            const int cur_fps  = static_cast<int>(frames_sent / elapsed_s + 0.5);
            const int cur_kbps = static_cast<int>(bytes_sent * 8.0 / 1000.0 / elapsed_s);
            fps_.store(cur_fps);
            kbps_.store(cur_kbps);
            g_gui.fps.store(cur_fps);
            g_gui.bitrateKbps.store(cur_kbps);
        }
    }
    } catch (const std::exception& e) {
        const std::string msg = std::string("Exception in Session::StreamLoop: ") + e.what();
        std::cerr << "  [Session] CRASH: " << msg << "\n";
        WriteCrashLog(msg);
        running_.store(false);
    } catch (...) {
        const std::string msg = "Unknown exception in Session::StreamLoop";
        std::cerr << "  [Session] CRASH: " << msg << "\n";
        WriteCrashLog(msg);
        running_.store(false);
    }
}
