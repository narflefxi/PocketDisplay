#include "TouchReceiver.h"
#include <windows.h>
#include <ws2tcpip.h>
#include <cstring>

TouchReceiver::~TouchReceiver() { Stop(); }

bool TouchReceiver::Start(uint16_t port) {
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
    thread_  = std::thread(&TouchReceiver::ReceiverLoop, this);
    return true;
}

// Must match TouchSender.kt packet layout exactly (big-endian, 16 bytes)
#pragma pack(push, 1)
struct TouchPacket {
    uint8_t magic[4];
    uint8_t type;
    uint8_t reserved[3];
    float   nx;   // big-endian
    float   ny;   // big-endian
};
#pragma pack(pop)
static_assert(sizeof(TouchPacket) == 16, "TouchPacket size mismatch");

static float be_to_host_float(float f) {
    uint32_t tmp;
    std::memcpy(&tmp, &f, 4);
    tmp = ntohl(tmp);
    std::memcpy(&f, &tmp, 4);
    return f;
}

void TouchReceiver::ReceiverLoop() {
    uint8_t buf[sizeof(TouchPacket)];

    while (running_) {
        int n = recv(sock_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n <= 0) break;
        if (n < static_cast<int>(sizeof(TouchPacket))) continue;

        auto* pkt = reinterpret_cast<const TouchPacket*>(buf);
        if (pkt->magic[0] != 'P' || pkt->magic[1] != 'D' ||
            pkt->magic[2] != 'T' || pkt->magic[3] != 'I') continue;

        ApplyEvent(pkt->type, be_to_host_float(pkt->nx), be_to_host_float(pkt->ny));
    }
}

void TouchReceiver::ApplyEvent(uint8_t type, float nx, float ny) const {
    // MOUSEEVENTF_ABSOLUTE: [0, 65535] spans the primary monitor
    const LONG ax = static_cast<LONG>(nx * 65535.0f);
    const LONG ay = static_cast<LONG>(ny * 65535.0f);

    INPUT inputs[2] = {};
    int   count     = 1;

    inputs[0].type       = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
    inputs[0].mi.dx      = ax;
    inputs[0].mi.dy      = ay;

    if (type == 1) {        // DOWN → move + left button press
        inputs[1].type       = INPUT_MOUSE;
        inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        count = 2;
    } else if (type == 2) { // UP → left button release
        inputs[1].type       = INPUT_MOUSE;
        inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        count = 2;
    }
    // type == 0 (MOVE): just the move, count stays 1

    SendInput(count, inputs, sizeof(INPUT));
}

void TouchReceiver::Stop() {
    running_ = false;
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    if (thread_.joinable()) thread_.join();
}
