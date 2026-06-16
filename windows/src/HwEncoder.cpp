#include "HwEncoder.h"

#include <mferror.h>
#include <strmif.h>   // for CODECAPI_AVEncVideoForceKeyFrame
#include <codecapi.h> // for CODECAPI_AVEncVideoForceKeyFrame, CODECAPI_AVEncMPVGOPSize
#include <cstring>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <iomanip>    // for hex logging

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "strmiids.lib")

using Microsoft::WRL::ComPtr;

// ── Pipeline instrumentation counters ─────────────────────────────────────────
static std::atomic<int> s_captured_frames{0};
static std::atomic<int> s_encode_calls{0};
static std::atomic<int> s_encode_outputs{0};
static std::atomic<int> s_sent_bytes{0};
static std::atomic<int> s_send_errors{0};
static std::atomic<int> s_need_input_events{0};
static std::atomic<int> s_have_output_events{0};
static std::atomic<int> s_process_input_calls{0};
static std::atomic<int> s_process_output_calls{0};

// ── Diagnostic logging helpers ────────────────────────────────────────────────

static void LogHex(const std::string& prefix, const uint8_t* data, size_t len) {
    std::cout << prefix;
    const size_t max_bytes = 16;
    for (size_t i = 0; i < std::min(len, max_bytes); ++i) {
        std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)data[i] << " ";
    }
    if (len > max_bytes) std::cout << "...";
    std::cout << std::dec << " (size=" << len << ")\n";
}

// ── NAL format helpers ───────────────────────────────────────────────────────

static const uint8_t kAnnexBStartCode[] = {0, 0, 0, 1};

// Convert AVCC (length-prefixed) to Annex-B (start code) format.
// If output_annexb is empty, input may already be Annex-B (no conversion needed).
static bool ConvertAvccToAnnexB(const uint8_t* data, size_t size,
                                 std::vector<uint8_t>& output_annexb) {
    output_annexb.clear();
    if (size < 4) return false;

    // Check if already Annex-B (starts with 00 00 00 01 or 00 00 01)
    if ((data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) ||
        (data[0] == 0 && data[1] == 0 && data[2] == 1)) {
        output_annexb.assign(data, data + size);
        return true;
    }

    // Assume AVCC format: 4-byte big-endian length prefix per NAL unit
    size_t pos = 0;
    while (pos + 4 <= size) {
        uint32_t nal_len = (data[pos] << 24) | (data[pos+1] << 16) |
                          (data[pos+2] << 8) | data[pos+3];
        pos += 4;
        if (nal_len > size - pos || nal_len == 0) break;

        // Add Annex-B start code
        output_annexb.insert(output_annexb.end(),
                             kAnnexBStartCode, kAnnexBStartCode + 4);
        output_annexb.insert(output_annexb.end(),
                             data + pos, data + pos + nal_len);
        pos += nal_len;
    }
    return !output_annexb.empty();
}

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
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(nv12_size), &media_buf))) {
        std::cerr << "[PIPE] MakeNv12Sample: MFCreateMemoryBuffer failed\n";
        return nullptr;
    }

    BYTE* ptr = nullptr; DWORD max_len = 0, cur_len = 0;
    if (FAILED(media_buf->Lock(&ptr, &max_len, &cur_len))) {
        std::cerr << "[PIPE] MakeNv12Sample: Lock failed\n";
        return nullptr;
    }

    BgraToNv12(bgra, ptr);
    media_buf->Unlock();
    media_buf->SetCurrentLength(static_cast<DWORD>(nv12_size));

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) {
        std::cerr << "[PIPE] MakeNv12Sample: MFCreateSample failed\n";
        return nullptr;
    }
    sample->AddBuffer(media_buf.Get());

    // Set timestamp and duration (critical for NVENC)
    // Duration: 1/60fps = 1666667 * 100ns units
    LONGLONG timestamp_100ns = pts_us * 10;
    LONGLONG duration_100ns = fps_ > 0 ? (10'000'000LL / fps_) : 1666667;
    sample->SetSampleTime(timestamp_100ns);
    sample->SetSampleDuration(duration_100ns);

    // Log first 5 samples for diagnostics
    int sample_num = s_process_input_calls.load();
    if (sample_num < 5) {
        std::cout << "[PIPE] MakeNv12Sample: pts=" << pts_us << "us (" << timestamp_100ns
                  << " *100ns), duration=" << duration_100ns
                  << " *100ns, size=" << nv12_size << " (expected " << nv12_size << ")\n";
    }

    return sample;
}

// ── Async event loop (runs on event_thread_) ─────────────────────────────────

void HwEncoder::EventLoop() {
    int need_input_count = 0;
    int have_output_count = 0;
    int other_count = 0;

    while (running_) {
        ComPtr<IMFMediaEvent> event;
        HRESULT hr = event_gen_->GetEvent(0, &event);   // blocking
        if (FAILED(hr)) {
            std::cerr << "[PIPE] EventLoop: GetEvent failed hr=0x" << std::hex << hr << std::dec << "\n";
            break;
        }

        MediaEventType type = MEUnknown;
        event->GetType(&type);

        // Count events and log first few of each type
        if (type == METransformNeedInput) {
            s_need_input_events.fetch_add(1);
            int cnt = ++need_input_count;
            if (cnt <= 5) {
                std::cout << "[PIPE] EventLoop: METransformNeedInput #" << cnt << "\n";
            }

            std::unique_lock<std::mutex> lk(input_mutex_);
            if (!pending_samples_.empty()) {
                auto sample = pending_samples_.front();
                pending_samples_.pop();
                lk.unlock();
                int pi_cnt = s_process_input_calls.fetch_add(1) + 1;
                HRESULT hr_pi = encoder_->ProcessInput(0, sample.Get(), 0);
                if (pi_cnt <= 5) {
                    std::cout << "[PIPE] ProcessInput #" << pi_cnt << " hr=0x" << std::hex << hr_pi << std::dec << "\n";
                }
                if (FAILED(hr_pi) && pi_cnt <= 5) {
                    std::cerr << "[PIPE] ERROR ProcessInput #" << pi_cnt << " failed hr=0x" << std::hex << hr_pi << std::dec << "\n";
                }
            } else {
                needs_input_ = true;
                lk.unlock();
                needs_input_cv_.notify_one();
            }
        } else if (type == METransformHaveOutput) {
            s_have_output_events.fetch_add(1);
            int cnt = ++have_output_count;
            if (cnt <= 5) {
                std::cout << "[PIPE] EventLoop: METransformHaveOutput #" << cnt << "\n";
            }
            DrainOutput();
        } else {
            // Log other event types for diagnostics
            int cnt = ++other_count;
            if (cnt <= 5) {
                std::cout << "[PIPE] EventLoop: other event type=" << type << " (count=" << cnt << ")\n";
            }
        }
    }

    // Log final event counts
    std::cout << "[PIPE] EventLoop exiting: NeedInput=" << need_input_count
              << " HaveOutput=" << have_output_count
              << " Other=" << other_count << "\n";
}

void HwEncoder::DrainOutput() {
    MFT_OUTPUT_DATA_BUFFER out_buf = {};
    DWORD status = 0;

    int po_cnt = s_process_output_calls.fetch_add(1) + 1;
    // Some hardware MFTs allocate their own output samples
    HRESULT hr = encoder_->ProcessOutput(0, 1, &out_buf, &status);

    // Log first 5 calls and their results
    if (po_cnt <= 5) {
        std::cout << "[PIPE] ProcessOutput #" << po_cnt << " hr=0x" << std::hex << hr << std::dec;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) std::cout << " (STREAM_CHANGE)";
        else if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) std::cout << " (NEED_MORE_INPUT)";
        else if (SUCCEEDED(hr)) std::cout << " (OK)";
        std::cout << "\n";
    }

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        // Output type changed — extract SPS/PPS from MF_MT_MPEG_SEQUENCE_HEADER
        // This is where MF encoders store the codec config data
        ComPtr<IMFMediaType> mt;
        if (SUCCEEDED(encoder_->GetOutputCurrentType(0, &mt))) {
            UINT32 blob_size = 0;
            UINT8 blob_buf[1024] = {};  // SPS/PPS should fit in 1KB
            HRESULT hr_blob = mt->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob_buf, sizeof(blob_buf), &blob_size);
            if (SUCCEEDED(hr_blob) && blob_size > 0) {
                // MF stores SPS/PPS in AVCC format - convert to Annex-B
                std::vector<uint8_t> annexb;
                if (ConvertAvccToAnnexB(blob_buf, blob_size, annexb)) {
                    sps_pps_cache_ = std::move(annexb);
                    sps_pps_found_ = true;
                    LogHex("[HwEncoder] SPS/PPS extracted (AVCC->AnnexB): ", sps_pps_cache_.data(), sps_pps_cache_.size());
                }
            }
        }
        return;
    }
    if (FAILED(hr)) {
        if (po_cnt <= 5 && hr != MF_E_TRANSFORM_NEED_MORE_INPUT) {
            std::cerr << "[PIPE] ERROR ProcessOutput #" << po_cnt << " failed hr=0x" << std::hex << hr << std::dec << "\n";
        }
        return;
    }

    if (out_buf.pSample) {
        ComPtr<IMFMediaBuffer> media_buf;
        out_buf.pSample->ConvertToContiguousBuffer(&media_buf);
        out_buf.pSample->Release();

        BYTE* data = nullptr; DWORD len = 0;
        if (FAILED(media_buf->Lock(&data, nullptr, &len))) return;

        std::vector<uint8_t> raw_data(data, data + len);
        media_buf->Unlock();

        // Convert AVCC to Annex-B format (MF outputs AVCC, Android expects Annex-B)
        std::vector<uint8_t> nal_data;
        if (!ConvertAvccToAnnexB(raw_data.data(), raw_data.size(), nal_data)) {
            // If conversion fails, use raw data (might already be Annex-B)
            nal_data = std::move(raw_data);
        }

        // Check for SPS/PPS if not yet found (fallback in case stream change wasn't detected)
        if (!sps_pps_found_) {
            bool has_idr = false;
            if (ParseSpsPps(nal_data.data(), nal_data.size(), sps_pps_cache_, has_idr)) {
                sps_pps_found_ = true;
                LogHex("[HwEncoder] SPS/PPS found inline: ", sps_pps_cache_.data(), sps_pps_cache_.size());
            }
        }

        {
            std::lock_guard<std::mutex> lk(output_mutex_);
            output_queue_.push(std::move(nal_data));
            s_encode_outputs.fetch_add(1);
        }
        output_cv_.notify_one();
    }

    if (out_buf.pEvents) out_buf.pEvents->Release();
}

// ── Public interface ─────────────────────────────────────────────────────────

bool HwEncoder::Initialize(int width, int height, int fps, int bitrate_kbps) {
    width_  = width;
    height_ = height;
    fps_    = fps;
    is_hardware_ = false;
    is_async_    = false;

    // Reset pipeline counters
    s_captured_frames.store(0);
    s_encode_calls.store(0);
    s_encode_outputs.store(0);
    s_need_input_events.store(0);
    s_have_output_events.store(0);
    s_process_input_calls.store(0);
    s_process_output_calls.store(0);

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
        std::cout << "      No hardware H.264 encoder found, trying software MFT...\n";

        // Try software MFT (non-GPL fallback) - omit HARDWARE and ASYNCMFT flags
        // Software H.264 MFT is SYNCHRONOUS - it does NOT fire NeedInput/HaveOutput events
        MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                  MFT_ENUM_FLAG_SORTANDFILTER,  // No ASYNCMFT flag
                  &in_type, &out_type, &activates, &count);

        if (count == 0) {
            std::cerr << "      ERROR: No H.264 encoder available (hardware or software).\n";
            MFShutdown();
            return false;
        }
        is_hardware_ = false;
    } else {
        is_hardware_ = true;
    }

    HRESULT hr = activates[0]->ActivateObject(IID_PPV_ARGS(&encoder_));
    PWSTR name_w = nullptr;
    UINT32 name_len = 0;
    if (SUCCEEDED(activates[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name_w, &name_len))) {
        // Convert wide string to narrow for logging
        char name_a[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, name_w, -1, name_a, sizeof(name_a), nullptr, nullptr);
        std::cout << "      " << (is_hardware_ ? "Hardware" : "Software") << " encoder: " << name_a << "\n";
        CoTaskMemFree(name_w);
    }
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    if (FAILED(hr)) { MFShutdown(); return false; }

    // Detect ASYNC vs SYNC MFT (CRITICAL for correct driving model)
    ComPtr<IMFAttributes> attrs;
    UINT32 is_async_mft = 0;
    encoder_->GetAttributes(&attrs);
    if (attrs) {
        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async_mft);
    }
    is_async_ = (is_async_mft != 0);
    std::cout << "      MFT type: " << (is_async_ ? "ASYNC (event-driven)" : "SYNC (direct loop)") << "\n";

    // For ASYNC MFTs, get the event generator. SYNC MFTs don't have one.
    if (is_async_) {
        hr = encoder_.As(&event_gen_);
        if (FAILED(hr)) {
            std::cerr << "      ERROR: Failed to get IMFMediaEventGenerator from async MFT\n";
            MFShutdown();
            return false;
        }
    } else {
        event_gen_.Reset();  // Ensure no event generator for SYNC MFTs
    }

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
    mt_out->SetUINT32(MF_MT_MPEG2_PROFILE,   100);  // H.264 High = 100
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

    // Configure encoder properties for proper streaming
    ComPtr<ICodecAPI> codec_api;
    if (SUCCEEDED(encoder_.As(&codec_api))) {
        // Force an IDR (keyframe) for the first encoded frame
        VARIANT var;
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        HRESULT hr_force = codec_api->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var);
        if (SUCCEEDED(hr_force)) {
            std::cout << "      CODECAPI_AVEncVideoForceKeyFrame enabled for first frame\n";
        }

        // Set GOP size to 60 frames (1 second at 60fps) for periodic keyframes
        var.vt = VT_UI4;
        var.ulVal = 60;  // GOP size
        HRESULT hr_gop = codec_api->SetValue(&CODECAPI_AVEncMPVGOPSize, &var);
        if (SUCCEEDED(hr_gop)) {
            std::cout << "      CODECAPI_AVEncMPVGOPSize set to 60\n";
        }
    }

    running_ = true;

    // Only start event loop for ASYNC MFTs. SYNC MFTs are driven directly in EncodeFrame.
    if (is_async_) {
        event_thread_ = std::thread(&HwEncoder::EventLoop, this);
        std::cout << "      Event thread started for ASYNC MFT\n";
    } else {
        std::cout << "      SYNC MFT: no event thread (direct ProcessInput/ProcessOutput)\n";
    }

    // ── PRIME ENCODER: produce SPS/PPS at init to break handshake deadlock ──
    // The old x264 encoder produced SPS/PPS at Initialize() (x264_encoder_headers).
    // MF encoders only produce SPS/PPS after encoding the first frame.
    // Session::StreamLoop skips capture while !android_ready_, and android_ready_
    // only becomes true after Android ACKs codec config. Deadlock: no frames → no SPS/PPS.
    // Fix: Prime the encoder with a black frame, extract SPS/PPS, discard output.
    std::cout << "      Priming encoder to extract SPS/PPS at init...\n";
    if (!PrimeEncoder()) {
        std::cerr << "      WARNING: Encoder priming failed - SPS/PPS may not be available immediately\n";
    } else {
        std::cout << "      Encoder primed successfully, SPS/PPS available\n";
    }

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

// ── Encoder priming: feed black frame to extract SPS/PPS at init ─────────────

bool HwEncoder::PrimeEncoder() {
    // Create a black NV12 frame: luma=0x10 (video black), chroma=0x80 (neutral)
    const size_t nv12_size = static_cast<size_t>(width_ * height_ * 3 / 2);
    std::vector<uint8_t> black_nv12(nv12_size, 0);
    // Y plane: 0x10 (video black level for limited range)
    std::fill(black_nv12.begin(), black_nv12.begin() + width_ * height_, static_cast<uint8_t>(0x10));
    // UV plane: 0x80 (neutral chroma)
    std::fill(black_nv12.begin() + width_ * height_, black_nv12.end(), static_cast<uint8_t>(0x80));

    // Create MF sample from black NV12
    ComPtr<IMFMediaBuffer> media_buf;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(nv12_size), &media_buf))) {
        std::cerr << "[Prime] MFCreateMemoryBuffer failed\n";
        return false;
    }

    BYTE* ptr = nullptr;
    if (FAILED(media_buf->Lock(&ptr, nullptr, nullptr))) {
        std::cerr << "[Prime] Lock failed\n";
        return false;
    }
    memcpy(ptr, black_nv12.data(), nv12_size);
    media_buf->Unlock();
    media_buf->SetCurrentLength(static_cast<DWORD>(nv12_size));

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) {
        std::cerr << "[Prime] MFCreateSample failed\n";
        return false;
    }
    sample->AddBuffer(media_buf.Get());

    // Set timestamp (0) and duration for priming frame
    LONGLONG duration_100ns = fps_ > 0 ? (10'000'000LL / fps_) : 1666667;
    sample->SetSampleTime(0);
    sample->SetSampleDuration(duration_100ns);

    // Feed the priming frame
    HRESULT hr = encoder_->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
        std::cerr << "[Prime] ProcessInput failed hr=0x" << std::hex << hr << std::dec << "\n";
        return false;
    }
    std::cout << "[Prime] Black frame submitted, pumping for SPS/PPS...\n";

    // Pump output until SPS/PPS is found or timeout
    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::seconds(2);
    int pump_count = 0;

    while (!sps_pps_found_ && std::chrono::steady_clock::now() - start < timeout) {
        MFT_OUTPUT_DATA_BUFFER out_buf = {};
        DWORD status = 0;
        HRESULT hr_out = encoder_->ProcessOutput(0, 1, &out_buf, &status);
        ++pump_count;

        if (hr_out == MF_E_TRANSFORM_STREAM_CHANGE) {
            // Extract SPS/PPS from MF_MT_MPEG_SEQUENCE_HEADER
            ComPtr<IMFMediaType> mt;
            if (SUCCEEDED(encoder_->GetOutputCurrentType(0, &mt))) {
                UINT32 blob_size = 0;
                UINT8 blob_buf[1024] = {};
                HRESULT hr_blob = mt->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob_buf, sizeof(blob_buf), &blob_size);
                if (SUCCEEDED(hr_blob) && blob_size > 0) {
                    std::vector<uint8_t> annexb;
                    if (ConvertAvccToAnnexB(blob_buf, blob_size, annexb)) {
                        sps_pps_cache_ = std::move(annexb);
                        sps_pps_found_ = true;
                        LogHex("[Prime] SPS/PPS extracted from STREAM_CHANGE: ", sps_pps_cache_.data(), sps_pps_cache_.size());
                    }
                }
            }
            if (out_buf.pSample) out_buf.pSample->Release();
            if (out_buf.pEvents) out_buf.pEvents->Release();
            break;
        }

        if (hr_out == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            // Need more input - but we only have one priming frame
            break;
        }

        if (FAILED(hr_out)) {
            if (pump_count <= 5) {
                std::cerr << "[Prime] ProcessOutput failed hr=0x" << std::hex << hr_out << std::dec << "\n";
            }
            break;
        }

        if (out_buf.pSample) {
            // Process output to check for inline SPS/PPS
            ComPtr<IMFMediaBuffer> buf;
            out_buf.pSample->ConvertToContiguousBuffer(&buf);
            out_buf.pSample->Release();

            BYTE* data = nullptr; DWORD len = 0;
            if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
                std::vector<uint8_t> raw_data(data, data + len);
                buf->Unlock();

                std::vector<uint8_t> nal_data;
                if (!ConvertAvccToAnnexB(raw_data.data(), raw_data.size(), nal_data)) {
                    nal_data = std::move(raw_data);
                }

                // Check for SPS/PPS inline
                if (!sps_pps_found_) {
                    bool has_idr = false;
                    if (ParseSpsPps(nal_data.data(), nal_data.size(), sps_pps_cache_, has_idr)) {
                        sps_pps_found_ = true;
                        LogHex("[Prime] SPS/PPS found inline: ", sps_pps_cache_.data(), sps_pps_cache_.size());
                    }
                }
            }
            // Discard the primed output (do not send)
        }

        if (out_buf.pEvents) out_buf.pEvents->Release();

        // Small delay for ASYNC MFTs to process
        if (!sps_pps_found_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // For ASYNC MFTs, also pump via EventLoop if needed
    if (is_async_ && !sps_pps_found_) {
        // Give EventLoop time to process (it runs on its own thread)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // Check again if SPS/PPS was found via the event loop
        if (sps_pps_found_) {
            std::cout << "[Prime] SPS/PPS found via EventLoop\n";
        }
    }

    // Reset pts_ so first REAL frame starts clean (frame 0)
    pts_ = 0;

    if (sps_pps_found_) {
        std::cout << "[Prime] SUCCESS: SPS/PPS cached (" << sps_pps_cache_.size() << " bytes) after " << pump_count << " pumps\n";
        return true;
    }

    std::cerr << "[Prime] FAILED: SPS/PPS not found after " << pump_count << " pumps\n";
    return false;
}

bool HwEncoder::EncodeFrame(const uint8_t* bgra,
                              std::vector<uint8_t>& nal_out,
                              bool& is_keyframe) {
    int call_num = s_encode_calls.fetch_add(1) + 1;

    // Log first 5 calls
    if (call_num <= 5) {
        std::cout << "[PIPE] EncodeFrame call #" << call_num << " pts=" << pts_ << "\n";
    }

    auto sample = MakeNv12Sample(bgra, pts_++);
    if (!sample) {
        if (call_num <= 5) std::cerr << "[PIPE] MakeNv12Sample failed\n";
        return false;
    }

    // ASYNC MFT: queue sample for EventLoop, wait for output
    // SYNC MFT: drive ProcessInput/ProcessOutput directly
    if (is_async_) {
        // ASYNC path: use event-driven model
        std::unique_lock<std::mutex> lk(input_mutex_);
        if (needs_input_) {
            needs_input_ = false;
            lk.unlock();
            int pi_cnt = s_process_input_calls.fetch_add(1) + 1;
            HRESULT hr = encoder_->ProcessInput(0, sample.Get(), 0);
            if (call_num <= 5) {
                std::cout << "[PIPE] ASYNC ProcessInput #" << pi_cnt << " hr=0x" << std::hex << hr << std::dec << "\n";
            }
        } else {
            pending_samples_.push(sample);
            if (call_num <= 5) {
                std::cout << "[PIPE] ASYNC: sample queued (queue size=" << pending_samples_.size() << ")\n";
            }
        }
    } else {
        // SYNC path: direct ProcessInput call
        int pi_cnt = s_process_input_calls.fetch_add(1) + 1;
        HRESULT hr = encoder_->ProcessInput(0, sample.Get(), 0);
        if (call_num <= 5) {
            std::cout << "[PIPE] SYNC ProcessInput #" << pi_cnt << " hr=0x" << std::hex << hr << std::dec << "\n";
        }
        if (FAILED(hr) && call_num <= 5) {
            std::cerr << "[PIPE] ERROR SYNC ProcessInput failed hr=0x" << std::hex << hr << std::dec << "\n";
        }

        // Immediately drain output (SYNC MFT doesn't emit events)
        // May need multiple calls to drain all output
        for (int drain_attempt = 0; drain_attempt < 5; ++drain_attempt) {
            MFT_OUTPUT_DATA_BUFFER out_buf = {};
            DWORD status = 0;
            int po_cnt = s_process_output_calls.fetch_add(1) + 1;
            HRESULT hr_out = encoder_->ProcessOutput(0, 1, &out_buf, &status);

            if (call_num <= 5 && drain_attempt == 0) {
                std::cout << "[PIPE] SYNC ProcessOutput #" << po_cnt << " drain_attempt=" << drain_attempt
                          << " hr=0x" << std::hex << hr_out << std::dec;
                if (hr_out == MF_E_TRANSFORM_NEED_MORE_INPUT) std::cout << " (NEED_MORE_INPUT)";
                else if (hr_out == MF_E_TRANSFORM_STREAM_CHANGE) std::cout << " (STREAM_CHANGE)";
                else if (SUCCEEDED(hr_out)) std::cout << " (OK, sample=" << (out_buf.pSample ? "yes" : "no") << ")";
                std::cout << "\n";
            }

            if (hr_out == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                break;  // No more output available
            }
            if (FAILED(hr_out)) {
                if (hr_out != MF_E_TRANSFORM_STREAM_CHANGE && call_num <= 5) {
                    std::cerr << "[PIPE] ERROR SYNC ProcessOutput failed hr=0x" << std::hex << hr_out << std::dec << "\n";
                }
                break;
            }

            if (out_buf.pSample) {
                ComPtr<IMFMediaBuffer> media_buf;
                out_buf.pSample->ConvertToContiguousBuffer(&media_buf);
                out_buf.pSample->Release();

                BYTE* data = nullptr; DWORD len = 0;
                if (SUCCEEDED(media_buf->Lock(&data, nullptr, &len))) {
                    std::vector<uint8_t> raw_data(data, data + len);
                    media_buf->Unlock();

                    // Convert AVCC to Annex-B
                    std::vector<uint8_t> nal_data;
                    if (!ConvertAvccToAnnexB(raw_data.data(), raw_data.size(), nal_data)) {
                        nal_data = std::move(raw_data);
                    }

                    // Check for SPS/PPS
                    if (!sps_pps_found_) {
                        bool has_idr = false;
                        if (ParseSpsPps(nal_data.data(), nal_data.size(), sps_pps_cache_, has_idr)) {
                            sps_pps_found_ = true;
                            LogHex("[HwEncoder] SPS/PPS found inline (SYNC): ", sps_pps_cache_.data(), sps_pps_cache_.size());
                        }
                    }

                    // Add to output queue
                    {
                        std::lock_guard<std::mutex> lk(output_mutex_);
                        output_queue_.push(std::move(nal_data));
                        s_encode_outputs.fetch_add(1);
                    }
                    output_cv_.notify_one();

                    if (call_num <= 5) {
                        std::cout << "[PIPE] SYNC output queued (queue size=" << output_queue_.size() << ")\n";
                    }
                }
            }

            if (out_buf.pEvents) out_buf.pEvents->Release();
        }
    }

    // Wait up to one frame time for output (ASYNC path, or if SYNC just queued)
    std::unique_lock<std::mutex> out_lk(output_mutex_);
    output_cv_.wait_for(out_lk, std::chrono::milliseconds(33),
                        [this] { return !output_queue_.empty(); });

    if (output_queue_.empty()) {
        nal_out.clear();
        is_keyframe = false;
        if (call_num <= 5) {
            std::cout << "[PIPE] EncodeFrame #" << call_num << " returned EMPTY (pipelining)\n";
        }
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

    // Defense-in-depth: prepend SPS/PPS in-band to first keyframe
    // This ensures Android can configure even if csd-0 timing is off
    if (is_keyframe && !first_keyframe_done_ && sps_pps_found_ && !sps_pps_cache_.empty()) {
        std::vector<uint8_t> with_spspps;
        with_spspps.reserve(sps_pps_cache_.size() + nal_out.size());
        with_spspps.insert(with_spspps.end(), sps_pps_cache_.begin(), sps_pps_cache_.end());
        with_spspps.insert(with_spspps.end(), nal_out.begin(), nal_out.end());
        nal_out = std::move(with_spspps);
        first_keyframe_done_ = true;
        if (call_num <= 5) {
            std::cout << "[PIPE] First keyframe: prepended SPS/PPS in-band ("
                      << sps_pps_cache_.size() << " + original bytes)\n";
        }
    }

    // Diagnostic logging: config packet and first 5 frames
    if (call_num <= 5) {
        std::cout << "[PIPE] EncodeFrame #" << call_num
                  << " output size=" << nal_out.size()
                  << " keyframe=" << (is_keyframe ? "YES" : "no") << "\n";
    }

    // Log config packet once when available (only for first few frames)
    static std::atomic<bool> s_config_logged{false};
    if (!s_config_logged.load() && sps_pps_found_ && call_num <= 10) {
        s_config_logged.store(true);
        LogHex("[HwEncoder] Config packet (SPS/PPS): ", sps_pps_cache_.data(), sps_pps_cache_.size());
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
    is_hardware_ = false;
    first_keyframe_done_ = false;

    // Note: Pipeline counters (s_encode_calls, s_process_input_calls, etc.)
    // are reset in Initialize(), not here, to preserve diagnostic data between sessions.
}
