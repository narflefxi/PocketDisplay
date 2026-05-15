#include "TcpVideoServer.h"

#include <chrono>
#include <cstring>
#include <iostream>

#pragma comment(lib, "ws2_32.lib")

namespace {

enum UsbTcpPayloadType : uint8_t {
    kVideo      = 0,
    kCodec      = 1,
    kStreamInfo = 2,
    kCursor     = 3,
};

} // namespace

TcpVideoServer::~TcpVideoServer() { Close(); }

bool TcpVideoServer::SendAll(SOCKET s, const uint8_t* buf, int len) {
    int sent = 0;
    while (sent < len) {
        const int n = send(s, reinterpret_cast<const char*>(buf + sent), len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

bool TcpVideoServer::StartListen(uint16_t port) {
    Close();

    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "  [USB/video] WSAStartup failed\n";
        return false;
    }
    listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == INVALID_SOCKET) {
        std::cerr << "  [USB/video] socket() failed\n";
        Close();
        return false;
    }

    int reuse = 1;
    setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    int flag = 1;
    setsockopt(listen_sock_, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&flag), sizeof(flag));

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(listen_sock_, 1) == SOCKET_ERROR) {
        std::cerr << "  [USB/video] bind/listen on port " << port << " failed\n";
        Close();
        return false;
    }

    running_       = true;
    accept_thread_ = std::thread(&TcpVideoServer::AcceptLoop, this);
    std::cout << "  [USB/video] Listening on TCP :" << port
              << " (adb reverse tcp:" << port << " tcp:" << port << ")\n";
    return true;
}

void TcpVideoServer::AcceptLoop() {
    while (running_) {
        const SOCKET ls = listen_sock_;
        if (ls == INVALID_SOCKET) break;

        sockaddr_in client_addr = {};
        int         addr_len = sizeof(client_addr);
        SOCKET      c = accept(ls, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (c == INVALID_SOCKET) {
            if (running_) std::cerr << "  [USB/video] accept failed\n";
            break;
        }

        int flag = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));
        int buf_size = 4 * 1024 * 1024;
        setsockopt(c, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf_size), sizeof(buf_size));

        // Read mode selection line before streaming begins.
        {
            DWORD rcvto = 5000;
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<char*>(&rcvto), sizeof(rcvto));
            std::string line;
            char ch;
            while (recv(c, &ch, 1, 0) == 1) {
                if (ch == '\n') break;
                if (ch != '\r') line += ch;
                if (line.size() > 128) break;
            }
            DWORD zero = 0;
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<char*>(&zero), sizeof(zero));

            const int val = (line.rfind("POCKETDISPLAY_MODE:", 0) == 0 &&
                             line.substr(19) == "extend") ? 1 : 0;
            {
                std::lock_guard<std::mutex> lk(mode_mu_);
                if (mode_value_ < 0) mode_value_ = val;  // keep first
            }
            mode_cv_.notify_one();
            std::cout << "  [USB/video] Android connected — mode="
                      << (val == 1 ? "Extended" : "Mirror") << "\n";
        }

        {
            std::lock_guard<std::mutex> lock(client_mu_);
            if (client_sock_ != INVALID_SOCKET) closesocket(client_sock_);
            client_sock_ = c;
        }
    }
}

bool TcpVideoServer::SendFrame(const uint8_t* data, size_t size, uint32_t /*frame_id*/,
                               uint8_t flags) {
    using namespace pocketdisplay;

    UsbTcpPayloadType t = kVideo;
    if ((flags & FLAG_CODEC_CONFIG) != 0) t = kCodec;
    else if ((flags & FLAG_STREAM_INFO) != 0) t = kStreamInfo;
    else if ((flags & FLAG_CURSOR_POS) != 0) t = kCursor;

    const uint32_t body = static_cast<uint32_t>(1u + size);
    if (body < 1) return false;

    switch (t) {
        case kStreamInfo:
            std::cout << "  [USB/video] send type=" << static_cast<int>(t)
                      << " display_size payload=" << size << "\n";
            break;
        case kCodec:
            std::cout << "  [USB/video] send type=" << static_cast<int>(t)
                      << " codec_config payload=" << size << "\n";
            break;
        case kVideo:
            std::cout << "  [USB/video] send type=" << static_cast<int>(t)
                      << " video_frame payload=" << size << "\n";
            break;
        case kCursor:
            break;
    }

    const uint32_t be = htonl(body);
    std::lock_guard<std::mutex> lock(client_mu_);
    if (client_sock_ == INVALID_SOCKET) return false;
    if (!SendAll(client_sock_, reinterpret_cast<const uint8_t*>(&be), 4)) return false;
    if (!SendAll(client_sock_, reinterpret_cast<const uint8_t*>(&t), 1)) return false;
    if (size > 0 && !SendAll(client_sock_, data, static_cast<int>(size))) return false;
    return true;
}

bool TcpVideoServer::WaitForMode(int timeout_ms, bool& extend_out) {
    std::unique_lock<std::mutex> lk(mode_mu_);
    const bool got = mode_cv_.wait_for(
        lk, std::chrono::milliseconds(timeout_ms),
        [this] { return mode_value_ >= 0 || !running_; });
    if (!got || mode_value_ < 0) {
        extend_out = false;  // timeout → Mirror
        return false;
    }
    extend_out = (mode_value_ == 1);
    return true;
}

void TcpVideoServer::Close() {
    running_ = false;

    if (listen_sock_ != INVALID_SOCKET) {
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
    }
    if (accept_thread_.joinable()) accept_thread_.join();

    {
        std::lock_guard<std::mutex> lock(client_mu_);
        if (client_sock_ != INVALID_SOCKET) {
            closesocket(client_sock_);
            client_sock_ = INVALID_SOCKET;
        }
    }

    // Do not WSACleanup here — TouchReceiver may still be using Winsock on USB.
}
