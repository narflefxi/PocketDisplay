# PocketDisplay — AI Assistant Context

## Project Overview
Windows-to-Android screen sharing app (like Super Display).
- Windows app: C++ (DXGI capture, x264/NVENC encode, Win32 GUI)
- Android app: Kotlin (MediaCodec decode, Material Design 3)
- Protocol: Custom binary TCP (USB) & UDP (WiFi)

## Tech Stack
- Windows: C++, DXGI, x264, Win32, CMake
- Android: Kotlin, MediaCodec, Material Design 3
- Ports: 7777 (video), 7778 (touch/ACK)

## Build Commands
Windows:
cmake --build windows/build --config Release

Android:
cd android
$env:JAVA_HOME = "C:\Program Files\Android\Android Studio\jbr"
.\gradlew assembleDebug
adb install -r app\build\outputs\apk\debug\app-debug.apk

## Key Files
- windows/src/main.cpp — main streaming loop, one-click mode
- windows/src/GuiApp.cpp — Win32 GUI dashboard
- windows/src/TcpVideoServer.cpp — USB video server
- windows/src/TouchReceiver.cpp — touch/ACK receiver
- android/.../TcpStreamReceiver.kt — USB video client
- android/.../TcpTouchSender.kt — touch/ACK sender (USB)
- android/.../MainActivity.kt — main UI & connection logic
- android/.../VideoDecoder.kt — MediaCodec H.264 decoder

## Brand
- Primary: #FF4500 (orange)
- Dark bg: #0B0B0D
- Font: Space Grotesk
- Logo: Orange P mascot with eyes

## Current Status
- WiFi & USB streaming: working
- Mirror & Extended mode: working
- Touch/keyboard input: working
- One-click GUI launch: working
- Auto-reconnect: working

## Known Issues (Open)
- #20: First-time setup requires too many manual steps (bug)
- #19: Auto-switch between USB and Wi-Fi connection modes based on USB detection (enhancement)
- #8: Add custom resolution selection for streaming (enhancement)
- #5: Connection not seamless on startup (bug)
- #2: Find better cursor type for Android (enhancement)
- #1: Cursor position mismatch on Android (touch vs mouse) (bug)

## Recently Fixed
- #18: WiFi black screen on reconnect (android_ready not reset) ✅
- #17: Display ghosted/soft — sharper output ✅
- #16: Android-first USB connection deadlock ✅
- #15: USB reconnect works consistently ✅
- #14: Black screen on re-launch ✅
- #13: Add About screen to Android app ✅
- #12: Remove Quick Actions (Keyboard & Mouse) from Android Dashboard ✅
- #11: Windows taskbar icon ✅
- #10: Android app icon (black bg + centered) ✅
- #9: One-click launch from GUI ✅
- #7: Video blurry/ghosting ✅

## Connection Flow (USB)
1. Windows starts → EarlyAdbReverse opens ports 7777/7778
2. Android connects to 7777 → sends mode (mirror/extend)
3. Windows WaitForMode() returns
4. resend_thread immediately sends stream_info + codec_config (no gate)
5. Android TcpTouchSender connects to 7778 (retries until Windows opens it)
6. Android configures decoder → sends ACK (retries every 1s until first frame)
7. android_ready = true → video frames flow
   Note: TcpTouchSender connect thread is persistent — reconnects automatically
   if socket dies (e.g. Windows restart). rawSend nulls tcpSocket on failure.

## Important Rules
- Never use system() or _popen() for adb — causes cmd popup
- Use CreateProcess() with CREATE_NO_WINDOW for silent adb calls
- Extended mode uses the bundled VirtualDrivers Virtual Display Driver at windows/drivers/virtual-display; install is attempted only when extended mode is selected
- Always test: Windows-first, Android-first, AND reconnect scenarios
- Do not break existing working flows when fixing new issues
