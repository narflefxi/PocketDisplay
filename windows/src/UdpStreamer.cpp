#include "UdpStreamer.h"
#include <cstring>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

UdpStreamer::~UdpStreamer() {
    Close();
}

bool UdpStreamer::Initialize(const std::string& target_ip, uint16_t port) {
    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) return false;

    // Large send buffer to absorb burst traffic
    int buf_size = 4 * 1024 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&buf_size), sizeof(buf_size));

    dest_.sin_family = AF_INET;
    dest_.sin_port   = htons(port);
    inet_pton(AF_INET, target_ip.c_str(), &dest_.sin_addr);

    return true;
}

bool UdpStreamer::SendFrame(const uint8_t* data, size_t size,
                            uint32_t frame_id, uint8_t flags) {
    using namespace pocketdisplay;

    const size_t total_pkts = (size + MAX_PACKET_PAYLOAD - 1) / MAX_PACKET_PAYLOAD;
    if (total_pkts > 0xFFFF) return false;  // frame too large

    uint8_t pkt_buf[sizeof(PacketHeader) + MAX_PACKET_PAYLOAD];

    for (size_t i = 0; i < total_pkts; ++i) {
        const size_t offset      = i * MAX_PACKET_PAYLOAD;
        const size_t payload_len = std::min(MAX_PACKET_PAYLOAD, size - offset);

        auto* hdr = reinterpret_cast<PacketHeader*>(pkt_buf);
        std::memcpy(hdr->magic, MAGIC, 4);
        hdr->frame_id      = htonl(frame_id);
        hdr->packet_idx    = htons(static_cast<uint16_t>(i));
        hdr->total_packets = htons(static_cast<uint16_t>(total_pkts));
        hdr->frame_size    = htonl(static_cast<uint32_t>(size));
        hdr->flags         = flags;
        hdr->reserved[0]   = hdr->reserved[1] = hdr->reserved[2] = 0;

        std::memcpy(pkt_buf + sizeof(PacketHeader), data + offset, payload_len);

        const int send_len = static_cast<int>(sizeof(PacketHeader) + payload_len);
        sendto(sock_, reinterpret_cast<const char*>(pkt_buf), send_len, 0,
               reinterpret_cast<const sockaddr*>(&dest_), sizeof(dest_));
    }
    return true;
}

void UdpStreamer::Close() {
    if (sock_ == INVALID_SOCKET) return;
    closesocket(sock_);
    sock_ = INVALID_SOCKET;
    WSACleanup();
}
