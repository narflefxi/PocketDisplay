#include "ScreenCapture.h"
#include "Encoder.h"
#include "HwEncoder.h"
#include "UdpStreamer.h"
#include "TcpVideoServer.h"
#include "AdbUsbSetup.h"
#include "TouchReceiver.h"
#include "Protocol.h"

#include <windows.h>
#include <iostream>
#include <string>
#include <sstream>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <cstring>
#include <iomanip>

// ── Console color helpers ───────────────────────────────────────────────────

static HANDLE g_con = INVALID_HANDLE_VALUE;
enum Color : WORD { CYAN = 11, GREEN = 10, YELLOW = 14, RED = 12, WHITE = 15, GRAY = 8 };

static void SetColor(Color c) {
    if (g_con != INVALID_HANDLE_VALUE) SetConsoleTextAttribute(g_con, c);
}
static void ResetColor() { SetColor(WHITE); }

static void PrintBanner() {
    SetColor(CYAN);
    std::cout << R"(
  ____            _        _   ____  _           _
 |  _ \ ___   ___| | _____| |_|  _ \(_)___ _ __ | | __ _ _   _
 | |_) / _ \ / __| |/ / _ \ __| | | | / __| '_ \| |/ _` | | | |
 |  __/ (_) | (__|   <  __/ |_| |_| | \__ \ |_) | | (_| | |_| |
 |_|   \___/ \___|_|\_\___|\__|____/|_|___/ .__/|_|\__,_|\__, |
                                           |_|            |___/
)" "\n";
    ResetColor();
}

static void PrintUsage(const char* exe) {
    std::cout << "Usage:\n";
    SetColor(YELLOW);
    std::cout << "  WiFi: " << exe << " <android_ip> [port] [bitrate_kbps] [fps] [--hw] [--extend | --monitor=N]\n";
    std::cout << "  USB:  " << exe << " --usb [--hw] [--extend | --monitor=N]\n";
    ResetColor();
    std::cout << "\nOptions:\n"
              << "  --usb        USB mode: adb reverse + TCP video server (device connects to PC)\n"
              << "  --hw         Hardware encoder (NVENC/Intel/AMD — falls back to x264)\n"
              << "  --extend     Capture last DXGI output (virtual/extended display)\n"
              << "  --monitor=N  Capture monitor N (1-based index, e.g. --monitor=2)\n"
              << "\nExamples:\n"
              << "  " << exe << " 192.168.1.100\n"
              << "  " << exe << " 192.168.1.100 7777 8000 60 --hw\n"
              << "  " << exe << " --usb --hw\n"
              << "  " << exe << " --usb --extend\n"
              << "  " << exe << " --usb --monitor=2\n";
}

// ── DXGI output enumerator ────────────────────────────────────────────────────
// Prints available monitors on adapter 0 and returns the output index to use.
static int PickOutputIdx(bool extend, int monitor_num) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> dev;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                  0, nullptr, 0, D3D11_SDK_VERSION,
                                  &dev, nullptr, nullptr))) {
        std::cerr << "      WARNING: Could not enumerate outputs, defaulting to 0\n";
        return monitor_num > 0 ? std::max(0, monitor_num - 1) : 0;
    }
    ComPtr<IDXGIDevice> dxgi_dev;
    dev.As(&dxgi_dev);
    ComPtr<IDXGIAdapter> adapter;
    dxgi_dev->GetAdapter(&adapter);

    int last_idx = 0;
    for (int i = 0; ; ++i) {
        ComPtr<IDXGIOutput> out;
        if (FAILED(adapter->EnumOutputs(i, &out))) break;
        DXGI_OUTPUT_DESC d = {};
        out->GetDesc(&d);
        char nm[64] = {};
        WideCharToMultiByte(CP_UTF8, 0, d.DeviceName, -1, nm, sizeof(nm), nullptr, nullptr);
        SetColor(GRAY);
        std::cout << "      [" << (i + 1) << "] " << nm
                  << "  " << (d.DesktopCoordinates.right  - d.DesktopCoordinates.left)
                  << "x" << (d.DesktopCoordinates.bottom - d.DesktopCoordinates.top)
                  << " @(" << d.DesktopCoordinates.left << "," << d.DesktopCoordinates.top << ")"
                  << (d.AttachedToDesktop ? "" : "  [inactive]") << "\n";
        ResetColor();
        last_idx = i;
    }
    if (monitor_num > 0) return std::max(0, monitor_num - 1);
    if (extend)          return last_idx;
    return 0;
}

// ── Signal ──────────────────────────────────────────────────────────────────

static std::atomic<bool> g_running{true};
static void SignalHandler(int) { g_running = false; }

// ── Live stats line (in-place update) ───────────────────────────────────────

static void PrintStats(double fps, double kbps, size_t frame_bytes,
                        const char* enc_name, const char* mode_name) {
    std::ostringstream oss;
    oss << "\r  [" << mode_name << "/" << enc_name << "]  "
        << "FPS: " << std::setw(3) << static_cast<int>(fps)
        << "  Bitrate: " << std::setw(6) << static_cast<int>(kbps) << " kbps"
        << "  Frame: " << std::setw(6) << frame_bytes << " B  ";
    std::cout << oss.str() << std::flush;
}

// ── Encoder wrapper: either HW or SW ────────────────────────────────────────

struct IEncoder {
    virtual bool Initialize(int w, int h, int fps, int kbps) = 0;
    virtual bool GetConfigPacket(std::vector<uint8_t>& out) = 0;
    virtual bool EncodeFrame(const uint8_t* bgra,
                              std::vector<uint8_t>& nal_out, bool& kf) = 0;
    virtual void Close() = 0;
    virtual ~IEncoder() = default;
};

struct SwEncoderWrap : IEncoder {
    Encoder enc;
    bool Initialize(int w, int h, int fps, int kbps) override { return enc.Initialize(w,h,fps,kbps); }
    bool GetConfigPacket(std::vector<uint8_t>& o) override    { return enc.GetConfigPacket(o); }
    bool EncodeFrame(const uint8_t* b, std::vector<uint8_t>& o, bool& kf) override { return enc.EncodeFrame(b,o,kf); }
    void Close() override { enc.Close(); }
};

struct HwEncoderWrap : IEncoder {
    HwEncoder enc;
    bool Initialize(int w, int h, int fps, int kbps) override { return enc.Initialize(w,h,fps,kbps); }
    bool GetConfigPacket(std::vector<uint8_t>& o) override    { return enc.GetConfigPacket(o); }
    bool EncodeFrame(const uint8_t* b, std::vector<uint8_t>& o, bool& kf) override { return enc.EncodeFrame(b,o,kf); }
    void Close() override { enc.Close(); }
};

// ── Streamer wrapper: UDP or TCP ────────────────────────────────────────────

struct IStreamer {
    virtual bool Initialize(const std::string& ip, uint16_t port) = 0;
    virtual bool SendFrame(const uint8_t* d, size_t sz,
                            uint32_t id, uint8_t flags) = 0;
    virtual void Close() = 0;
    virtual ~IStreamer() = default;
};

struct UdpStreamerWrap : IStreamer {
    UdpStreamer s;
    bool Initialize(const std::string& ip, uint16_t port) override { return s.Initialize(ip,port); }
    bool SendFrame(const uint8_t* d, size_t sz, uint32_t id, uint8_t f) override { return s.SendFrame(d,sz,id,f); }
    void Close() override { s.Close(); }
};

// USB: PC listens; Android connects via adb reverse. Length-prefixed messages (see TcpVideoServer).
struct TcpVideoServerWrap : IStreamer {
    TcpVideoServer s;
    bool Initialize(const std::string& /*ip*/, uint16_t port) override { return s.StartListen(port); }
    bool SendFrame(const uint8_t* d, size_t sz, uint32_t id, uint8_t f) override {
        return s.SendFrame(d, sz, id, f);
    }
    void Close() override { s.Close(); }
};

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    g_con = GetStdHandle(STD_OUTPUT_HANDLE);
    PrintBanner();

    bool usb_mode     = false;
    bool hw_enc       = false;
    bool extend_mode  = false;
    int  monitor_num  = 0;       // 1-based (0 = default = primary)
    std::string target_ip;
    uint16_t port         = pocketdisplay::DEFAULT_PORT;
    uint16_t touch_port   = 7778;
    int      bitrate_kbps = 8000;
    int      target_fps   = 60;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg == "--usb")    { usb_mode    = true; }
        else if (arg == "--hw")     { hw_enc      = true; }
        else if (arg == "--extend") { extend_mode = true; }
        else if (arg.rfind("--monitor=", 0) == 0) { monitor_num = std::stoi(arg.substr(10)); }
        else if (arg == "--help" || arg == "-h") { PrintUsage(argv[0]); return 0; }
        else if (arg == "--port"    && i+1 < argc) port         = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--fps"     && i+1 < argc) target_fps   = std::stoi(argv[++i]);
        else if (arg == "--bitrate" && i+1 < argc) bitrate_kbps = std::stoi(argv[++i]);
        else if (arg[0] != '-' && target_ip.empty()) target_ip = arg;
        // Legacy positional: ip port bitrate fps
        else if (arg[0] != '-' && !target_ip.empty()) {
            if (port == pocketdisplay::DEFAULT_PORT) port = static_cast<uint16_t>(std::stoi(arg));
            else if (bitrate_kbps == 8000)           bitrate_kbps = std::stoi(arg);
            else if (target_fps == 60)               target_fps   = std::stoi(arg);
        }
    }

    if (!usb_mode && target_ip.empty()) {
        PrintUsage(argv[0]);
        return 1;
    }
    if (usb_mode) target_ip = "127.0.0.1";

    const bool extended = extend_mode || monitor_num > 0;

    std::signal(SIGINT, SignalHandler);

    const char* mode_name = usb_mode ? (extended ? "USB/EXT"  : "USB")
                                     : (extended ? "WiFi/EXT" : "WiFi");
    const char* enc_name  = hw_enc   ? "HW"  : "x264";

    // ── Screen capture ───────────────────────────────────────────────────────

    SetColor(CYAN);
    std::cout << "[1/4] Screen capture...\n";
    int output_idx = 0;
    if (extended) {
        std::cout << "      Available monitors (adapter 0):\n";
        output_idx = PickOutputIdx(extend_mode, monitor_num);
    }
    ResetColor();

    ScreenCapture capture;
    if (!capture.Initialize(0, output_idx)) {
        SetColor(RED);
        std::cerr << "  ERROR: DXGI Desktop Duplication failed (monitor " << (output_idx + 1) << ").\n";
        ResetColor();
        return 1;
    }
    SetColor(GREEN);
    std::cout << "      Display [" << (output_idx + 1) << "]: "
              << capture.GetWidth() << "x" << capture.GetHeight();
    if (extended) std::cout << "  (extended mode)";
    std::cout << "\n";
    ResetColor();

    // ── Encoder ─────────────────────────────────────────────────────────────

    SetColor(CYAN);
    std::cout << "[2/4] Encoder (" << enc_name << ", "
              << bitrate_kbps << " kbps, " << target_fps << " fps)...\n";
    ResetColor();

    std::unique_ptr<IEncoder> encoder;
    if (hw_enc) {
        auto hw = std::make_unique<HwEncoderWrap>();
        if (hw->Initialize(capture.GetWidth(), capture.GetHeight(),
                            target_fps, bitrate_kbps)) {
            encoder = std::move(hw);
            enc_name = "HW";
        } else {
            std::cout << "      Falling back to x264.\n";
            hw_enc = false;
        }
    }
    if (!encoder) {
        auto sw = std::make_unique<SwEncoderWrap>();
        if (!sw->Initialize(capture.GetWidth(), capture.GetHeight(),
                             target_fps, bitrate_kbps)) {
            SetColor(RED);
            std::cerr << "  ERROR: x264 encoder init failed.\n";
            ResetColor();
            return 1;
        }
        enc_name = "x264";
        encoder  = std::move(sw);
    }
    SetColor(GREEN);
    std::cout << "      Encoder ready: " << enc_name << "\n";
    ResetColor();

    // ── Stream transport ─────────────────────────────────────────────────────

    SetColor(CYAN);
    std::cout << "[3/4] Stream transport (" << mode_name << ") -> "
              << target_ip << ":" << port << "\n";
    ResetColor();

    std::unique_ptr<IStreamer> streamer;
    if (usb_mode) {
        SetColor(CYAN);
        std::cout << "      USB: configuring adb reverse (tcp:" << port << ", tcp:" << touch_port << ")...\n";
        ResetColor();
        if (const std::string adb_err = RunAdbUsbReverse(port, touch_port); !adb_err.empty()) {
            SetColor(RED);
            std::cerr << "  ERROR: " << adb_err << "\n";
            ResetColor();
            return 1;
        }
        SetColor(GREEN);
        std::cout << "      USB: adb reverse OK.\n";
        ResetColor();

        auto srv = std::make_unique<TcpVideoServerWrap>();
        if (!srv->Initialize("", port)) {
            SetColor(RED);
            std::cerr << "  ERROR: USB TCP video listen failed on port " << port << ".\n";
            ResetColor();
            return 1;
        }
        streamer = std::move(srv);
    } else {
        auto udp = std::make_unique<UdpStreamerWrap>();
        if (!udp->Initialize(target_ip, port)) {
            SetColor(RED);
            std::cerr << "  ERROR: UDP socket init failed.\n";
            ResetColor();
            return 1;
        }
        streamer = std::move(udp);
    }
    SetColor(GREEN);
    if (usb_mode)
        std::cout << "      USB: TCP video server ready on :" << port
                  << " — start Android app (USB) to connect.\n";
    else
        std::cout << "      UDP sender ready.\n";
    ResetColor();

    // ── Touch receiver ───────────────────────────────────────────────────────

    SetColor(CYAN);
    std::cout << "[4/4] Touch/keyboard receiver (port " << touch_port << ")...\n";
    ResetColor();

    TouchReceiver touch;
    if (touch.Start(touch_port, usb_mode)) {
        SetColor(GREEN);
        std::cout << "      Listening on " << (usb_mode ? "TCP" : "UDP")
                  << " :" << touch_port << "\n";
        ResetColor();
    } else {
        SetColor(YELLOW);
        std::cout << "      WARNING: Could not start touch receiver (non-fatal)\n";
        ResetColor();
    }
    if (extended) touch.SetExtendedMonitor(capture.GetMonitorRect());

    // ── Codec config resend thread ────────────────────────────────────────────
    // Resends FLAG_STREAM_INFO + FLAG_CODEC_CONFIG every 1.5 s until Android
    // acknowledges via a codec-ready ACK packet on the touch channel.
    // This fixes the connection-order bug: Android can start any time.

    std::atomic<bool> android_ready{false};
    touch.SetAckCallback([&]() {
        android_ready = true;
        SetColor(GREEN);
        std::cout << "\n  Android ready — codec confirmed.\n";
        ResetColor();
    });

    const int cap_w = capture.GetWidth();
    const int cap_h = capture.GetHeight();

    std::thread resend_thread([&]() {
        bool dims_sent = false;
        while (g_running) {
            // Stream dimensions — static for session lifetime; send only once.
            if (!dims_sent) {
                // 12-byte stream info: width(4) + height(4) + flags(4)
                // flags bit 0: extended display mode
                uint8_t dims[12];
                const uint32_t sw = htonl(static_cast<uint32_t>(cap_w));
                const uint32_t sh = htonl(static_cast<uint32_t>(cap_h));
                const uint32_t sf = htonl(extended ? 1u : 0u);
                std::memcpy(dims,     &sw, 4);
                std::memcpy(dims + 4, &sh, 4);
                std::memcpy(dims + 8, &sf, 4);
                streamer->SendFrame(dims, 12, 0, pocketdisplay::FLAG_STREAM_INFO);
                dims_sent = true;
            }

            // Codec config — resent every 2 s until Android acknowledges.
            // Stops once android_ready is set to avoid repeated MediaCodec reinitialisation.
            if (!android_ready) {
                std::vector<uint8_t> sps_pps;
                if (encoder->GetConfigPacket(sps_pps) && !sps_pps.empty()) {
                    streamer->SendFrame(sps_pps.data(), sps_pps.size(),
                                        0, pocketdisplay::FLAG_CODEC_CONFIG);
                }
            }

            // Sleep 2 s in short intervals so shutdown is responsive
            for (int i = 0; i < 20 && g_running; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    // ── Cursor position + type thread (60 Hz) ────────────────────────────────

    const int  screen_w = GetSystemMetrics(SM_CXSCREEN);
    const int  screen_h = GetSystemMetrics(SM_CYSCREEN);
    const RECT mon_rect = extended ? capture.GetMonitorRect()
                                   : RECT{0, 0, screen_w, screen_h};
    std::thread cursor_thread([&]() {
        // Preload shared system cursor handles once for fast comparison.
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

        POINT   last_pos  = {-1, -1};
        uint8_t last_type = 0xFF;

        while (g_running) {
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

                float nx, ny;
                if (extended) {
                    const bool on_mon =
                        last_pos.x >= mon_rect.left && last_pos.x < mon_rect.right &&
                        last_pos.y >= mon_rect.top  && last_pos.y < mon_rect.bottom;
                    if (!on_mon) goto cursor_sleep;
                    const int mw = mon_rect.right  - mon_rect.left;
                    const int mh = mon_rect.bottom - mon_rect.top;
                    nx = static_cast<float>(last_pos.x - mon_rect.left) / mw;
                    ny = static_cast<float>(last_pos.y - mon_rect.top)  / mh;
                } else {
                    nx = static_cast<float>(last_pos.x) / screen_w;
                    ny = static_cast<float>(last_pos.y) / screen_h;
                }
                {
                    uint32_t nx_be, ny_be;
                    std::memcpy(&nx_be, &nx, 4); nx_be = htonl(nx_be);
                    std::memcpy(&ny_be, &ny, 4); ny_be = htonl(ny_be);
                    uint8_t payload[9];
                    std::memcpy(payload,     &nx_be, 4);
                    std::memcpy(payload + 4, &ny_be, 4);
                    payload[8] = last_type;
                    streamer->SendFrame(payload, 9, 0, pocketdisplay::FLAG_CURSOR_POS);
                }
                cursor_sleep:;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    });

    SetColor(GREEN);
    std::cout << "\n  Streaming — press Ctrl+C to stop.\n\n";
    ResetColor();

    // ── Capture / encode / send loop ─────────────────────────────────────────

    std::vector<uint8_t> bgra_buf, nal_buf;
    uint32_t frame_id    = 1;
    uint64_t frames_sent = 0;
    size_t   bytes_sent  = 0;

    const auto frame_interval = std::chrono::microseconds(1'000'000 / target_fps);
    auto       next_frame     = std::chrono::steady_clock::now();
    const auto start_time     = next_frame;

    while (g_running) {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_frame)
            std::this_thread::sleep_for(next_frame - now);
        next_frame += frame_interval;

        int w = 0, h = 0;
        if (!capture.CaptureFrame(bgra_buf, w, h)) continue;

        bool is_keyframe = false;
        if (!encoder->EncodeFrame(bgra_buf.data(), nal_buf, is_keyframe)) continue;
        if (nal_buf.empty()) continue;

        const uint8_t flags = is_keyframe ? pocketdisplay::FLAG_KEYFRAME : pocketdisplay::FLAG_NONE;
        streamer->SendFrame(nal_buf.data(), nal_buf.size(), frame_id++, flags);

        ++frames_sent;
        bytes_sent += nal_buf.size();

        // Stats every 5 s
        const double elapsed_s = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_time).count();
        if (elapsed_s >= 1.0 &&
            frames_sent % static_cast<uint64_t>(target_fps * 5) == 0) {
            PrintStats(frames_sent / elapsed_s,
                       bytes_sent * 8.0 / 1000.0 / elapsed_s,
                       nal_buf.size(), enc_name, mode_name);
        }
    }

    std::cout << "\n\n  Shutting down...\n";
    if (resend_thread.joinable()) resend_thread.join();
    if (cursor_thread.joinable()) cursor_thread.join();
    encoder->Close();
    streamer->Close();
    capture.Release();
    touch.Stop();
    return 0;
}
