#pragma once
#include <cstdint>

namespace pocketdisplay {

constexpr uint8_t  MAGIC[4]           = {'P', 'D', 'S', 'M'};
constexpr uint16_t DEFAULT_PORT       = 7777;
constexpr size_t   MAX_PACKET_PAYLOAD = 1380;  // safe UDP payload under 1500 byte MTU

enum PacketFlags : uint8_t {
    FLAG_NONE         = 0x00,
    FLAG_CODEC_CONFIG = 0x01,  // SPS/PPS headers
    FLAG_KEYFRAME     = 0x02,  // IDR frame
    FLAG_STREAM_INFO  = 0x04,  // 8-byte payload: uint32 width + uint32 height (network byte order)
    FLAG_CURSOR_POS   = 0x08,  // 9-byte payload: float nx + float ny + uint8 cursor_type (network byte order, normalized [0,1])
};

#pragma pack(push, 1)
struct PacketHeader {
    uint8_t  magic[4];       // 'P','D','S','M'
    uint32_t frame_id;       // frame sequence number (network byte order)
    uint16_t packet_idx;     // 0-based index within this frame (network byte order)
    uint16_t total_packets;  // total packet count for this frame (network byte order)
    uint32_t frame_size;     // total assembled frame size in bytes (network byte order)
    uint8_t  flags;          // PacketFlags bitmask
    uint8_t  reserved[3];
    // payload bytes follow immediately after this header
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 20, "PacketHeader size must be 20 bytes");

} // namespace pocketdisplay
