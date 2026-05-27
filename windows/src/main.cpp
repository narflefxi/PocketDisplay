#include "ScreenCapture.h"
#include "Encoder.h"
#include "HwEncoder.h"
#include "UdpStreamer.h"
#include "TcpVideoServer.h"
#include "AdbUsbSetup.h"
#include "TouchReceiver.h"
#include "Protocol.h"
#include "GuiApp.h"
#include "VirtualDisplayDriver.h"

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

static MonitorSel PickMonitor(bool extend, int monitor_num) {
    using Microsoft::WRL::ComPtr;

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

static std::string GetSubnetBroadcast(const std::string& local_ip) {
    const size_t dot = local_ip.rfind('.');
    return (dot != std::string::npos) ? local_ip.substr(0, dot + 1) + "255"
                                       : "255.255.255.255";
}

struct DiscResult { std::string android_ip; bool extend = false; };

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
        if (phase2 && (now - phase2_start) > std::chrono::seconds(30)) break;

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
        } else if (msg.rfind("POCKETDISPLAY_MODE:", 0) == 0) {
            const std::string mode_str = msg.substr(19);
            SetColor(GREEN);
            std::cout << "  [WiFi] MODE received: \"" << mode_str << "\"  -> "
                      << (mode_str == "extend" ? "Extended" : "Mirror") << "\n";
            ResetColor();
            result.extend = (mode_str == "extend");
            if (result.android_ip.empty()) {
                char ip_buf[INET_ADDRSTRLEN] = {};
                if (inet_ntop(AF_INET, &from.sin_addr, ip_buf, sizeof(ip_buf)))
                    result.android_ip = ip_buf;
            }
            break;
        }
    }

    closesocket(sock);
    if (!g_running) return {};
    return result;
}

// ── Live stats line ───────────────────────────────────────────────────────────

static void PrintStats(double fps, double kbps, size_t frame_bytes,
                        const char* enc_name, const char* mode_name) {
    std::ostringstream oss;
    oss << "\r  [" << mode_name << "/" << enc_name << "]  "
        << "FPS: " << std::setw(3) << static_cast<int>(fps)
        << "  Bitrate: " << std::setw(6) << static_cast<int>(kbps) << " kbps"
        << "  Frame: " << std::setw(6) << frame_bytes << " B  ";
    std::cout << oss.str() << std::flush;
}

// ── Encoder wrapper ──────────────────────────────────────────────────────────

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

// ── Streamer wrapper ─────────────────────────────────────────────────────────

struct IStreamer {
    virtual bool Initialize(const std::string& ip, uint16_t port) = 0;
    virtual bool SendFrame(const uint8_t* d, size_t sz,
                            uint32_t id, uint8_t flags) = 0;
    virtual void UpdateTarget(const std::string& /*ip*/) {}  // no-op for TCP
    virtual void Close() = 0;
    virtual ~IStreamer() = default;
};

struct UdpStreamerWrap : IStreamer {
    UdpStreamer s;
    bool Initialize(const std::string& ip, uint16_t port) override { return s.Initialize(ip,port); }
    bool SendFrame(const uint8_t* d, size_t sz, uint32_t id, uint8_t f) override { return s.SendFrame(d,sz,id,f); }
    void UpdateTarget(const std::string& ip) override { s.UpdateTarget(ip); }
    void Close() override { s.Close(); }
};

struct TcpVideoServerWrap : IStreamer {
    TcpVideoServer s;
    bool Initialize(const std::string& /*ip*/, uint16_t port) override { return s.StartListen(port); }
    bool SendFrame(const uint8_t* d, size_t sz, uint32_t id, uint8_t f) override {
        return s.SendFrame(d, sz, id, f);
    }
    void Close() override { s.Close(); }
};

// ── VDD auto-resize helpers ──────────────────────────────────────────────────

static std::pair<int,int> CalcVddResolution(int and_w, int and_h) {
    if (and_w <= 0 || and_h <= 0) return {0, 0};
    int target_w = (and_w + 1) & ~1;
    int target_h = (and_h + 1) & ~1;
    if (target_w >= 640 && target_h >= 480 && target_w <= 3200 && target_h <= 1800)
        return {target_w, target_h};

    const double ratio = static_cast<double>(target_w) / target_h;
    constexpr int heights[] = {1440, 1200, 1080, 900, 800, 768, 720, 600};
    for (int h : heights) {
        int w = static_cast<int>(h * ratio + 0.5);
        w = (w + 1) & ~1;  // round to even
        if (w >= 640 && w <= 3200) return {w, h};
    }
    return {1280, 720};
}

static bool SetVddResolution(int target_w, int target_h) {
    for (DWORD i = 0; ; ++i) {
        DISPLAY_DEVICEW dd = {}; dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
        const bool is_virtual =
            (dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0
            || wcsstr(dd.DeviceString, L"Virtual")  != nullptr
            || wcsstr(dd.DeviceString, L"Indirect") != nullptr
            || wcsstr(dd.DeviceString, L"IDD")      != nullptr
            || wcsstr(dd.DeviceString, L"Dummy")    != nullptr;
        if (!is_virtual) continue;
        DEVMODEW cur = {}; cur.dmSize = sizeof(cur);
        EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &cur);
        DEVMODEW dm = {};
        dm.dmSize             = sizeof(dm);
        dm.dmPelsWidth        = static_cast<DWORD>(target_w);
        dm.dmPelsHeight       = static_cast<DWORD>(target_h);
        dm.dmBitsPerPel       = cur.dmBitsPerPel > 0 ? cur.dmBitsPerPel : 32;
        dm.dmDisplayFrequency = cur.dmDisplayFrequency > 0 ? cur.dmDisplayFrequency : 60;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
        const LONG r = ChangeDisplaySettingsExW(dd.DeviceName, &dm, nullptr, 0, nullptr);
        char dev[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, dd.DeviceName, -1, dev, sizeof(dev), nullptr, nullptr);
        if (r == DISP_CHANGE_SUCCESSFUL) {
            SetColor(GREEN);
            std::cout << "[USB] VDD " << dev << " -> " << target_w << "x" << target_h << "\n";
            ResetColor();
            return true;
        }
        SetColor(YELLOW);
        std::cout << "[USB] VDD " << dev << " resolution change failed (err=" << r << ")\n";
        ResetColor();
    }
    return false;
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    g_con = GetStdHandle(STD_OUTPUT_HANDLE);
    WSADATA wsa{}; WSAStartup(MAKEWORD(2, 2), &wsa);

    {
        DWORD pids[2];
        if (GetConsoleProcessList(pids, 2) <= 1) {
            FreeConsole();
            g_con = INVALID_HANDLE_VALUE;
        }
    }

    GuiLaunch();
    g_gui.setupActive.store(true);
    strncpy_s(g_gui.setupMsg, "First-time setup: checking Virtual Display Driver…", 255);
    if (const std::string e = EnsureVirtualDisplayDriverInstalled(); !e.empty()) {
        strncpy_s(g_gui.setupMsg, e.c_str(), 255);
        SetColor(YELLOW);
        std::cout << "[SETUP] " << e << "\n";
        ResetColor();
    } else {
        strncpy_s(g_gui.setupMsg, "First-time setup complete: VDD ready", 255);
    }
    g_gui.setupActive.store(false);
    PrintBanner();

    bool usb_mode     = false;
    bool hw_enc       = false;
    bool extend_mode  = false;
    int  monitor_num  = 0;
    std::string target_ip;
    uint16_t port         = pocketdisplay::DEFAULT_PORT;
    uint16_t touch_port   = 7778;
    int      bitrate_kbps = 30000;
    int      target_fps   = 60;

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
        else if (arg[0] != '-' && !target_ip.empty()) {
            if (port == pocketdisplay::DEFAULT_PORT) port = static_cast<uint16_t>(std::stoi(arg));
            else if (bitrate_kbps == 30000)          bitrate_kbps = std::stoi(arg);
            else if (target_fps == 60)               target_fps   = std::stoi(arg);
        }
    }

    // ── One-click mode ───────────────────────────────────────────────────────
    const bool oneClickMode = (argc == 1);
    if (oneClickMode) {
        strncpy_s(g_gui.statusMsg, "Click \u25b6 Start Streaming to begin", 255);
        g_gui.guiStartRequested.store(false);
        g_gui.waitingForStart.store(true);
        while (g_running && !g_gui.guiStartRequested.load()) {
            Sleep(100);
        }
        g_gui.waitingForStart.store(false);
        if (!g_running) return 0;
        strncpy_s(g_gui.statusMsg, "Detecting connection\u2026", 255);
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
    strncpy_s(g_gui.mode, usb_mode ? "USB" : (target_ip.empty() ? "WiFi (auto)" : "WiFi"), 31);

    std::signal(SIGINT, SignalHandler);

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

    std::unique_ptr<TcpVideoServerWrap> usb_server;
    if (usb_mode) {
        SetColor(CYAN);
        std::cout << "[USB] Configuring adb reverse (tcp:" << port
                  << ", tcp:" << touch_port << ")...\n";
        ResetColor();
        g_gui.setupActive.store(true);
        strncpy_s(g_gui.setupMsg, "USB setup: configuring adb reverse…", 255);
        if (const std::string e = RunAdbUsbReverse(port, touch_port); !e.empty()) {
            g_gui.setupActive.store(false);
            strncpy_s(g_gui.setupMsg, e.c_str(), 255);
            SetColor(RED); std::cerr << "  ERROR: " << e << "\n"; ResetColor();
            return 1;
        }
        strncpy_s(g_gui.setupMsg, "USB setup complete: adb reverse ready", 255);
        g_gui.setupActive.store(false);
        SetColor(GREEN); std::cout << "[USB] adb reverse OK.\n"; ResetColor();
        StartUsbMonitorThread(port, touch_port, g_running);

        usb_server = std::make_unique<TcpVideoServerWrap>();
        if (!usb_server->Initialize("", port)) {
            SetColor(RED);
            std::cerr << "  ERROR: USB TCP listen failed on port " << port << ".\n";
            ResetColor();
            return 1;
        }

        SetColor(CYAN);
        std::cout << "[USB] Waiting for Android to connect and select mode "
                     "(120 s \u2192 Mirror)...\n";
        ResetColor();
        usb_server->s.WaitForMode(120000, extend_mode);
        if (!g_running) return 0;

        SetColor(GREEN);
        std::cout << "[USB] Mode: " << (extend_mode ? "Extended" : "Mirror") << "\n";
        ResetColor();

        if (extend_mode) {
            SetColor(CYAN);
            std::cout << "[USB] Checking virtual display driver...\n";
            ResetColor();
            if (const std::string e = EnsureVirtualDisplayDriverForExtendedMode(); !e.empty()) {
                SetColor(RED);
                std::cerr << "  ERROR: " << e << "\n";
                ResetColor();
                return 1;
            }

            int and_w = 0, and_h = 0;
            usb_server->s.GetAndroidSize(and_w, and_h);
            if (and_w > 0 && and_h > 0) {
                SetColor(CYAN);
                std::cout << "[USB] Android screen: " << and_w << "x" << and_h << "\n";
                ResetColor();
                const auto [vdd_w, vdd_h] = CalcVddResolution(and_w, and_h);
                if (vdd_w > 0) {
                    SetVddResolution(vdd_w, vdd_h);
                    Sleep(800);  // let Windows settle before capture
                }
            }
        }
    }

    if (extend_mode && !usb_mode) {
        SetColor(CYAN);
        std::cout << "[EXT] Checking virtual display driver...\n";
        ResetColor();
        if (const std::string e = EnsureVirtualDisplayDriverForExtendedMode(); !e.empty()) {
            SetColor(RED);
            std::cerr << "  ERROR: " << e << "\n";
            ResetColor();
            return 1;
        }
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
        streamer = std::move(usb_server);
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
        std::cout << "      USB: Android connected on :" << port
                  << " \u2014 starting stream.\n";
    else
        std::cout << "      UDP sender ready.\n";
    ResetColor();

    // ── Touch receiver ───────────────────────────────────────────────────────

    SetColor(CYAN);
    std::cout << "[4/4] Touch/keyboard receiver (port " << touch_port << ")...\n";
    ResetColor();

    TouchReceiver touch;

    // ── Codec config resend thread ────────────────────────────────────────────

    std::atomic<bool> android_ready{false};
    std::atomic<int>  connect_attempt{0};  // incremented on every (re)connect
    // Reset android_ready on every reconnect (after the first WaitForMode connection)
    // so resend_thread re-sends codec config.  Without this, android_ready stays true
    // after Android disconnects and the decoder never reconfigures on reconnect.
    // usb_server was moved into streamer above; cast back to reach TcpVideoServer.
    if (usb_mode) {
        auto* tcp = static_cast<TcpVideoServerWrap*>(streamer.get());
        tcp->s.SetReconnectCallback([&]() {
            const int attempt = connect_attempt.fetch_add(1) + 1;
            const bool was_ready = android_ready.exchange(false);
            SetColor(YELLOW);
            std::cout << "\n[DBG] ==> RECONNECT attempt #" << attempt
                      << "  android_ready was=" << (was_ready ? "true" : "false")
                      << " -> now FALSE  (resend_thread will send codec config)\n";
            ResetColor();
        });
    }
    // IMPORTANT: register callbacks BEFORE touch.Start() opens the port.
    // If Android launched before Windows, TcpTouchSender is already retrying.
    // Start() opens the port; Android can connect and fire TcpAcceptLoop in the
    // tiny gap before SetConnectCallback() runs — connect_cb_ would be null,
    // touch_socket_ready stays false, and resend_thread deadlocks forever.
    touch.SetConnectCallback([&]() {
        SetColor(CYAN);
        std::cout << "[DBG] touch socket accepted\n" << std::flush;
        ResetColor();
    });
    touch.SetAckCallback([&](const std::string& sender_ip) {
        const int attempt = connect_attempt.load();
        if (!usb_mode && !sender_ip.empty() && sender_ip != target_ip) {
            SetColor(YELLOW);
            std::cout << "\n[DBG] ==> Android IP changed: " << target_ip
                      << " -> " << sender_ip << "  updating UDP target\n";
            ResetColor();
            target_ip = sender_ip;
            streamer->UpdateTarget(sender_ip);
        }
        android_ready = true;
        g_gui.connected.store(true);
        strncpy_s(g_gui.statusMsg, "Android ready \u2014 streaming", 255);
        SetColor(GREEN);
        std::cout << "\n[DBG] ==> ACK received on attempt #" << attempt
                  << "  android_ready -> TRUE  (video frames will flow)\n";
        ResetColor();
    });

    // Open the port now \u2014 callbacks already registered, no race window.
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

    const int cap_w = capture.GetWidth();
    const int cap_h = capture.GetHeight();
    std::atomic<int> stream_w{cap_w}, stream_h{cap_h};
    g_gui.capW.store(cap_w);
    g_gui.capH.store(cap_h);
    g_gui.streaming.store(true);
    strncpy_s(g_gui.statusMsg, "Waiting for Android…", 255);

    std::thread resend_thread([&]() {
        while (g_running) {
            // Always send stream_info + codec_config so Android can (re)configure
            // its decoder after any WiFi reconnect or IP change, without waiting
            // for a signal from Windows.  VideoDecoder.configure() deduplicates
            // identical SPS: same codec → just re-ACKs; fresh decoder → full init.
            uint8_t dims[12];
            const uint32_t sw = htonl(static_cast<uint32_t>(stream_w.load()));
            const uint32_t sh = htonl(static_cast<uint32_t>(stream_h.load()));
            const uint32_t sf = htonl(extended ? 1u : 0u);
            std::memcpy(dims,     &sw, 4);
            std::memcpy(dims + 4, &sh, 4);
            std::memcpy(dims + 8, &sf, 4);
            streamer->SendFrame(dims, 12, 0, pocketdisplay::FLAG_STREAM_INFO);

            std::vector<uint8_t> sps_pps;
            if (encoder->GetConfigPacket(sps_pps) && !sps_pps.empty())
                streamer->SendFrame(sps_pps.data(), sps_pps.size(),
                                    0, pocketdisplay::FLAG_CODEC_CONFIG);

            for (int i = 0; i < 20 && g_running; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    // ── Cursor thread ────────────────────────────────────────────────────────

    const int  screen_w = GetSystemMetrics(SM_CXSCREEN);
    const int  screen_h = GetSystemMetrics(SM_CYSCREEN);
    const RECT mon_rect = extended ? capture.GetMonitorRect()
                                   : RECT{0, 0, screen_w, screen_h};
    std::thread cursor_thread([&]() {
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

    // ── Crash log helper ─────────────────────────────────────────────────────

    auto WriteCrashLog = [](const std::string& msg) {
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
    };

    // ── Capture / encode / send loop ─────────────────────────────────────────

    std::vector<uint8_t> bgra_buf, nal_buf;
    uint32_t frame_id    = 1;
    uint64_t frames_sent = 0;
    size_t   bytes_sent  = 0;

    const auto frame_interval = std::chrono::microseconds(1'000'000 / target_fps);
    auto       next_frame     = std::chrono::steady_clock::now();
    const auto start_time     = next_frame;

    try {
    while (g_running) {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_frame)
            std::this_thread::sleep_for(next_frame - now);
        next_frame += frame_interval;

        // Gate: jangan kirim video frames sampai Android konfirmasi codec ready
        if (!android_ready) continue;

        int w = 0, h = 0;
        if (!capture.CaptureFrame(bgra_buf, w, h)) continue;

        if (w != stream_w.load() || h != stream_h.load()) {
            SetColor(YELLOW);
            std::cout << "\n[DBG] Resolution changed " << stream_w.load() << "x" << stream_h.load()
                      << " -> " << w << "x" << h << "  re-initializing encoder\n";
            ResetColor();
            encoder->Close();
            if (!encoder->Initialize(w, h, target_fps, bitrate_kbps)) {
                std::cerr << "  [ERROR] Encoder re-init after resolution change failed\n";
                continue;
            }
            stream_w.store(w);
            stream_h.store(h);
            android_ready = false;  // trigger resend_thread to push new codec config + dims
            continue;
        }

        bool is_keyframe = false;
        if (!encoder->EncodeFrame(bgra_buf.data(), nal_buf, is_keyframe)) continue;
        if (nal_buf.empty()) continue;

        const uint8_t flags = is_keyframe ? pocketdisplay::FLAG_KEYFRAME : pocketdisplay::FLAG_NONE;
        streamer->SendFrame(nal_buf.data(), nal_buf.size(), frame_id++, flags);

        ++frames_sent;
        bytes_sent += nal_buf.size();

        const double elapsed_s = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_time).count();
        if (elapsed_s >= 1.0 &&
            frames_sent % static_cast<uint64_t>(target_fps * 5) == 0) {
            const double cur_fps = frames_sent / elapsed_s;
            g_gui.fps.store(static_cast<int>(cur_fps + 0.5));
            g_gui.bitrateKbps.store(static_cast<int>(bytes_sent * 8.0 / 1000.0 / elapsed_s));
            PrintStats(cur_fps, bytes_sent * 8.0 / 1000.0 / elapsed_s,
                       nal_buf.size(), enc_name, mode_name);
        }
    }
    } catch (const std::exception& e) {
        const std::string msg = std::string("Exception in main loop: ") + e.what();
        SetColor(RED); std::cerr << "\n[CRASH] " << msg << "\n"; ResetColor();
        WriteCrashLog(msg);
        g_running = false;
    } catch (...) {
        const std::string msg = "Unknown exception in main loop";
        SetColor(RED); std::cerr << "\n[CRASH] " << msg << "\n"; ResetColor();
        WriteCrashLog(msg);
        g_running = false;
    }

    // ── Shutdown ─────────────────────────────────────────────────────────────

    g_gui.streaming.store(false);
    g_gui.connected.store(false);
    strncpy_s(g_gui.statusMsg, "Stopped", 255);
    std::cout << "\n\n  Shutting down...\n";
    if (resend_thread.joinable()) resend_thread.join();
    if (cursor_thread.joinable()) cursor_thread.join();
    encoder->Close();
    streamer->Close();
    capture.Release();
    touch.Stop();
    return 0;
}
