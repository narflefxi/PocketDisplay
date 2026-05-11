#include "HwEncoder.h"

#include <mferror.h>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <iostream>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

// ── Annex-B NAL helpers ──────────────────────────────────────────────────────

static bool FindNalStartCode(const uint8_t* data, size_t size,
                              size_t start, size_t& sc_pos, int& sc_len) {
    for (size_t i = start; i + 3 < size; ++i) {
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) {
            sc_pos = i; sc_len = 4; return true;
        }
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) {
            sc_pos = i; sc_len = 3; return true;
        }
    }
    return false;
}

// Extract SPS (type 7) + PPS (type 8) NALs from an Annex-B bitstream.
static bool ParseSpsPps(const uint8_t* data, size_t size,
                         std::vector<uint8_t>& sps_pps_out,
                         bool& has_idr) {
    sps_pps_out.clear();
    has_idr = false;

    size_t pos = 0;
    while (pos < size) {
        size_t sc_pos; int sc_len;
        if (!FindNalStartCode(data, size, pos, sc_pos, sc_len)) break;

        size_t nal_start = sc_pos + sc_len;
        if (nal_start >= size) break;

        // Find end of NAL (next start code)
        size_t nal_end = size;
        size_t next_sc; int next_sc_len;
        if (FindNalStartCode(data, size, nal_start + 1, next_sc, next_sc_len))
            nal_end = next_sc;

        const uint8_t nal_type = data[nal_start] & 0x1F;
        if (nal_type == 7 || nal_type == 8) {
            // Append with 4-byte start code
            uint8_t sc4[4] = {0, 0, 0, 1};
            sps_pps_out.insert(sps_pps_out.end(), sc4, sc4 + 4);
            sps_pps_out.insert(sps_pps_out.end(), data + nal_start, data + nal_end);
        }
        if (nal_type == 5) has_idr = true;

        pos = nal_end;
    }
    return !sps_pps_out.empty();
}

// ── BGRA → NV12 ─────────────────────────────────────────────────────────────

void HwEncoder::BgraToNv12(const uint8_t* bgra, uint8_t* nv12) const {
    uint8_t* y_plane  = nv12;
    uint8_t* uv_plane = nv12 + width_ * height_;

    for (int row = 0; row < height_; ++row) {
        for (int col = 0; col < width_; ++col) {
            const uint8_t* px = bgra + (row * width_ + col) * 4;
            const uint8_t b = px[0], g = px[1], r = px[2];
            y_plane[row * width_ + col] =
                static_cast<uint8_t>(((66*r + 129*g + 25*b + 128) >> 8) + 16);

            if ((row & 1) == 0 && (col & 1) == 0) {
                const int uv_idx = (row / 2) * width_ + col;
                uv_plane[uv_idx]     = static_cast<uint8_t>(((-38*r - 74*g + 112*b + 128) >> 8) + 128);
                uv_plane[uv_idx + 1] = static_cast<uint8_t>(((112*r - 94*g - 18*b + 128) >> 8) + 128);
            }
        }
    }
}

ComPtr<IMFSample> HwEncoder::MakeNv12Sample(const uint8_t* bgra, int64_t pts_us) {
    const size_t nv12_size = static_cast<size_t>(width_ * height_ * 3 / 2);

    ComPtr<IMFMediaBuffer> media_buf;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(nv12_size), &media_buf)))
        return nullptr;

    BYTE* ptr = nullptr; DWORD max_len = 0;
    if (FAILED(media_buf->Lock(&ptr, &max_len, nullptr))) return nullptr;

    BgraToNv12(bgra, ptr);
    media_buf->Unlock();
    media_buf->SetCurrentLength(static_cast<DWORD>(nv12_size));

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return nullptr;
    sample->AddBuffer(media_buf.Get());
    sample->SetSampleTime(pts_us * 10);   // 100-ns units
    sample->SetSampleDuration(0);
    return sample;
}

// ── Async event loop (runs on event_thread_) ─────────────────────────────────

void HwEncoder::EventLoop() {
    while (running_) {
        ComPtr<IMFMediaEvent> event;
        HRESULT hr = event_gen_->GetEvent(0, &event);   // blocking
        if (FAILED(hr)) break;

        MediaEventType type = MEUnknown;
        event->GetType(&type);

        if (type == METransformNeedInput) {
            std::unique_lock<std::mutex> lk(input_mutex_);
            if (!pending_samples_.empty()) {
                auto sample = pending_samples_.front();
                pending_samples_.pop();
                lk.unlock();
                encoder_->ProcessInput(0, sample.Get(), 0);
            } else {
                needs_input_ = true;
                lk.unlock();
                needs_input_cv_.notify_one();
            }
        } else if (type == METransformHaveOutput) {
            DrainOutput();
        }
    }
}

void HwEncoder::DrainOutput() {
    MFT_OUTPUT_DATA_BUFFER out_buf = {};
    DWORD status = 0;

    // Some hardware MFTs allocate their own output samples
    HRESULT hr = encoder_->ProcessOutput(0, 1, &out_buf, &status);
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        // Output type changed — ignore and re-query
        ComPtr<IMFMediaType> ignored;
        encoder_->GetOutputCurrentType(0, &ignored);
        return;
    }
    if (FAILED(hr)) return;

    if (out_buf.pSample) {
        ComPtr<IMFMediaBuffer> media_buf;
        out_buf.pSample->ConvertToContiguousBuffer(&media_buf);
        out_buf.pSample->Release();

        BYTE* data = nullptr; DWORD len = 0;
        if (FAILED(media_buf->Lock(&data, nullptr, &len))) return;

        std::vector<uint8_t> nal_data(data, data + len);
        media_buf->Unlock();

        if (!sps_pps_found_) {
            bool has_idr = false;
            if (ParseSpsPps(nal_data.data(), nal_data.size(), sps_pps_cache_, has_idr))
                sps_pps_found_ = true;
        }

        {
            std::lock_guard<std::mutex> lk(output_mutex_);
            output_queue_.push(std::move(nal_data));
        }
        output_cv_.notify_one();
    }

    if (out_buf.pEvents) out_buf.pEvents->Release();
}

// ── Public interface ─────────────────────────────────────────────────────────

bool HwEncoder::Initialize(int width, int height, int fps, int bitrate_kbps) {
    width_  = width;
    height_ = height;

    if (FAILED(MFStartup(MF_VERSION))) return false;

    // Enumerate hardware H.264 encoders (NV12 → H264)
    MFT_REGISTER_TYPE_INFO in_type  = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO out_type = { MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
              MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT |
              MFT_ENUM_FLAG_SORTANDFILTER,
              &in_type, &out_type, &activates, &count);

    if (count == 0) {
        std::cout << "      No hardware H.264 encoder found, falling back to x264.\n";
        MFShutdown();
        return false;
    }

    HRESULT hr = activates[0]->ActivateObject(IID_PPV_ARGS(&encoder_));
    PWSTR name_w = nullptr;
    UINT32 name_len = 0;
    if (SUCCEEDED(activates[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name_w, &name_len))) {
        // Convert wide string to narrow for logging
        char name_a[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, name_w, -1, name_a, sizeof(name_a), nullptr, nullptr);
        std::cout << "      Hardware encoder: " << name_a << "\n";
        CoTaskMemFree(name_w);
    }
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    if (FAILED(hr)) { MFShutdown(); return false; }

    // Unlock async transform
    ComPtr<IMFAttributes> attrs;
    encoder_->GetAttributes(&attrs);
    if (attrs) attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);

    hr = encoder_.As(&event_gen_);
    if (FAILED(hr)) { MFShutdown(); return false; }

    // Set output type FIRST (required for hardware encoders)
    ComPtr<IMFMediaType> mt_out;
    MFCreateMediaType(&mt_out);
    mt_out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mt_out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(mt_out.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(mt_out.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(mt_out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    mt_out->SetUINT32(MF_MT_INTERLACE_MODE,  MFVideoInterlace_Progressive);
    mt_out->SetUINT32(MF_MT_AVG_BITRATE,     bitrate_kbps * 1000);
    mt_out->SetUINT32(MF_MT_MPEG2_PROFILE,   66);  // H.264 Baseline = 66
    mt_out->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    encoder_->SetOutputType(0, mt_out.Get(), 0);

    // Set input type
    ComPtr<IMFMediaType> mt_in;
    MFCreateMediaType(&mt_in);
    mt_in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mt_in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(mt_in.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(mt_in.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(mt_in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    mt_in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    mt_in->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    encoder_->SetInputType(0, mt_in.Get(), 0);

    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    running_ = true;
    event_thread_ = std::thread(&HwEncoder::EventLoop, this);
    return true;
}

bool HwEncoder::GetConfigPacket(std::vector<uint8_t>& sps_pps_out) {
    if (sps_pps_found_) {
        sps_pps_out = sps_pps_cache_;
        return true;
    }
    // Not available yet — caller should retry after first keyframe
    return false;
}

bool HwEncoder::EncodeFrame(const uint8_t* bgra,
                              std::vector<uint8_t>& nal_out,
                              bool& is_keyframe) {
    auto sample = MakeNv12Sample(bgra, pts_++);
    if (!sample) return false;

    {
        std::unique_lock<std::mutex> lk(input_mutex_);
        if (needs_input_) {
            needs_input_ = false;
            lk.unlock();
            encoder_->ProcessInput(0, sample.Get(), 0);
        } else {
            pending_samples_.push(sample);
        }
    }

    // Wait up to one frame time for output
    std::unique_lock<std::mutex> out_lk(output_mutex_);
    output_cv_.wait_for(out_lk, std::chrono::milliseconds(33),
                        [this] { return !output_queue_.empty(); });

    if (output_queue_.empty()) {
        nal_out.clear();
        return true;   // Encoder is pipelining — output arrives next call
    }

    nal_out = std::move(output_queue_.front());
    output_queue_.pop();

    // Detect IDR
    is_keyframe = false;
    for (size_t i = 0; i + 3 < nal_out.size(); ++i) {
        if (nal_out[i] == 0 && nal_out[i+1] == 0 &&
            nal_out[i+2] == 0 && nal_out[i+3] == 1) {
            if (i + 4 < nal_out.size()) {
                if ((nal_out[i+4] & 0x1F) == 5) { is_keyframe = true; break; }
            }
        }
    }
    return true;
}

void HwEncoder::Close() {
    if (!running_) return;
    running_ = false;
    if (encoder_) {
        encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    output_cv_.notify_all();
    needs_input_cv_.notify_all();
    if (event_thread_.joinable()) event_thread_.join();
    encoder_.Reset();
    event_gen_.Reset();
    MFShutdown();
    pts_ = 0;
    sps_pps_found_ = false;
}
