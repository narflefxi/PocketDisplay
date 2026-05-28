#include "TcpVideoServer.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>

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
        listen(listen_sock_, SOMAXCONN) == SOCKET_ERROR) {
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

        char peer_ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &client_addr.sin_addr, peer_ip, sizeof(peer_ip));
        std::cout << "  [USB/video] accepted connection from " << peer_ip
                  << "  (client_sock_="
                  << (client_sock_ != INVALID_SOCKET ? "SET" : "INVALID") << ")\n";

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
            int recv_result = 0;
            while ((recv_result = recv(c, &ch, 1, 0)) == 1) {
                if (ch == '\n') break;
                if (ch != '\r') line += ch;
                if (line.size() > 128) break;
            }
            // Log recv exit reason to diagnose probe vs real connection.
            if (recv_result == 0) {
                std::cout << "  [USB/video] mode-read: peer closed (FIN) — line=\"" << line << "\"\n";
            } else if (recv_result < 0) {
                const int err = WSAGetLastError();
                if (err == WSAETIMEDOUT)
                    std::cout << "  [USB/video] mode-read: 5s SO_RCVTIMEO timeout — line=\"" << line << "\"\n";
                else
                    std::cout << "  [USB/video] mode-read: recv error " << err << " — line=\"" << line << "\"\n";
            }
            DWORD zero = 0;
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<char*>(&zero), sizeof(zero));

            // Empty mode line = TCP probe from Android usbPollRunnable (port reachability check).
            // Discard silently: do NOT replace client_sock_, do NOT call reconnect_cb_,
            // do NOT update mode_value_.  Without this guard the probe was closing the real
            // streaming socket every 3 s and waking WaitForMode() with a spurious Mirror value.
            if (line.empty()) {
                std::cout << "  [USB/video] empty mode line (TCP probe) — discarded, streaming unaffected\n";
                std::cout << "  [SOCK] closing probe socket c=" << c << " — reason: empty mode line (probe discard)\n";
                closesocket(c);
                continue;
            }

            std::cout << "  [USB/video] mode line received: \"" << line << "\"\n";
            // Parse: POCKETDISPLAY_MODE:<mode>[:<width>:<height>]
            int val = 0, aW = 0, aH = 0;
            if (line.rfind("POCKETDISPLAY_MODE:", 0) == 0) {
                const std::string rest = line.substr(19);
                const size_t p1 = rest.find(':');
                const std::string mstr = (p1 == std::string::npos) ? rest : rest.substr(0, p1);
                val = (mstr == "extend") ? 1 : 0;
                if (p1 != std::string::npos) {
                    const size_t p2 = rest.find(':', p1 + 1);
                    if (p2 != std::string::npos) {
                        try { aW = std::stoi(rest.substr(p1 + 1, p2 - p1 - 1)); } catch (...) {}
                        try { aH = std::stoi(rest.substr(p2 + 1)); } catch (...) {}
                    }
                }
            } else {
                // Unrecognised line (not a probe, not our protocol) — discard too.
                std::cout << "  [USB/video] unrecognised mode line — discarded\n";
                std::cout << "  [SOCK] closing unrecognised c=" << c << " — reason: unrecognised mode line\n";
                closesocket(c);
                continue;
            }
            android_w_ = aW; android_h_ = aH;
            {
                std::lock_guard<std::mutex> lk(mode_mu_);
                mode_value_ = val;  // always update on each new connection
            }
            mode_cv_.notify_one();
            std::cout << "  [TCP] valid mode received: "
                      << (val == 1 ? "Extended" : "Mirror")
                      << " screen=" << aW << "x" << aH
                      << " — keeping socket open, waiting for ACK\n";
            std::cout << "  [USB/video] Android connected — mode="
                      << (val == 1 ? "Extended" : "Mirror")
                      << "  screen=" << aW << "x" << aH << "\n";
        }

        {
            std::lock_guard<std::mutex> lock(client_mu_);
            if (client_sock_ != INVALID_SOCKET) {
                std::cout << "  [SOCK] closing client socket — reason: new Android connection replacing old\n";
                closesocket(client_sock_);
            }
            client_sock_ = c;
            std::cout << "  [SOCK] client_sock_ set to new socket " << c << "\n";
        }
        // Notify main that Android (re)connected — lets it reset android_ready
        // so the resend_thread re-sends codec config on every new connection.
        if (reconnect_cb_) reconnect_cb_();
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
            break;  // per-frame logging suppressed (too noisy at 60 fps)
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
            std::cout << "  [SOCK] closing client socket — reason: TcpVideoServer::Close() (shutdown)\n";
            closesocket(client_sock_);
            client_sock_ = INVALID_SOCKET;
        }
    }

    // Do not WSACleanup here — TouchReceiver may still be using Winsock on USB.
}
