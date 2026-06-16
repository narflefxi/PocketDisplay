#include "Encoder.h"
#include <cstring>
#include <cstdio>

#ifdef POCKETDISPLAY_ENABLE_X264
#include <excpt.h>

// Wraps x264_encoder_encode in SEH so an AV inside x264 returns -2 instead of crashing.
static int SafeX264Encode(x264_t* h, x264_nal_t** pp_nal, int* pi_nal,
                          x264_picture_t* pic_in, x264_picture_t* pic_out) {
    __try {
        return x264_encoder_encode(h, pp_nal, pi_nal, pic_in, pic_out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr, "[ENC] __except: AV inside x264_encoder_encode (code=0x%08lX) — frame skipped\n",
                GetExceptionCode());
        return -2;
    }
}

// Wraps x264_encoder_headers in SEH — called from resend_thread, separate from main encode path.
static int SafeX264Headers(x264_t* h, x264_nal_t** pp_nal, int* pi_nal) {
    __try {
        return x264_encoder_headers(h, pp_nal, pi_nal);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr, "[ENC] __except: AV inside x264_encoder_headers (code=0x%08lX) — headers skipped\n",
                GetExceptionCode());
        return -2;
    }
}
#endif

Encoder::~Encoder() {
    Close();
}

bool Encoder::Initialize(int width, int height, int fps, int bitrate_kbps) {
#ifdef POCKETDISPLAY_ENABLE_X264
    std::lock_guard<std::mutex> lock(mtx_);

    width_       = width;
    height_      = height;
    pts_         = 0;
    frame_count_ = 0;

    x264_param_t param;
    if (x264_param_default_preset(&param, "fast", "zerolatency") < 0) return false;

    // Screen content has hard edges (text, UI); deblocking smooths them into softness.
    param.b_deblocking_filter = 0;

    param.i_width          = width;
    param.i_height         = height;
    param.i_fps_num        = static_cast<uint32_t>(fps);
    param.i_fps_den        = 1;
    param.i_keyint_max     = fps * 2;
    param.rc.i_rc_method   = X264_RC_ABR;
    param.rc.i_bitrate     = bitrate_kbps;
    param.rc.i_vbv_max_bitrate = bitrate_kbps * 2;
    param.rc.i_vbv_buffer_size = bitrate_kbps;
    param.b_annexb         = 1;
    param.b_repeat_headers = 1;
    param.i_log_level      = X264_LOG_NONE;

    if (x264_param_apply_profile(&param, "high") < 0) return false;

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

    fprintf(stderr, "[ENC] Initialized %dx%d @ %d fps %d kbps\n",
            width, height, fps, bitrate_kbps);
    return true;
#else
    (void)width; (void)height; (void)fps; (void)bitrate_kbps;
    fprintf(stderr, "[ENC] ERROR: x264 not available (POCKETDISPLAY_ENABLE_X264 not defined)\n");
    return false;
#endif
}

bool Encoder::GetConfigPacket(std::vector<uint8_t>& sps_pps_out) {
#ifdef POCKETDISPLAY_ENABLE_X264
    std::lock_guard<std::mutex> lock(mtx_);
    if (!handle_) return false;

    x264_nal_t* nals  = nullptr;
    int         count = 0;
    if (SafeX264Headers(handle_, &nals, &count) < 0) return false;

    sps_pps_out.clear();
    for (int i = 0; i < count; ++i) {
        sps_pps_out.insert(sps_pps_out.end(),
            nals[i].p_payload, nals[i].p_payload + nals[i].i_payload);
    }
    return !sps_pps_out.empty();
#else
    (void)sps_pps_out;
    return false;
#endif
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
#ifdef POCKETDISPLAY_ENABLE_X264
    std::lock_guard<std::mutex> lock(mtx_);

    if (!bgra || !handle_) return false;

    // Validate plane buffers before touching them.
    if (!pic_in_.img.plane[0] || !pic_in_.img.plane[1] || !pic_in_.img.plane[2]) {
        fprintf(stderr, "[ENC] ERROR: null plane buffer (frame %llu) — skipping\n",
                static_cast<unsigned long long>(frame_count_));
        return false;
    }
    if (pic_in_.img.i_stride[0] < width_ || pic_in_.img.i_stride[1] < width_ / 2) {
        fprintf(stderr, "[ENC] ERROR: stride mismatch stride0=%d width=%d (frame %llu) — skipping\n",
                pic_in_.img.i_stride[0], width_,
                static_cast<unsigned long long>(frame_count_));
        return false;
    }

    ++frame_count_;
    if (frame_count_ % 60 == 1) {
        fprintf(stderr, "[ENC] frame %llu pts=%lld stride=%d size=%dx%d\n",
                static_cast<unsigned long long>(frame_count_),
                static_cast<long long>(pts_),
                pic_in_.img.i_stride[0], width_, height_);
    }

    BgraToI420(bgra, pic_in_.img.plane[0], pic_in_.img.plane[1], pic_in_.img.plane[2]);
    pic_in_.i_pts = pts_++;

    x264_nal_t* nals  = nullptr;
    int         count = 0;
    const int   frame_size = SafeX264Encode(handle_, &nals, &count, &pic_in_, &pic_out_);
    if (frame_size < 0) return false;

    nal_out.clear();
    if (frame_size == 0) return true;   // encoder buffering

    is_keyframe = (pic_out_.b_keyframe != 0);
    for (int i = 0; i < count; ++i) {
        nal_out.insert(nal_out.end(),
            nals[i].p_payload, nals[i].p_payload + nals[i].i_payload);
    }
    return true;
#else
    (void)bgra; (void)nal_out; (void)is_keyframe;
    return false;
#endif
}

void Encoder::Close() {
#ifdef POCKETDISPLAY_ENABLE_X264
    std::lock_guard<std::mutex> lock(mtx_);
    if (!handle_) return;
    delete[] pic_in_.img.plane[0];
    delete[] pic_in_.img.plane[1];
    delete[] pic_in_.img.plane[2];
    pic_in_.img.plane[0] = nullptr;
    pic_in_.img.plane[1] = nullptr;
    pic_in_.img.plane[2] = nullptr;
    x264_encoder_close(handle_);
    handle_ = nullptr;
    pts_    = 0;
    fprintf(stderr, "[ENC] Closed (total frames encoded: %llu)\n",
            static_cast<unsigned long long>(frame_count_));
#endif
}
