# PocketDisplay

Low-latency Windows → Android screen sharing (Phase 1).

## Architecture

```
Windows (C++)                        Android (Kotlin)
─────────────────────────────────    ──────────────────────────────
DXGI Desktop Duplication             DatagramSocket :7777
  → BGRA frame                         → packet reassembly
  → BGRA→I420 (software)             MediaCodec (H.264 decoder)
  → x264 H.264 encode                  → SurfaceView render
  → UDP fragmented packets
```

Protocol: custom binary over UDP (see `windows/src/Protocol.h`).  
Default port: **7777**. Default bitrate: **8000 kbps**. Default FPS: **60**.

---

## Windows Build

### Prerequisites

| Tool | Where |
|------|-------|
| Visual Studio 2022 (C++ workload) | visualstudio.microsoft.com |
| CMake ≥ 3.20 | cmake.org |
| vcpkg | github.com/microsoft/vcpkg |

### Steps

```powershell
# 1. Install x264 via vcpkg
vcpkg install x264:x64-windows

# 2. Configure (point CMAKE_TOOLCHAIN_FILE at your vcpkg install)
cmake -B build -S windows `
  -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows

# 3. Build
cmake --build build --config Release

# 4. Run (replace with your Android device's IP)
.\build\Release\PocketDisplay.exe 192.168.1.100
# Optional: PocketDisplay.exe <ip> <port> <bitrate_kbps> <fps>
```

---

## Android Build

### Prerequisites

- Android Studio Hedgehog (2023.1) or newer
- Android SDK 34 + NDK (NDK not required for Phase 1)
- Physical device or emulator with API ≥ 26

### Steps

1. Open `android/` in Android Studio ("Open existing project").
2. Android Studio will download Gradle and sync automatically.
3. Connect your device and click **Run**.
4. Grant network permissions if prompted.
5. Tap **Start Listening** — the app shows the device IP.
6. Enter that IP on the Windows side and run `PocketDisplay.exe`.

---

## Tuning Tips

| Goal | Change |
|------|--------|
| Lower latency | Reduce `bitrate_kbps`, use Wi-Fi 5 GHz |
| Better quality | Increase `bitrate_kbps` (try 15000–20000) |
| Lower CPU on Windows | Phase 2: switch to NVENC/AMF hardware encode |
| Fit phone screen | Phase 2: add resolution scaling before encode |

---

## Project Structure

```
PocketDisplay/
├── windows/
│   ├── CMakeLists.txt
│   ├── vcpkg.json
│   └── src/
│       ├── Protocol.h        # shared packet format
│       ├── ScreenCapture.{h,cpp}   # DXGI Desktop Duplication
│       ├── Encoder.{h,cpp}         # x264 H.264 encode
│       ├── UdpStreamer.{h,cpp}     # Winsock UDP send
│       └── main.cpp
└── android/
    └── app/src/main/
        ├── java/com/pocketdisplay/app/
        │   ├── MainActivity.kt     # UI + surface lifecycle
        │   ├── StreamReceiver.kt   # UDP receive + frame reassembly
        │   └── VideoDecoder.kt     # MediaCodec H.264 decode
        └── res/layout/activity_main.xml
```

## Phase 2 Roadmap

- [ ] NVENC / AMF hardware encoding (replace x264)
- [ ] Resolution scaling / aspect-ratio letterboxing
- [ ] Touch input forwarding (Android → Windows)
- [ ] Adaptive bitrate based on measured RTT
- [ ] USB/ADB tunnel mode (bypass Wi-Fi for ultra-low latency)
