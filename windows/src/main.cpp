#include "Session.h"
#include "UdpStreamer.h"
#include "TcpVideoServer.h"
#include "AdbUsbSetup.h"
#include "Protocol.h"
#include "GuiApp.h"
#include "VirtualDisplayDriver.h"

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>
#include <sstream>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <cstring>
#include <memory>
#include <mutex>

// â”€â”€ Console color helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

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
              << "  --hw         Hardware encoder (NVENC/Intel/AMD â€” falls back to x264)\n"
              << "  --extend     Capture last DXGI output (virtual/extended display)\n"
              << "  --monitor=N  Capture monitor N (1-based index, e.g. --monitor=2)\n"
              << "\nExamples:\n"
              << "  " << exe << "                     (auto-discover Android)\n"
              << "  " << exe << " 192.168.1.100\n"
              << "  " << exe << " --usb\n"
              << "  " << exe << " --usb --extend\n";
}

// â”€â”€ Monitor selection â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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

// â”€â”€ Signal â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static std::atomic<bool> g_running{true};
static void SignalHandler(int) { g_running = false; }

// â”€â”€ Auto-discovery helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

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

// â”€â”€ Streamer implementations â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// WiFi: wraps UdpStreamer.
struct UdpStreamerWrap : IStreamer {
    UdpStreamer s;
    bool Initialize(const std::string& ip, uint16_t port) { return s.Initialize(ip, port); }
    bool SendFrame(const uint8_t* d, size_t sz, uint32_t id, uint8_t f) override { return s.SendFrame(d,sz,id,f); }
    void UpdateTarget(const std::string& ip) override { s.UpdateTarget(ip); }
    void Close() override { s.Close(); }
};

// USB: owns the TCP socket handed off from TcpVideoServer's AcceptLoop.
struct DirectSocketStreamer : IStreamer {
    SOCKET     sock_ = INVALID_SOCKET;
    std::mutex mu_;
    explicit DirectSocketStreamer(SOCKET s) : sock_(s) {}
    bool SendFrame(const uint8_t* data, size_t size, uint32_t, uint8_t flags) override {
        using namespace pocketdisplay;
        uint8_t type = 0;
        if      (flags & FLAG_CODEC_CONFIG) type = 1;
        else if (flags & FLAG_STREAM_INFO)  type = 2;
        else if (flags & FLAG_CURSOR_POS)   type = 3;
        const uint32_t be = htonl(static_cast<uint32_t>(1u + size));
        std::lock_guard<std::mutex> lk(mu_);
        if (sock_ == INVALID_SOCKET) return false;
        auto sa = [&](const uint8_t* b, int n) -> bool {
            for (int s = 0; s < n; ) {
                int r = send(sock_, reinterpret_cast<const char*>(b+s), n-s, 0);
                if (r <= 0) return false; s += r;
            } return true;
        };
        return sa(reinterpret_cast<const uint8_t*>(&be), 4)
            && sa(&type, 1)
            && (size == 0 || sa(data, static_cast<int>(size)));
    }
    void Close() override {
        std::lock_guard<std::mutex> lk(mu_);
        if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
    }
    ~DirectSocketStreamer() override { Close(); }
};

// â”€â”€ VDD auto-resize helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

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

// â”€â”€ main â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

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
    strncpy_s(g_gui.setupMsg, "First-time setup: checking Virtual Display Driver\u2026", 255);
    if (const std::string e = EnsureVirtualDisplayDriverInstalled(); !e.empty()) {
        strncpy_s(g_gui.setupMsg, e.c_str(), 255);
        SetColor(YELLOW); std::cout << "[SETUP] " << e << "\n"; ResetColor();
    } else {
        strncpy_s(g_gui.setupMsg, "First-time setup complete: VDD ready", 255);
    }
    g_gui.setupActive.store(false);
    PrintBanner();

    // â”€â”€ Arg parsing â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    bool     force_usb    = false;
    bool     hw_enc       = false;
    int      monitor_num  = 0;
    uint16_t port         = pocketdisplay::DEFAULT_PORT;
    uint16_t touch_port   = 7778;
    int      bitrate_kbps = 30000;
    int      target_fps   = 60;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if      (arg == "--usb")    { force_usb  = true; }
        else if (arg == "--hw")     { hw_enc     = true; }
        else if (arg == "--extend") { /* mode comes from Android HELLO */ }
        else if (arg.rfind("--monitor=", 0) == 0) { monitor_num = std::stoi(arg.substr(10)); }
        else if (arg == "--help" || arg == "-h")   { PrintUsage(argv[0]); return 0; }
        else if (arg == "--port"    && i+1 < argc) port         = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--fps"     && i+1 < argc) target_fps   = std::stoi(argv[++i]);
        else if (arg == "--bitrate" && i+1 < argc) bitrate_kbps = std::stoi(argv[++i]);
        // Positional IP / extra args: accepted but unused; HELLO delivers Android IP.
    }

    std::signal(SIGINT, SignalHandler);

    // â”€â”€ One-click mode: wait for GUI Start button â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    const bool oneClickMode = (argc == 1);
    if (oneClickMode) {
        strncpy_s(g_gui.statusMsg, "Click \u25b6 Start Streaming to begin", 255);
        g_gui.guiStartRequested.store(false);
        g_gui.waitingForStart.store(true);
        while (g_running && !g_gui.guiStartRequested.load()) Sleep(100);
        g_gui.waitingForStart.store(false);
        if (!g_running) return 0;
        strncpy_s(g_gui.statusMsg, "Detecting connection\u2026", 255);
    }

    // â”€â”€ USB device detection + adb reverse â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    const bool usb_device_present = force_usb || DetectUsbDevice();
    bool       usb_adb_ready      = false;
    if (usb_device_present) {
        SetColor(CYAN);
        std::cout << "[USB] Configuring adb reverse (tcp:" << port
                  << ", tcp:" << touch_port << ")...\n";
        ResetColor();
        g_gui.setupActive.store(true);
        strncpy_s(g_gui.setupMsg, "USB setup: configuring adb reverse\u2026", 255);
        const std::string adb_err = RunAdbUsbReverse(port, touch_port);
        g_gui.setupActive.store(false);
        if (!adb_err.empty()) {
            SetColor(YELLOW);
            std::cerr << "  [USB] adb reverse failed: " << adb_err << "\n";
            ResetColor();
            if (force_usb) { strncpy_s(g_gui.setupMsg, adb_err.c_str(), 255); return 1; }
        } else {
            usb_adb_ready = true;
            strncpy_s(g_gui.setupMsg, "USB setup complete: adb reverse ready", 255);
            SetColor(GREEN); std::cout << "[USB] adb reverse OK.\n"; ResetColor();
            Sleep(500);
            StartUsbMonitorThread(port, touch_port, g_running);
        }
    }
    strncpy_s(g_gui.mode, (usb_device_present && usb_adb_ready) ? "USB" : "WiFi", 31);

    // â”€â”€ Always-on HELLO server â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    TcpVideoServer hello_server;
    if (!hello_server.StartListen(port)) {
        SetColor(RED);
        std::cerr << "  ERROR: HELLO server listen failed on port " << port << ".\n";
        ResetColor();
        return 1;
    }

    // â”€â”€ Session manager â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    std::mutex               session_mu;
    std::shared_ptr<Session> current_session;

    hello_server.SetHelloCallback(
        [&](bool extend, int aW, int aH, const std::string& peer, SOCKET sock) {
        SetColor(GREEN);
        std::cout << "\n[HELLO] " << (peer == "127.0.0.1" ? "USB" : "WiFi")
                  << " from " << peer
                  << "  mode=" << (extend ? "Extended" : "Mirror")
                  << "  screen=" << aW << "x" << aH << "\n";
        ResetColor();

        // Stop existing session before creating the new one.
        std::shared_ptr<Session> old_sess;
        {
            std::lock_guard<std::mutex> lk(session_mu);
            old_sess = std::move(current_session);
        }
        if (old_sess) {
            SetColor(YELLOW);
            std::cout << "  [HELLO] Stopping previous session...\n";
            ResetColor();
            old_sess->Stop();
            old_sess.reset();
        }

        const bool is_usb = (peer == "127.0.0.1");

        // VDD / resolution setup for Extended mode.
        if (extend) {
            if (const std::string e = EnsureVirtualDisplayDriverForExtendedMode(); !e.empty()) {
                SetColor(YELLOW);
                std::cerr << "  [HELLO] VDD warning: " << e << "\n";
                ResetColor();
            }
            if (aW > 0 && aH > 0) {
                const auto [vw, vh] = CalcVddResolution(aW, aH);
                if (vw > 0) { SetVddResolution(vw, vh); Sleep(800); }
            }
        }

        // Pick capture monitor.
        const MonitorSel mon = PickMonitor(extend, monitor_num);
        if (mon.adapter < 0) {
            SetColor(RED);
            std::cerr << "  [HELLO] Monitor selection failed.\n";
            ResetColor();
            if (is_usb && sock != INVALID_SOCKET) closesocket(sock);
            return;
        }

        // Build transport streamer.
        std::unique_ptr<IStreamer> streamer;
        if (is_usb) {
            streamer = std::make_unique<DirectSocketStreamer>(sock);
        } else {
            auto udp = std::make_unique<UdpStreamerWrap>();
            if (!udp->Initialize(peer, port)) {
                SetColor(RED); std::cerr << "  [HELLO] UDP init failed.\n"; ResetColor();
                return;
            }
            streamer = std::move(udp);
        }

        // Build and start session.
        Session::Config cfg;
        cfg.adapter_idx  = mon.adapter;
        cfg.output_idx   = mon.output;
        cfg.monitor_num  = mon.number;
        cfg.extend_mode  = extend;
        cfg.hw_enc       = hw_enc;
        cfg.android_w    = aW;
        cfg.android_h    = aH;
        cfg.bitrate_kbps = bitrate_kbps;
        cfg.target_fps   = target_fps;
        cfg.usb_mode     = is_usb;
        cfg.touch_port   = touch_port;
        cfg.streamer     = std::move(streamer);

        auto sess = std::make_shared<Session>(std::move(cfg));
        if (sess->Start()) {
            std::lock_guard<std::mutex> lk(session_mu);
            current_session = std::move(sess);
            strncpy_s(g_gui.mode, is_usb ? "USB" : "WiFi", 31);
            SetColor(GREEN); std::cout << "  [HELLO] Session started.\n"; ResetColor();
        } else {
            SetColor(RED);
            std::cerr << "  [HELLO] Session start failed \u2014 waiting for reconnect.\n";
            ResetColor();
            strncpy_s(g_gui.statusMsg, "Session init failed \u2014 waiting\u2026", 255);
        }
    });

    // â”€â”€ WiFi discovery broadcaster (continuous background; skipped with --usb) â”€â”€
    std::thread disc_thread;
    if (!force_usb) {
        disc_thread = std::thread([&]() {
            SOCKET ds = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (ds == INVALID_SOCKET) return;
            BOOL on = TRUE;
            setsockopt(ds, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char*>(&on), sizeof(on));

            const std::string local_ip = GetLocalIp();
            const std::string subnet   = GetSubnetBroadcast(local_ip);
            const bool dual = (subnet != "255.255.255.255");
            static constexpr const char* MCAST_GRP = "239.0.0.1";

            sockaddr_in bc_all{}, bc_sub{}, bc_mcast{};
            bc_all.sin_family      = AF_INET;
            bc_all.sin_port        = htons(DISC_PORT);
            bc_all.sin_addr.s_addr = INADDR_BROADCAST;
            bc_sub.sin_family      = AF_INET;
            bc_sub.sin_port        = htons(DISC_PORT);
            inet_pton(AF_INET, subnet.c_str(), &bc_sub.sin_addr);
            bc_mcast.sin_family    = AF_INET;
            bc_mcast.sin_port      = htons(DISC_PORT);
            inet_pton(AF_INET, MCAST_GRP, &bc_mcast.sin_addr);

            DWORD ttl = 4;
            setsockopt(ds, IPPROTO_IP, IP_MULTICAST_TTL,
                       reinterpret_cast<char*>(&ttl), sizeof(ttl));

            const std::string ann =
                std::string("POCKETDISPLAY_HOST:") + local_ip + ":" + std::to_string(port);
            auto do_bcast = [&]() {
                sendto(ds, ann.c_str(), static_cast<int>(ann.size()), 0,
                       reinterpret_cast<sockaddr*>(&bc_all), sizeof(bc_all));
                if (dual)
                    sendto(ds, ann.c_str(), static_cast<int>(ann.size()), 0,
                           reinterpret_cast<sockaddr*>(&bc_sub), sizeof(bc_sub));
                sendto(ds, ann.c_str(), static_cast<int>(ann.size()), 0,
                       reinterpret_cast<sockaddr*>(&bc_mcast), sizeof(bc_mcast));
            };

            SetColor(CYAN);
            std::cout << "  [Discovery] Broadcasting POCKETDISPLAY_HOST on UDP :"
                      << DISC_PORT << " (local IP: " << local_ip << ")\n";
            ResetColor();

            while (g_running) {
                // Pause while a session is active to prevent spurious reconnects.
                bool active = false;
                {
                    std::lock_guard<std::mutex> lk(session_mu);
                    active = current_session && current_session->IsRunning();
                }
                if (!active) do_bcast();
                for (int i = 0; i < 20 && g_running; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            closesocket(ds);
        });
    }

    SetColor(CYAN);
    std::cout << "  Waiting for Android connection on port " << port << "...\n\n";
    ResetColor();
    strncpy_s(g_gui.statusMsg, "Waiting for connection\u2026", 255);

    // â”€â”€ Main wait loop â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    while (g_running) {
        Sleep(100);
        std::shared_ptr<Session> sess;
        {
            std::lock_guard<std::mutex> lk(session_mu);
            sess = current_session;
        }
        if (sess && !sess->IsRunning()) {
            SetColor(YELLOW);
            std::cout << "\n  [Main] Session ended \u2014 waiting for reconnect...\n";
            ResetColor();
            sess->Stop();
            {
                std::lock_guard<std::mutex> lk(session_mu);
                current_session.reset();
            }
            sess.reset();
            g_gui.connected.store(false);
            strncpy_s(g_gui.statusMsg, "Disconnected \u2014 waiting\u2026", 255);
        }
    }

    // â”€â”€ Shutdown â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    SetColor(YELLOW); std::cout << "\n  Shutting down...\n"; ResetColor();
    {
        std::lock_guard<std::mutex> lk(session_mu);
        if (current_session) { current_session->Stop(); current_session.reset(); }
    }
    hello_server.Close();
    if (disc_thread.joinable()) disc_thread.join();
    if (usb_device_present && usb_adb_ready) ClearAdbReverse();
    g_gui.streaming.store(false);
    g_gui.connected.store(false);
    strncpy_s(g_gui.statusMsg, "Stopped", 255);
    WSACleanup();
    return 0;
}
