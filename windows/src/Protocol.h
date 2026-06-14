#pragma once
#include <cstdint>

namespace pocketdisplay {

constexpr uint16_t DEFAULT_PORT = 7777;

// Flags used in DirectSocketStreamer / Session to identify TCP message types.
enum PacketFlags : uint8_t {
    FLAG_NONE         = 0x00,
    FLAG_CODEC_CONFIG = 0x01,  // SPS/PPS headers
    FLAG_KEYFRAME     = 0x02,  // IDR frame
    FLAG_STREAM_INFO  = 0x04,  // 12-byte payload: uint32 w, uint32 h, uint32 mode_flags
    FLAG_CURSOR_POS   = 0x08,  // 9-byte payload: float nx, float ny, uint8 cursor_type
};

// Sentinel cursor_type value sent in a FLAG_CURSOR_POS packet when the PC cursor leaves
// the extended display region (extended mode only).  Android hides the overlay on receipt.
// Normal cursor types are small integers 0–11; 0xFF is safely outside that range.
constexpr uint8_t CURSOR_TYPE_HIDDEN = 0xFF;

} // namespace pocketdisplay
