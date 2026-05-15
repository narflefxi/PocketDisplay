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
#include <cstdio>
#include <future>
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
    std::cout << "  AUTO: " << exe << "          (auto-discovers Android via UDP broadcast)\n";
    std::cout << "  WiFi: " << exe << " <android_ip> [port] [bitrate_kbps] [fps] [--hw] [--extend | --monitor=N]\n";
    std::cout << "  USB:  " << exe << " --usb [--hw] [--extend | --monitor=N]\n";
    ResetColor();
    std::cout << "\nOptions:\n"
              << "  --usb        USB mode: adb reverse + TCP video server (device connects to PC)\n"
              << "  --hw         Hardware encoder (NVENC/Intel/AMD — falls back to x264)\n"
              << "  --extend     Capture last DXGI output (virtual/extended display)\n"
              << "  --monitor=N  Capture monitor N (1-based index, e.g. --monitor=2)\n"
              << "\nExamples:\n"
              << "  " << exe << "                     (auto-discover Android)\n"
              << "  " << exe << " 192.168.1.100\n"
              << "  " << exe << " --usb\n"
              << "  " << exe << " --usb --extend\n";
}

// ── Monitor selection ─────────────────────────────────────────────────────────
struct MonitorSel { int adapter = 0; int output = 0; int number = 1; };

// Enumerates monitors in Windows display order (matches Display Settings numbering),
// matches each to the correct DXGI adapter+output by device name, prints the list,
// and returns the adapter/output indices for the selected monitor.
static MonitorSel PickMonitor(bool extend, int monitor_num) {
    using Microsoft::WRL::ComPtr;

    // Step 1 — GDI enumeration gives Windows-display-settings order.
    struct GdiMon {
        WCHAR device[CCHDEVICENAME]; RECT rect; bool primary;
        int ai = -1; int oi = -1;
    };
    std::vector<GdiMon> mons;

    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hmon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto& v = *reinterpret_cast<std::vector<GdiMon>*>(lp);
            MONITORINFOEXW mi = {}; mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hmon, reinterpret_cast<MONITORINFO*>(&mi))) {
                GdiMon m = {};
                wcscpy_s(m.device, mi.szDevice);
                m.rect    = mi.rcMonitor;
                m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
                v.push_back(m);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&mons));

    if (mons.empty()) {
        std::cerr << "      WARNING: No monitors found\n";
        return {};
    }

    // Step 2 — Enumerate DXGI outputs across ALL adapters; match by DeviceName.
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                   reinterpret_cast<void**>(factory.GetAddressOf())))) {
        std::cerr << "      WARNING: CreateDXGIFactory1 failed\n";
        return {};
    }
    for (int ai = 0; ; ++ai) {
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(factory->EnumAdapters(ai, &adapter))) break;
        for (int oi = 0; ; ++oi) {
            ComPtr<IDXGIOutput> out;
            if (FAILED(adapter->EnumOutputs(oi, &out))) break;
            DXGI_OUTPUT_DESC d = {};
            out->GetDesc(&d);
            for (auto& m : mons)
                if (_wcsicmp(d.DeviceName, m.device) == 0) { m.ai = ai; m.oi = oi; }
        }
    }

    // Step 3 — Print.
    for (int i = 0; i < (int)mons.size(); ++i) {
        const auto& m = mons[i];
        const int w = m.rect.right - m.rect.left, h = m.rect.bottom - m.rect.top;
        DISPLAY_DEVICEW dd = {}; dd.cb = sizeof(dd);
        EnumDisplayDevicesW(m.device, 0, &dd, 0);
        const bool is_virtual =
            (dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0
            || wcsstr(dd.DeviceString, L"Virtual")  != nullptr
            || wcsstr(dd.DeviceString, L"Indirect") != nullptr
            || wcsstr(dd.DeviceString, L"IDD")      != nullptr
            || wcsstr(dd.DeviceString, L"Dummy")    != nullptr;
        char nm[CCHDEVICENAME] = {};
        WideCharToMultiByte(CP_UTF8, 0, m.device, -1, nm, sizeof(nm), nullptr, nullptr);
        SetColor(GRAY);
        std::cout << "      Monitor " << (i + 1) << ": " << nm << "  " << w << "x" << h;
        if (m.primary)       std::cout << "  (PRIMARY)";
        else if (is_virtual) std::cout << "  (VIRTUAL)";
        if (m.ai >= 0) std::cout << "  [adapter:" << m.ai << " output:" << m.oi << "]";
        else           std::cout << "  [no DXGI]";
        std::cout << "\n";
        ResetColor();
    }

    // Step 4 — Select.
    int idx = 0;
    for (int i = 0; i < (int)mons.size(); ++i)
        if (mons[i].primary) { idx = i; break; }
    if      (monitor_num > 0 && monitor_num <= (int)mons.size()) idx = monitor_num - 1;
    else if (extend && !mons.empty())                            idx = (int)mons.size() - 1;

    const auto& sel = mons[idx];
    if (sel.ai < 0) {
        char selnm_err[CCHDEVICENAME] = {};
        WideCharToMultiByte(CP_UTF8, 0, sel.device, -1, selnm_err, sizeof(selnm_err), nullptr, nullptr);
        std::cerr << "  ERROR: Monitor " << (idx + 1) << " (" << selnm_err
                  << ") has no matching DXGI output.\n";
        return {};
    }

    char selnm[CCHDEVICENAME] = {};
    WideCharToMultiByte(CP_UTF8, 0, sel.device, -1, selnm, sizeof(selnm), nullptr, nullptr);
    SetColor(CYAN);
    std::cout << "      Capturing monitor " << (idx + 1) << ": " << selnm
              << "  " << (sel.rect.right - sel.rect.left) << "x" << (sel.rect.bottom - sel.rect.top)
              << "  [adapter:" << sel.ai << " output:" << sel.oi << "]\n";
    ResetColor();

    return {sel.ai, sel.oi, idx + 1};
}

// ── Signal ──────────────────────────────────────────────────────────────────

static std::atomic<bool> g_running{true};
static void SignalHandler(int) { g_running = false; }

// ── Auto-discovery helpers ───────────────────────────────────────────────────

static constexpr uint16_t DISC_PORT = 7779;

// Returns the local IPv4 address used to reach the LAN (UDP connect trick).
static std::string GetLocalIp() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return "127.0.0.1";
    sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &dest.sin_addr);
    connect(s, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    sockaddr_in local = {};
    int len = sizeof(local);
    getsockname(s, reinterpret_cast<sockaddr*>(&local), &len);
    closesocket(s);
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
    return std::string(buf);
}

// Returns true if 'adb devices' shows at least one ready Android device.
static bool DetectUsbDevice() {
    FILE* pipe = _popen("adb devices 2>NUL", "r");
    if (!pipe) return false;
    char line[256] = {};
    bool header_done = false, found = false;
    while (fgets(line, sizeof(line), pipe)) {
        if (!header_done) { header_done = true; continue; }
        if (strstr(line, "\tdevice") != nullptr) { found = true; break; }
    }
    _pclose(pipe);
    return found;
}

// Returns the /24 subnet broadcast for local_ip (e.g. 192.168.1.5 → 192.168.1.255).
static std::string GetSubnetBroadcast(const std::string& local_ip) {
    const size_t dot = local_ip.rfind('.');
    return (dot != std::string::npos) ? local_ip.substr(0, dot + 1) + "255"
                                       : "255.255.255.255";
}

// Discovery result returned by RunDiscovery.
struct DiscResult { std::string android_ip; bool extend = false; };

// Phase 1: broadcast POCKETDISPLAY_HOST on 255.255.255.255 + subnet broadcast.
// Phase 2: once Android replies, wait for POCKETDISPLAY_MODE (30 s → Mirror default).
static DiscResult RunDiscovery(const std::string& local_ip, uint16_t video_port) {
    DiscResult result;
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return result;

    BOOL on = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char*>(&on), sizeof(on));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,  reinterpret_cast<char*>(&on), sizeof(on));

    sockaddr_in ba = {};
    ba.sin_family = AF_INET; ba.sin_port = htons(DISC_PORT); ba.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, reinterpret_cast<sockaddr*>(&ba), sizeof(ba)) != 0) {
        closesocket(sock); return result;
    }

    DWORD tmout = 200;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&tmout), sizeof(tmout));

    const std::string subnet_bcast = GetSubnetBroadcast(local_ip);
    const bool dual = (subnet_bcast != "255.255.255.255");

    static constexpr const char* MCAST_GROUP = "239.0.0.1";

    sockaddr_in bc_all{}, bc_sub{}, bc_mcast{};
    bc_all.sin_family   = AF_INET; bc_all.sin_port   = htons(DISC_PORT);
    bc_all.sin_addr.s_addr = INADDR_BROADCAST;
    bc_sub.sin_family   = AF_INET; bc_sub.sin_port   = htons(DISC_PORT);
    inet_pton(AF_INET, subnet_bcast.c_str(), &bc_sub.sin_addr);
    bc_mcast.sin_family = AF_INET; bc_mcast.sin_port = htons(DISC_PORT);
    inet_pton(AF_INET, MCAST_GROUP, &bc_mcast.sin_addr);

    // Set multicast TTL so packets reach the local LAN.
    DWORD mcast_ttl = 4;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL,
               reinterpret_cast<char*>(&mcast_ttl), sizeof(mcast_ttl));

    const std::string ann =
        std::string("POCKETDISPLAY_HOST:") + local_ip + ":" + std::to_string(video_port);
    auto do_bcast = [&]() {
        sendto(sock, ann.c_str(), static_cast<int>(ann.size()), 0,
               reinterpret_cast<sockaddr*>(&bc_all), sizeof(bc_all));
        if (dual) sendto(sock, ann.c_str(), static_cast<int>(ann.size()), 0,
               reinterpret_cast<sockaddr*>(&bc_sub), sizeof(bc_sub));
        sendto(sock, ann.c_str(), static_cast<int>(ann.size()), 0,
               reinterpret_cast<sockaddr*>(&bc_mcast), sizeof(bc_mcast));
    };

    SetColor(CYAN);
    std::cout << "  Broadcasting to 255.255.255.255:" << DISC_PORT;
    if (dual) std::cout << " + " << subnet_bcast << ":" << DISC_PORT;
    std::cout << " + " << MCAST_GROUP << ":" << DISC_PORT << " (multicast)";
    std::cout << "\n  Local IP  : " << local_ip
              << "  |  Open PocketDisplay on Android...\n";
    ResetColor();

    auto last_bcast = std::chrono::steady_clock::now() - std::chrono::seconds(3);
    bool phase2 = false;
    std::chrono::steady_clock::time_point phase2_start;

    while (g_running) {
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_bcast).count() >= 2) {
            do_bcast(); last_bcast = now;
        }
        if (phase2 && (now - phase2_start) > std::chrono::seconds(30)) break; // timeout → Mirror

        char buf[256]{};
        sockaddr_in from{}; int flen = sizeof(from);
        const int n = recvfrom(sock, buf, static_cast<int>(sizeof(buf)) - 1, 0,
                               reinterpret_cast<sockaddr*>(&from), &flen);
        if (n <= 0) continue;

        std::string msg(buf, n);
        while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n')) msg.pop_back();

        if (!phase2 && msg.rfind("POCKETDISPLAY_CLIENT:", 0) == 0) {
            result.android_ip = msg.substr(21);
            phase2 = true; phase2_start = now;
            SetColor(GREEN);
            std::cout << "  Android connected: " << result.android_ip << "\n";
            ResetColor();
            SetColor(CYAN);
            std::cout << "  Waiting for display mode selection on Android (30 s → Mirror)...\n";
            ResetColor();
        } else if (phase2 && msg.rfind("POCKETDISPLAY_MODE:", 0) == 0) {
            const std::string mode = msg.substr(19);
            result.extend = (mode == "extend");
            SetColor(GREEN);
            std::cout << "  Mode: " << (result.extend ? "Extended" : "Mirror") << "\n";
            ResetColor();
            break;
        }
    }

    closesocket(sock);
    if (!g_running) return {};
    return result;
}

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
    WSADATA wsa{}; WSAStartup(MAKEWORD(2, 2), &wsa);  // must be before RunDiscovery
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

    bool auto_discover = false;
    if (!usb_mode && target_ip.empty()) {
        SetColor(YELLOW);
        std::cout << "  No target IP — ";
        ResetColor();
        if (DetectUsbDevice()) {
            SetColor(GREEN);
            std::cout << "USB device detected, switching to USB mode.\n";
            ResetColor();
            usb_mode = true;
        } else {
            std::cout << "will auto-discover Android on LAN.\n";
            auto_discover = true;
        }
    }

    if (usb_mode) target_ip = "127.0.0.1";

    std::signal(SIGINT, SignalHandler);

    // WiFi auto-discovery + mode selection (blocks until Android selects or Ctrl+C).
    if (auto_discover) {
        const auto disc = RunDiscovery(GetLocalIp(), port);
        if (!g_running) return 0;
        if (disc.android_ip.empty()) {
            SetColor(RED);
            std::cerr << "  ERROR: No Android device responded.\n"
                      << "         Run: " << argv[0] << " <android_ip>  to skip discovery.\n";
            ResetColor();
            return 1;
        }
        target_ip   = disc.android_ip;
        extend_mode = disc.extend;
    }

    const bool extended = extend_mode || monitor_num > 0;

    const char* mode_name = usb_mode ? (extended ? "USB/EXT"  : "USB")
                                     : (extended ? "WiFi/EXT" : "WiFi");
    const char* enc_name  = hw_enc   ? "HW"  : "x264";

    // ── Screen capture ───────────────────────────────────────────────────────

    SetColor(CYAN);
    std::cout << "[1/4] Screen capture...\n";
    ResetColor();

    const MonitorSel mon_sel = PickMonitor(extend_mode, monitor_num);

    ScreenCapture capture;
    if (!capture.Initialize(mon_sel.adapter, mon_sel.output)) {
        SetColor(RED);
        std::cerr << "  ERROR: DXGI Desktop Duplication failed for monitor "
                  << mon_sel.number << " (adapter " << mon_sel.adapter
                  << ", output " << mon_sel.output << ").\n"
                  << "         Ensure the monitor is active and supports desktop duplication.\n";
        ResetColor();
        return 1;
    }
    SetColor(GREEN);
    std::cout << "      Capture ready: "
              << capture.GetWidth() << "x" << capture.GetHeight()
              << (extended ? "  (extended mode)" : "") << "\n";
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
    std::cout << "[3/4] Stream transport (" << mode_name << ")";
    if (!target_ip.empty()) std::cout << " -> " << target_ip << ":" << port;
    std::cout << "\n";
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
