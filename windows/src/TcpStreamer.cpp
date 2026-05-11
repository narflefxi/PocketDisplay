#include "TcpStreamer.h"
#include <cstring>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

TcpStreamer::~TcpStreamer() { Close(); }

bool TcpStreamer::Initialize(const std::string& target_ip, uint16_t port) {
    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ == INVALID_SOCKET) return false;

    int flag = 1;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&flag), sizeof(flag));
    int buf_size = 4 * 1024 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&buf_size), sizeof(buf_size));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, target_ip.c_str(), &addr.sin_addr);

    if (connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        WSACleanup();
        return false;
    }
    return true;
}

bool TcpStreamer::SendAll(SOCKET s, const uint8_t* buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, reinterpret_cast<const char*>(buf + sent), len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

// Each chunk is framed as: [4-byte BE length][PacketHeader + payload]
// This mirrors the UDP packet layout so Android can reuse the same parser.
bool TcpStreamer::SendFrame(const uint8_t* data, size_t size,
                             uint32_t frame_id, uint8_t flags) {
    using namespace pocketdisplay;

    const size_t total_pkts = (size + MAX_PACKET_PAYLOAD - 1) / MAX_PACKET_PAYLOAD;
    if (total_pkts > 0xFFFF) return false;

    uint8_t pkt_buf[sizeof(PacketHeader) + MAX_PACKET_PAYLOAD];

    for (size_t i = 0; i < total_pkts; ++i) {
        const size_t  offset      = i * MAX_PACKET_PAYLOAD;
        const size_t  payload_len = std::min(MAX_PACKET_PAYLOAD, size - offset);
        const uint32_t pkt_len   = static_cast<uint32_t>(sizeof(PacketHeader) + payload_len);

        uint8_t len_prefix[4];
        const uint32_t pkt_len_be = htonl(pkt_len);
        std::memcpy(len_prefix, &pkt_len_be, 4);

        auto* hdr = reinterpret_cast<PacketHeader*>(pkt_buf);
        std::memcpy(hdr->magic, MAGIC, 4);
        hdr->frame_id      = htonl(frame_id);
        hdr->packet_idx    = htons(static_cast<uint16_t>(i));
        hdr->total_packets = htons(static_cast<uint16_t>(total_pkts));
        hdr->frame_size    = htonl(static_cast<uint32_t>(size));
        hdr->flags         = flags;
        hdr->reserved[0]   = hdr->reserved[1] = hdr->reserved[2] = 0;
        std::memcpy(pkt_buf + sizeof(PacketHeader), data + offset, payload_len);

        if (!SendAll(sock_, len_prefix, 4)) return false;
        if (!SendAll(sock_, pkt_buf, static_cast<int>(pkt_len))) return false;
    }
    return true;
}

void TcpStreamer::Close() {
    if (sock_ == INVALID_SOCKET) return;
    closesocket(sock_);
    sock_ = INVALID_SOCKET;
    WSACleanup();
}
