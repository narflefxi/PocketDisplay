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
- #16: First USB connection fails when Android launches before Windows
  Root cause: adb reverse fakes successful TCP connect before Windows 
  opens port 7778. TcpTouchSender treats phantom success as real, exits 
  retry loop. touch_socket_ready never set. resend_thread blocks forever.
  Latest attempt: probe write in TcpTouchSender (1a78b74) — still fails.
  
- #17: Display ghosted/soft — needs sharper output
- #18: WiFi black screen after connection
- #12: Remove Quick Actions from Android Dashboard
- #13: Add About screen to Android app
- #8: Custom resolution selection
- #5: Connection not seamless on startup
- #4: Cursor type issue on Android
- #1: Cursor position mismatch

## Recently Fixed
- #14: Black screen on re-launch ✅
- #15: USB reconnect works consistently ✅
- #10: Android app icon (black bg + centered) ✅
- #11: Windows taskbar icon ✅
- #9: One-click launch from GUI ✅
- #7: Video blurry/ghosting ✅

## Connection Flow (USB)
1. Windows starts → EarlyAdbReverse opens ports 7777/7778
2. Android connects to 7777 → sends mode (mirror/extend)
3. Windows WaitForMode() returns
4. resend_thread waits for touch_socket_ready
5. Android TcpTouchSender connects to 7778
6. touch_socket_ready = true → resend_thread sends stream_info + codec_config
7. Android configures decoder → sends ACK
8. android_ready = true → video frames flow

## Important Rules
- Never use system() or _popen() for adb — causes cmd popup
- Use CreateProcess() with CREATE_NO_WINDOW for silent adb calls
- Always test: Windows-first, Android-first, AND reconnect scenarios
- Do not break existing working flows when fixing new issues
