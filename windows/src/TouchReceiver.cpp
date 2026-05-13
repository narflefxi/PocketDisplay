#include "TouchReceiver.h"
#include <windows.h>
#include <ws2tcpip.h>
#include <cstring>
#include <iostream>

TouchReceiver::~TouchReceiver() { Stop(); }

bool TouchReceiver::Start(uint16_t port, bool tcp_mode) {
    tcp_mode_ = tcp_mode;

    if (tcp_mode) {
        // TCP server — Android connects after: adb reverse tcp:7778 tcp:7778
        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET) return false;

        int reuse = 1;
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr = {};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port);

        if (bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
            listen(sock_, 1) == SOCKET_ERROR) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
            return false;
        }
        running_ = true;
        thread_  = std::thread(&TouchReceiver::TcpAcceptLoop, this);
    } else {
        // UDP — existing WiFi path
        sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == INVALID_SOCKET) return false;

        sockaddr_in addr = {};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port);

        if (bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
            return false;
        }
        running_ = true;
        thread_  = std::thread(&TouchReceiver::UdpLoop, this);
    }
    return true;
}

// ── Packet layout (16 bytes, big-endian) ────────────────────────────────────
// [0-3]  magic  'P','D','T','I'
// [4]    type   0=MOVE 1=DOWN 2=UP 3=RIGHT_CLICK 4=SCROLL
//               5=KEY_CHAR  (bytes[8-11] = uint32 unicode codepoint)
//               6=KEY_DOWN  (bytes[8-9]  = uint16 Windows VK code)
//               7=KEY_UP    (bytes[8-9]  = uint16 Windows VK code)
// [5-7]  reserved
// [8-11] nx or key payload (big-endian float / uint32)
// [12-15] ny or padding   (big-endian float)
#pragma pack(push, 1)
struct TouchPacket {
    uint8_t magic[4];
    uint8_t type;
    uint8_t reserved[3];
    uint8_t payload[8];   // interpretation depends on type
};
#pragma pack(pop)
static_assert(sizeof(TouchPacket) == 16, "TouchPacket size mismatch");

static float be_to_f(const uint8_t* p) {
    uint32_t tmp = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                   (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
    float f;
    std::memcpy(&f, &tmp, 4);
    return f;
}

static uint32_t be_u32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

static uint16_t be_u16(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

void TouchReceiver::ProcessPacket(const uint8_t* buf, int len) {
    if (len < static_cast<int>(sizeof(TouchPacket))) return;
    auto* pkt = reinterpret_cast<const TouchPacket*>(buf);
    if (pkt->magic[0] != 'P' || pkt->magic[1] != 'D' ||
        pkt->magic[2] != 'T' || pkt->magic[3] != 'I') return;

    const uint8_t type = pkt->type;

    if (type <= 4) {
        ApplyTouchEvent(type, be_to_f(pkt->payload), be_to_f(pkt->payload + 4));
    } else if (type == 5) {
        InjectUnicodeChar(be_u32(pkt->payload));
    } else if (type == 6 || type == 7) {
        InjectVirtualKey(be_u16(pkt->payload), type == 6);
    } else if (type == 8) {
        // Codec-ready ACK from Android
        if (ack_cb_) ack_cb_();
    }
}

// ── Transport loops ──────────────────────────────────────────────────────────

void TouchReceiver::UdpLoop() {
    uint8_t buf[sizeof(TouchPacket)];
    while (running_) {
        int n = recv(sock_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n <= 0) break;
        ProcessPacket(buf, n);
    }
}

void TouchReceiver::TcpAcceptLoop() {
    while (running_) {
        sockaddr_in client_addr = {};
        int addr_len = sizeof(client_addr);
        SOCKET client = accept(sock_,
                               reinterpret_cast<sockaddr*>(&client_addr),
                               &addr_len);
        if (client == INVALID_SOCKET) break;
        std::cout << "[touch/TCP] Android connected\n";
        TcpClientLoop(client);
        closesocket(client);
        std::cout << "[touch/TCP] Android disconnected\n";
    }
}

void TouchReceiver::TcpClientLoop(SOCKET client) {
    // Fixed-size packets; read exactly 16 bytes per event
    uint8_t buf[sizeof(TouchPacket)];
    while (running_) {
        int received = 0;
        while (received < static_cast<int>(sizeof(buf))) {
            int n = recv(client,
                         reinterpret_cast<char*>(buf + received),
                         static_cast<int>(sizeof(buf)) - received, 0);
            if (n <= 0) return;
            received += n;
        }
        ProcessPacket(buf, static_cast<int>(sizeof(buf)));
    }
}

// ── Input injection ──────────────────────────────────────────────────────────

void TouchReceiver::ApplyTouchEvent(uint8_t type, float nx, float ny) const {
    if (type == 4) {
        // SCROLL: nx=horizontal delta, ny=vertical delta (WHEEL_DELTA units)
        const int v = static_cast<int>(ny);
        const int h = static_cast<int>(nx);
        if (v != 0) {
            INPUT s = {};
            s.type         = INPUT_MOUSE;
            s.mi.dwFlags   = MOUSEEVENTF_WHEEL;
            s.mi.mouseData = static_cast<DWORD>(v);
            SendInput(1, &s, sizeof(INPUT));
        }
        if (h != 0) {
            INPUT s = {};
            s.type         = INPUT_MOUSE;
            s.mi.dwFlags   = MOUSEEVENTF_HWHEEL;
            s.mi.mouseData = static_cast<DWORD>(h);
            SendInput(1, &s, sizeof(INPUT));
        }
        return;
    }

    const LONG ax = static_cast<LONG>(nx * 65535.0f);
    const LONG ay = static_cast<LONG>(ny * 65535.0f);

    INPUT inputs[3] = {};
    int   count     = 1;

    inputs[0].type       = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
    inputs[0].mi.dx      = ax;
    inputs[0].mi.dy      = ay;

    if (type == 1) {
        inputs[1].type       = INPUT_MOUSE;
        inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        count = 2;
    } else if (type == 2) {
        inputs[1].type       = INPUT_MOUSE;
        inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        count = 2;
    } else if (type == 3) {
        inputs[1].type       = INPUT_MOUSE;
        inputs[1].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
        inputs[2].type       = INPUT_MOUSE;
        inputs[2].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        count = 3;
    }

    SendInput(count, inputs, sizeof(INPUT));
}

void TouchReceiver::InjectUnicodeChar(uint32_t codepoint) const {
    if (codepoint == 0 || codepoint > 0x10FFFF) return;
    INPUT inputs[2] = {};
    inputs[0].type        = INPUT_KEYBOARD;
    inputs[0].ki.wScan    = static_cast<WORD>(codepoint);
    inputs[0].ki.dwFlags  = KEYEVENTF_UNICODE;
    inputs[1]             = inputs[0];
    inputs[1].ki.dwFlags  = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

void TouchReceiver::InjectVirtualKey(uint16_t vk, bool key_down) const {
    INPUT input = {};
    input.type      = INPUT_KEYBOARD;
    input.ki.wVk    = vk;
    input.ki.dwFlags = key_down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

void TouchReceiver::Stop() {
    running_ = false;
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    if (thread_.joinable()) thread_.join();
}
