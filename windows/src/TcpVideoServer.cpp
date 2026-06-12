#include "TcpVideoServer.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

enum UsbTcpPayloadType : uint8_t {
    kVideo      = 0,
    kCodec      = 1,
    kStreamInfo = 2,
    kCursor     = 3,
    kHello      = 4,  // Android→Windows handshake (first framed msg after connect)
};

// HELLO payload (after the type byte): version(1) mode(1) w(4BE) h(4BE) = 10 bytes
// Full frame: [len=11 4-byte BE][kHello][version=1][mode 0/1][w uint32BE][h uint32BE]
static constexpr uint8_t  kHelloProtocolVersion = 1;
static constexpr uint32_t kHelloPayloadSize     = 11;  // type(1)+version(1)+mode(1)+w(4)+h(4)

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

        // Read HELLO handshake: first framed message sent by Android after connecting.
        // Frame format: [4-byte BE length L][L bytes payload]
        // HELLO payload: [type=4][version=1][mode 0/1][w uint32BE][h uint32BE] = 11 bytes
        // A connection that closes before sending a valid HELLO is a TCP probe — discard
        // without touching client_sock_, mode_value_, or reconnect_cb_.
        {
            DWORD rcvto = 5000;
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<char*>(&rcvto), sizeof(rcvto));

            // Read 4-byte length prefix.
            uint8_t len_buf[4] = {};
            int     total      = 0;
            bool    ok         = true;
            while (total < 4) {
                const int n = recv(c, reinterpret_cast<char*>(len_buf + total), 4 - total, 0);
                if (n <= 0) { ok = false; break; }
                total += n;
            }

            DWORD zero = 0;
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<char*>(&zero), sizeof(zero));

            if (!ok) {
                // Connection closed / timed-out before HELLO arrived → TCP probe, discard.
                std::cout << "  [USB/video] no HELLO received (TCP probe?) — discarded, streaming unaffected\n";
                std::cout << "  [SOCK] closing probe socket c=" << c << " — reason: no HELLO (probe discard)\n";
                closesocket(c);
                continue;
            }

            const uint32_t msgLen = ntohl(*reinterpret_cast<uint32_t*>(len_buf));
            if (msgLen < 3 || msgLen > 64) {
                std::cout << "  [USB/video] invalid HELLO length " << msgLen << " — discarded\n";
                std::cout << "  [SOCK] closing c=" << c << " — reason: invalid HELLO length\n";
                closesocket(c);
                continue;
            }

            // Re-apply timeout for payload read.
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<char*>(&rcvto), sizeof(rcvto));

            std::vector<uint8_t> payload(msgLen);
            total = 0; ok = true;
            while (total < static_cast<int>(msgLen)) {
                const int n = recv(c, reinterpret_cast<char*>(payload.data() + total),
                                   static_cast<int>(msgLen) - total, 0);
                if (n <= 0) { ok = false; break; }
                total += n;
            }

            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<char*>(&zero), sizeof(zero));

            if (!ok) {
                std::cout << "  [USB/video] HELLO payload read failed — discarded\n";
                std::cout << "  [SOCK] closing c=" << c << " — reason: HELLO payload read failed\n";
                closesocket(c);
                continue;
            }

            const uint8_t type = payload[0];
            if (type != kHello) {
                std::cout << "  [USB/video] unexpected framed type=" << static_cast<int>(type)
                          << " (expected HELLO=4) — discarded\n";
                std::cout << "  [SOCK] closing c=" << c << " — reason: unexpected type (not HELLO)\n";
                closesocket(c);
                continue;
            }

            const uint8_t version = (msgLen >= 2) ? payload[1] : 0;
            if (version != kHelloProtocolVersion) {
                std::cout << "  [USB/video] unknown HELLO version=" << static_cast<int>(version)
                          << " — defaulting to Mirror\n";
            }

            const uint8_t mode_byte = (msgLen >= 3) ? payload[2] : 0;
            const int val = (mode_byte == 1) ? 1 : 0;

            int aW = 0, aH = 0;
            if (msgLen >= 11) {
                uint32_t w_net = 0, h_net = 0;
                std::memcpy(&w_net, payload.data() + 3, 4);
                std::memcpy(&h_net, payload.data() + 7, 4);
                aW = static_cast<int>(ntohl(w_net));
                aH = static_cast<int>(ntohl(h_net));
            }

            android_w_ = aW; android_h_ = aH;
            {
                std::lock_guard<std::mutex> lk(mode_mu_);
                mode_value_ = val;  // always update on each new connection
            }
            mode_cv_.notify_one();
            std::cout << "  [USB/video] HELLO v" << static_cast<int>(version)
                      << " received: " << (val == 1 ? "Extended" : "Mirror")
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
