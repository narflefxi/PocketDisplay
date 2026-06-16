#pragma once
#include <cstdint>
#include <vector>
#include <mutex>

#ifdef POCKETDISPLAY_ENABLE_X264
extern "C" {
#include <x264.h>
}
#endif

// Software H.264 encoder (x264 GPL) - only available when POCKETDISPLAY_ENABLE_X264 is defined.
// For non-GPL builds, use HwEncoder (Media Foundation) instead.
class Encoder {
public:
    Encoder() = default;
    ~Encoder();

    bool Initialize(int width, int height, int fps = 60, int bitrate_kbps = 8000);
    // Retrieve SPS/PPS headers to send as a codec-config packet.
    bool GetConfigPacket(std::vector<uint8_t>& sps_pps_out);
    // Encode one BGRA frame. nal_out is empty when encoder is buffering.
    bool EncodeFrame(const uint8_t* bgra, std::vector<uint8_t>& nal_out, bool& is_keyframe);
    void Close();

private:
    void BgraToI420(const uint8_t* bgra,
                    uint8_t* y_plane, uint8_t* u_plane, uint8_t* v_plane) const;

    std::mutex     mtx_;
#ifdef POCKETDISPLAY_ENABLE_X264
    x264_t*        handle_      = nullptr;
    x264_picture_t pic_in_      = {};
    x264_picture_t pic_out_     = {};
#endif
    int            width_       = 0;
    int            height_      = 0;
    int64_t        pts_         = 0;
    uint64_t       frame_count_ = 0;
};
