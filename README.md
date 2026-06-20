# PocketDisplay

Use your Android phone as a **wireless or USB secondary display** for Windows — with touch input, cursor overlay, and Mirror/Extended desktop modes.

---

## Features

### Streaming
- **WiFi mode** — H.264 video over UDP with automatic host discovery (multicast :7779). Low-latency, no pairing required.
- **USB mode** — H.264 video over TCP via ADB reverse tunnel (`adb reverse tcp:7777 tcp:7777`). More stable than WiFi; zero network configuration.
- **Auto-detection** — on launch with no arguments, the Windows app detects a connected USB device first, then falls back to WiFi auto-discovery.
- **Hardware encoding** — `--hw` flag enables NVENC / Intel Quick Sync / AMD VCE via Windows Media Foundation, with automatic x264 software fallback.
- **Configurable** — bitrate, FPS, port, and monitor selection all adjustable via CLI flags.

### Display Modes
- **Mirror** — replicates the primary monitor (or any selected monitor via `--monitor=N`).
- **Extended** — captures a virtual/extended DXGI output (`--extend`), making the phone a genuine second screen in Windows Display Settings.
- Mode is selected on the Android side at connect time; Windows acts on the selection before streaming begins.

### Input
- **Touch forwarding** — touch events are normalized and injected as Windows `POINTER_INPUT` (multi-touch capable). Coordinates correctly account for the 180° render rotation.
- **Keyboard capture** — Android soft keyboard input is forwarded to Windows as Unicode characters and virtual key events.
- **Cursor overlay** — a Windows-accurate arrow cursor (with hotspot at tip) is drawn on the Android screen at the real Windows cursor position, updating on every cursor move. Cursor shape type is transmitted alongside position.
- **Extended-mode touch mapping** — when in Extended mode, touch coordinates are mapped to the correct virtual monitor rectangle in Windows desktop space.

### Apps
- **Windows dashboard** — a native Win32 GUI (860×560) with a branded dark sidebar, live stats (FPS, bitrate, resolution, mode, connection status), and multi-page navigation. Launches automatically alongside the streaming server.
- **Android dashboard** — Material Design UI with a dark sidebar, mode toggle (WiFi / USB), live status card, and a HUD overlay that appears during streaming.

---

## Architecture

```
Windows (C++)                           Android (Kotlin)
──────────────────────────────────────  ──────────────────────────────────────
DXGI Desktop Duplication                [WiFi] UDP :7777 → reassemble → decode
  → BGRA frame                          [USB]  TCP 127.0.0.1:7777 (adb reverse)
  → x264 (software) or MF HW encoder   MediaCodec H.264 decoder
  → [WiFi] UDP fragmented packets       TextureView (rotated 180°)
  → [USB]  TCP length-prefixed msgs     CursorOverlayView (arrow, hotspot 0,0)

Touch/Keyboard (Android → Windows)     Auto-Discovery
  TouchSender → UDP/TCP :7778           DiscoveryClient ← multicast :7779
  TouchReceiver → POINTER_INPUT API     RunDiscovery → sends POCKETDISPLAY_HOST
```

### WiFi UDP Protocol (`Protocol.h`)
Custom binary packet format, magic `PDSM`, safe MTU payload of 1380 bytes:

| Field | Size | Description |
|---|---|---|
| `magic` | 4 B | `'P','D','S','M'` |
| `frame_id` | 4 B | Frame sequence number (BE) |
| `packet_idx` | 2 B | Packet index within frame (BE) |
| `total_packets` | 2 B | Total packets for this frame (BE) |
| `frame_size` | 4 B | Full assembled frame size (BE) |
| `flags` | 1 B | `CODEC_CONFIG`, `KEYFRAME`, `STREAM_INFO`, `CURSOR_POS` |

### USB TCP Protocol (`TcpVideoServer`)
Length-prefixed messages, big-endian:

```
[4-byte uint32 length][1-byte type][payload]
```

| Type | Payload | Description |
|---|---|---|
| `0` | H.264 Annex-B | Video access unit |
| `1` | SPS + PPS | Codec config |
| `2` | w, h (uint32 BE) + flags | Stream info / Extended flag |
| `3` | nx, ny (float BE) + cursor_type | Cursor position |

Default port: **7777**. Touch port: **7778**. Discovery port: **7779**.  
Default bitrate: **8000 kbps**. Default FPS: **60**.

---

## Build & Run

### Windows Prerequisites

| Tool | Notes |
|---|---|
| Visual Studio 2022 | C++ desktop workload required |
| CMake ≥ 3.20 | [cmake.org](https://cmake.org) |
| vcpkg | [github.com/microsoft/vcpkg](https://github.com/microsoft/vcpkg) |
| ADB (Android Platform Tools) | Required for USB mode |

### Windows Build

```powershell
# 1. Install x264 via vcpkg
vcpkg install x264:x64-windows

# 2. Configure
cmake -B windows/build -S windows `
  -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows

# 3. Build
cmake --build windows/build --config Release
```

### Windows Usage

```powershell
# Auto-detect (USB if plugged in, otherwise WiFi discovery)
.\windows\build\Release\PocketDisplay.exe

# WiFi — direct IP
.\windows\build\Release\PocketDisplay.exe 192.168.1.100

# WiFi — custom bitrate and FPS
.\windows\build\Release\PocketDisplay.exe 192.168.1.100 --bitrate 12000 --fps 30

# USB mode
.\windows\build\Release\PocketDisplay.exe --usb

# USB + Extended display + hardware encoder
.\windows\build\Release\PocketDisplay.exe --usb --extend --hw

# Capture a specific monitor (1-based, matches Windows Display Settings order)
.\windows\build\Release\PocketDisplay.exe --monitor=2
```

### Android Prerequisites

- Android Studio Hedgehog (2023.1) or newer
- Android SDK API ≥ 34
- Physical device with API ≥ 26 (USB debugging enabled for USB mode)

### Android Build & Run

1. Open the `android/` folder in Android Studio.
2. Let Gradle sync complete.
3. Connect your device and click **Run ▶**.
4. Grant network and USB permissions if prompted.
5. The app auto-connects once Windows is running — no manual IP entry needed in WiFi auto-discovery mode.

### USB Mode Setup

```bash
# Enable ADB reverse tunnels (run once per connection)
adb reverse tcp:7777 tcp:7777   # video
adb reverse tcp:7778 tcp:7778   # touch/keyboard
```

Then launch `PocketDisplay.exe --usb` on Windows and open the app on Android.

---

## Project Structure

```
PocketDisplay/
├── assets/                          # Brand source assets
│   ├── logo-primary.png             # Horizontal logo (sidebar/header)
│   ├── logo-icon.png                # Square icon (app icon, small spaces)
│   ├── Anton-Regular.ttf            # Display font (Anton 400)
│   ├── SpaceGrotesk-Medium.ttf      # Body font (Space Grotesk 500)
│   └── SpaceGrotesk-Bold.ttf        # Body font bold (Space Grotesk 700)
│
├── tools/
│   └── prepare_assets.py            # Generates ICO, mipmap PNGs, copies fonts
│
├── windows/
│   ├── CMakeLists.txt
│   ├── vcpkg.json
│   ├── resources/
│   │   ├── resource.h               # IDI_APPICON definition
│   │   ├── resource.rc              # Embeds icon.ico
│   │   └── icon.ico                 # Multi-size app icon (16–256 px)
│   └── src/
│       ├── main.cpp                 # Entry point, CLI parsing, streaming loop
│       ├── GuiApp.{h,cpp}           # Win32 dashboard window
│       ├── ScreenCapture.{h,cpp}    # DXGI Desktop Duplication
│       ├── Encoder.{h,cpp}          # x264 software H.264 encoder
│       ├── HwEncoder.{h,cpp}        # MF hardware encoder (NVENC/QSV/VCE)
│       ├── UdpStreamer.{h,cpp}      # WiFi UDP fragmented send
│       ├── TcpVideoServer.{h,cpp}   # USB TCP video server
│       ├── TcpStreamer.{h,cpp}      # USB TCP client-side helper
│       ├── TouchReceiver.{h,cpp}    # Touch + keyboard input receiver
│       ├── AdbUsbSetup.{h,cpp}      # USB device detection via ADB
│       └── Protocol.h               # WiFi UDP packet format
│
└── android/
    └── app/src/main/
        ├── java/com/pocketdisplay/app/
        │   ├── MainActivity.kt          # UI lifecycle, mode selection, HUD
        │   ├── StreamReceiver.kt        # WiFi UDP receive + frame reassembly
        │   ├── TcpStreamReceiver.kt     # USB TCP video receive + framing
        │   ├── VideoDecoder.kt          # MediaCodec H.264 decoder
        │   ├── TouchSender.kt           # Touch + keyboard event sender
        │   ├── TcpTouchSender.kt        # USB TCP touch channel
        │   ├── CursorOverlayView.kt     # Windows cursor overlay (arrow + hotspot)
        │   └── DiscoveryClient.kt       # WiFi auto-discovery (multicast UDP :7779)
        └── res/
            ├── layout/activity_main.xml # Main UI layout
            ├── values/colors.xml        # Brand color palette
            ├── values/themes.xml        # App theme + typography
            └── font/                    # Anton, Space Grotesk
```

---

## Tuning

| Goal | How |
|---|---|
| Lower latency | Use USB mode; reduce bitrate; use 5 GHz WiFi |
| Better quality | `--bitrate 15000` or higher |
| Lower CPU usage | `--hw` (NVENC / Quick Sync / VCE) |
| Extend desktop | `--extend` or `--monitor=N` |
| Higher framerate | `--fps 60` (default) or higher if encoder keeps up |

---

## Known Limitations

- **DRM-protected video (Netflix, Disney+, Prime Video, etc.) shows as black on the phone.** Windows blocks screen capture of DRM/HDCP-protected content at the OS level (the Protected Media Path replaces the captured region with pure black). This is an OS restriction that affects *all* screen-capture and second-display tools — it is not a PocketDisplay bug. Non-DRM content (YouTube, local video, normal apps) works normally. Audio continues to play from the PC speakers. The app shows a brief informational hint when sustained black is detected so users aren't left wondering if the connection dropped.

---

## Roadmap

- [ ] Adaptive bitrate based on measured round-trip time
- [ ] Audio forwarding (Windows → Android)
- [ ] Portrait / landscape auto-rotation without 180° workaround
- [ ] Resolution scaling / aspect-ratio letterboxing options
- [ ] Multi-monitor extended display (beyond the first virtual output)
- [ ] Android TV / tablet optimised layout
- [ ] Installer / auto-start on Windows boot
