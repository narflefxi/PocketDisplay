#include "Encoder.h"
#include <cstring>

Encoder::~Encoder() {
    Close();
}

bool Encoder::Initialize(int width, int height, int fps, int bitrate_kbps) {
    width_  = width;
    height_ = height;

    x264_param_t param;
    // ultrafast + zerolatency: minimizes encode delay at cost of compression ratio
    if (x264_param_default_preset(&param, "ultrafast", "zerolatency") < 0) return false;

    param.i_width          = width;
    param.i_height         = height;
    param.i_fps_num        = static_cast<uint32_t>(fps);
    param.i_fps_den        = 1;
    param.i_keyint_max     = fps * 2;   // force keyframe every 2 s for reconnect recovery
    param.rc.i_rc_method   = X264_RC_ABR;
    param.rc.i_bitrate     = bitrate_kbps;
    param.b_annexb         = 1;
    param.b_repeat_headers = 1;         // embed SPS/PPS in every keyframe for resilience
    param.i_log_level      = X264_LOG_NONE;

    if (x264_param_apply_profile(&param, "baseline") < 0) return false;

    handle_ = x264_encoder_open(&param);
    if (!handle_) return false;

    x264_picture_init(&pic_in_);
    pic_in_.img.i_csp   = X264_CSP_I420;
    pic_in_.img.i_plane = 3;

    const int y_size  = width * height;
    const int uv_size = (width / 2) * (height / 2);
    pic_in_.img.plane[0] = new uint8_t[y_size];
    pic_in_.img.plane[1] = new uint8_t[uv_size];
    pic_in_.img.plane[2] = new uint8_t[uv_size];
    pic_in_.img.i_stride[0] = width;
    pic_in_.img.i_stride[1] = width / 2;
    pic_in_.img.i_stride[2] = width / 2;

    return true;
}

bool Encoder::GetConfigPacket(std::vector<uint8_t>& sps_pps_out) {
    x264_nal_t* nals   = nullptr;
    int         count  = 0;
    if (x264_encoder_headers(handle_, &nals, &count) < 0) return false;

    sps_pps_out.clear();
    for (int i = 0; i < count; ++i) {
        sps_pps_out.insert(sps_pps_out.end(),
            nals[i].p_payload, nals[i].p_payload + nals[i].i_payload);
    }
    return !sps_pps_out.empty();
}

void Encoder::BgraToI420(const uint8_t* bgra,
                          uint8_t* y_plane, uint8_t* u_plane, uint8_t* v_plane) const {
    const int uv_w = width_ / 2;

    for (int row = 0; row < height_; ++row) {
        for (int col = 0; col < width_; ++col) {
            const uint8_t* px = bgra + (row * width_ + col) * 4;
            const uint8_t  b  = px[0], g = px[1], r = px[2];

            y_plane[row * width_ + col] =
                static_cast<uint8_t>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);

            if ((row & 1) == 0 && (col & 1) == 0) {
                const int uv_idx = (row / 2) * uv_w + (col / 2);
                u_plane[uv_idx] = static_cast<uint8_t>(((-38*r - 74*g + 112*b + 128) >> 8) + 128);
                v_plane[uv_idx] = static_cast<uint8_t>(((112*r - 94*g -  18*b + 128) >> 8) + 128);
            }
        }
    }
}

bool Encoder::EncodeFrame(const uint8_t* bgra,
                           std::vector<uint8_t>& nal_out, bool& is_keyframe) {
    BgraToI420(bgra, pic_in_.img.plane[0], pic_in_.img.plane[1], pic_in_.img.plane[2]);
    pic_in_.i_pts = pts_++;

    x264_nal_t* nals  = nullptr;
    int         count = 0;
    const int   frame_size = x264_encoder_encode(handle_, &nals, &count, &pic_in_, &pic_out_);
    if (frame_size < 0) return false;

    nal_out.clear();
    if (frame_size == 0) return true;   // encoder buffering

    is_keyframe = (pic_out_.b_keyframe != 0);
    for (int i = 0; i < count; ++i) {
        nal_out.insert(nal_out.end(),
            nals[i].p_payload, nals[i].p_payload + nals[i].i_payload);
    }
    return true;
}

void Encoder::Close() {
    if (!handle_) return;
    delete[] pic_in_.img.plane[0];
    delete[] pic_in_.img.plane[1];
    delete[] pic_in_.img.plane[2];
    x264_encoder_close(handle_);
    handle_ = nullptr;
    pts_    = 0;
}
