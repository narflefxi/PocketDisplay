#include "ScreenCapture.h"
#include "Encoder.h"
#include "UdpStreamer.h"
#include "TouchReceiver.h"
#include "Protocol.h"

#include <windows.h>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>

static std::atomic<bool> g_running{true};

static void SignalHandler(int) { g_running = false; }

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: PocketDisplay <android_ip> [port] [bitrate_kbps] [fps]\n";
        std::cerr << "  Example: PocketDisplay 192.168.1.100\n";
        std::cerr << "  Example: PocketDisplay 192.168.1.100 7777 8000 60\n";
        return 1;
    }

    const std::string target_ip    = argv[1];
    const uint16_t    port         = argc > 2 ? static_cast<uint16_t>(std::stoi(argv[2])) : pocketdisplay::DEFAULT_PORT;
    const int         bitrate_kbps = argc > 3 ? std::stoi(argv[3]) : 8000;
    const int         target_fps   = argc > 4 ? std::stoi(argv[4]) : 60;

    std::signal(SIGINT, SignalHandler);

    ScreenCapture capture;
    Encoder       encoder;
    UdpStreamer   streamer;
    TouchReceiver touch;

    std::cout << "[1/3] Initializing screen capture...\n";
    if (!capture.Initialize()) {
        std::cerr << "ERROR: DXGI Desktop Duplication failed.\n";
        std::cerr << "       Ensure you are running on the primary GPU output.\n";
        return 1;
    }
    std::cout << "      Display: " << capture.GetWidth() << "x" << capture.GetHeight() << "\n";

    std::cout << "[2/3] Initializing H.264 encoder (" << bitrate_kbps << " kbps, " << target_fps << " fps)...\n";
    if (!encoder.Initialize(capture.GetWidth(), capture.GetHeight(), target_fps, bitrate_kbps)) {
        std::cerr << "ERROR: x264 encoder init failed.\n";
        return 1;
    }

    std::cout << "[3/3] Opening UDP socket -> " << target_ip << ":" << port << "\n";
    if (!streamer.Initialize(target_ip, port)) {
        std::cerr << "ERROR: Winsock init failed.\n";
        return 1;
    }

    if (touch.Start(7778)) {
        std::cout << "      Touch input listening on UDP :7778\n";
    } else {
        std::cerr << "WARNING: Touch receiver failed to start (non-fatal)\n";
    }

    // Send SPS/PPS so Android can configure the decoder before the first frame arrives
    {
        std::vector<uint8_t> sps_pps;
        if (encoder.GetConfigPacket(sps_pps)) {
            streamer.SendFrame(sps_pps.data(), sps_pps.size(),
                               /*frame_id=*/0, pocketdisplay::FLAG_CODEC_CONFIG);
            std::cout << "Sent codec config (" << sps_pps.size() << " bytes)\n";
        }
    }

    std::cout << "Streaming — press Ctrl+C to stop.\n\n";

    std::vector<uint8_t> bgra_buf;
    std::vector<uint8_t> nal_buf;
    uint32_t frame_id    = 1;
    uint64_t frames_sent = 0;
    size_t   bytes_sent  = 0;

    const auto   frame_interval = std::chrono::microseconds(1'000'000 / target_fps);
    auto         next_frame     = std::chrono::steady_clock::now();
    const auto   start_time     = next_frame;

    while (g_running) {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_frame) {
            std::this_thread::sleep_for(next_frame - now);
        }
        next_frame += frame_interval;

        int w = 0, h = 0;
        if (!capture.CaptureFrame(bgra_buf, w, h)) continue;

        bool is_keyframe = false;
        if (!encoder.EncodeFrame(bgra_buf.data(), nal_buf, is_keyframe)) continue;
        if (nal_buf.empty()) continue;

        const uint8_t flags = is_keyframe ? pocketdisplay::FLAG_KEYFRAME : pocketdisplay::FLAG_NONE;
        streamer.SendFrame(nal_buf.data(), nal_buf.size(), frame_id++, flags);

        ++frames_sent;
        bytes_sent += nal_buf.size();

        // Print stats every 5 seconds
        const double elapsed_s = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_time).count();
        if (elapsed_s >= 1.0 && frames_sent % static_cast<uint64_t>(target_fps * 5) == 0) {
            const double fps     = frames_sent / elapsed_s;
            const double kbps    = (bytes_sent * 8.0 / 1000.0) / elapsed_s;
            std::cout << "FPS: " << static_cast<int>(fps)
                      << "  Bitrate: " << static_cast<int>(kbps) << " kbps"
                      << "  Frame: " << nal_buf.size() << " bytes\n";
        }
    }

    std::cout << "\nShutting down...\n";
    encoder.Close();
    streamer.Close();
    capture.Release();
    return 0;
}
