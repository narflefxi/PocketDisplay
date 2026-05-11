#pragma once
#include <cstdint>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

// Hardware H.264 encoder using Windows Media Foundation.
// Transparently uses NVENC, Intel Quick Sync, or AMD VCE depending on the GPU.
// Same interface as Encoder so main.cpp can switch between them.
class HwEncoder {
public:
    HwEncoder()  = default;
    ~HwEncoder() { Close(); }

    // Returns false if no hardware H.264 encoder is available on this system.
    bool Initialize(int width, int height, int fps = 60, int bitrate_kbps = 8000);

    // Returns SPS/PPS in Annex-B format.
    // May return false on the first call if the encoder hasn't produced a
    // keyframe yet; the caller should skip FLAG_CODEC_CONFIG in that case.
    bool GetConfigPacket(std::vector<uint8_t>& sps_pps_out);

    // Encode one BGRA frame. nal_out may be empty while the encoder pipelines.
    bool EncodeFrame(const uint8_t* bgra, std::vector<uint8_t>& nal_out,
                     bool& is_keyframe);

    void Close();

private:
    void EventLoop();
    void DrainOutput();
    void BgraToNv12(const uint8_t* bgra, uint8_t* nv12) const;
    Microsoft::WRL::ComPtr<IMFSample> MakeNv12Sample(const uint8_t* bgra, int64_t pts_us);

    Microsoft::WRL::ComPtr<IMFTransform>          encoder_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> event_gen_;

    int     width_  = 0;
    int     height_ = 0;
    int64_t pts_    = 0;

    // Thread-safe output queue
    std::mutex              output_mutex_;
    std::queue<std::vector<uint8_t>> output_queue_;
    std::condition_variable output_cv_;

    // Input slot signalling from event loop → EncodeFrame
    std::mutex              input_mutex_;
    bool                    needs_input_ = false;
    std::condition_variable needs_input_cv_;
    std::queue<Microsoft::WRL::ComPtr<IMFSample>> pending_samples_;

    std::atomic<bool> running_{false};
    std::thread       event_thread_;

    std::vector<uint8_t> sps_pps_cache_;
    bool                 sps_pps_found_ = false;
};
