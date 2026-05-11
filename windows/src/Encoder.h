#pragma once
#include <cstdint>
#include <vector>

extern "C" {
#include <x264.h>
}

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

    x264_t*        handle_  = nullptr;
    x264_picture_t pic_in_  = {};
    x264_picture_t pic_out_ = {};
    int            width_   = 0;
    int            height_  = 0;
    int64_t        pts_     = 0;
};
