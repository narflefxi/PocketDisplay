# PocketDisplay — AI Assistant Context

## Project Overview
Windows-to-Android screen sharing app (like Super Display).
- Windows app: C++ (DXGI capture, x264/NVENC encode, Win32 GUI)
- Android app: Kotlin (MediaCodec decode, Material Design 3)
- Protocol: Custom binary TCP for all video and touch (USB and WiFi)

## Tech Stack
- Windows: C++, DXGI, x264, Win32, CMake
- Android: Kotlin, MediaCodec, Material Design 3
- Ports: 7777 (video/HELLO), 7778 (touch/ACK), 7779 (UDP discovery — announcement only)

## Build Commands
Windows:
cmake --build windows/build --config Release

Android:
cd android
$env:JAVA_HOME = "C:\Program Files\Android\Android Studio\jbr"
.\gradlew assembleDebug
adb install -r app\build\outputs\apk\debug\app-debug.apk

## Key Files
- windows/src/main.cpp — session manager, discovery broadcaster, HELLO server, one-click mode
- windows/src/Session.h/.cpp — owns one streaming session (capture→encode→send); torn down on disconnect
- windows/src/GuiApp.cpp — Win32 GUI dashboard
- windows/src/TcpVideoServer.cpp — always-on HELLO listener; fires SetHelloCallback on valid HELLO
- windows/src/TouchReceiver.cpp — touch/ACK receiver
- windows/src/AdbUsbSetup.cpp — adb setup; GetUsbSerial() skips wireless serials (ip:port)
- android/.../ConnectionManager.kt — connection state machine (USB/WiFi lifecycle, mode persistence)
- android/.../TcpStreamReceiver.kt — TCP video client (host param; works for both USB and WiFi)
- android/.../TouchSender.kt — TCP-only touch/ACK sender
- android/.../MainActivity.kt — UI only; delegates all connection logic to ConnectionManager
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
- Auto-reconnect: working (session-based; no process restart required)
- Mode switching without restart: working (new HELLO tears down old Session, starts new one)
- Wireless adb + USB adb coexist: working (adb reverse uses -s <serial> to avoid multi-device error)
- WiFi→USB hot-switch: working (Phase 4b — always-on USB monitor + Android probe retry)

## Licensing
The app is intended for commercial sale. All bundled assets and dependencies must use permissive licenses (no GPL/copyleft, no CC-BY-SA).
- **Cursor assets**: CC0 — safe.
- **x264**: GPL-licensed — Now OPTIONAL (CMake option `POCKETDISPLAY_ENABLE_X264`, default OFF). Media Foundation H.264 encoder (hardware → software MFT fallback) is the default non-GPL encoder for commercial builds.

## Known Issues / Deferred
- **Resize cursor detection fails in custom-cursor apps** (e.g. Claude Desktop) — Windows-side cursor-type detection only matches standard `IDC_SIZE*` handles. Deferred.
- **VDD extended display (#20)** — true borderless extended display. Deferred.
- **Housekeeping**: consolidate `PROJECT_CONTEXT.md` into `CLAUDE.md`; delete merged branches.

## Recently Fixed
- **Media Foundation black screen fix — SYNC vs ASYNC MFT** ✅
  - **Root cause**: The code assumed all MFTs are ASYNC (event-driven), but the **software H.264 MFT is SYNCHRONOUS** and does NOT fire `METransformNeedInput`/`METransformHaveOutput` events. The encoder was being "driven" incorrectly, resulting in zero output frames.
  - **Fix**: Detect `MF_TRANSFORM_ASYNC` on the MFT during initialization:
    - ASYNC MFTs (NVENC): Use event-driven model with `EventLoop` thread
    - SYNC MFTs (software): Drive directly with `ProcessInput` followed by `ProcessOutput` drain loop in `EncodeFrame`
  - **Timestamp fix**: Set both `SetSampleTime` AND `SetSampleDuration` in `MakeNv12Sample` (NVENC requires valid timestamps). Duration = 1/fps in 100ns units.
  - **Pipeline instrumentation**: Added `[PIPE]` prefixed logging throughout:
    - Session capture loop: `[PIPE] captured frame N`, `[PIPE] calling EncodeFrame N`, `[PIPE] EncodeFrame returned nal=<bytes>`, `[PIPE] sent N bytes to socket`
    - HwEncoder: logs MFT type (ASYNC/SYNC), event counts, ProcessInput/ProcessOutput HRESULTs
  - Changed files: `windows/src/HwEncoder.cpp`, `windows/src/HwEncoder.h`, `windows/src/Session.cpp`.
- **Media Foundation bitstream fix (black screen issue)** ✅
  - **Root cause**: Media Foundation H.264 encoder outputs **AVCC format** (4-byte length-prefixed NALs), but Android MediaCodec expects **Annex-B format** (00 00 00 01 start codes).
  - **SPS/PPS extraction**: MF stores codec config in `MF_MT_MPEG_SEQUENCE_HEADER` attribute (after first `MF_E_TRANSFORM_STREAM_CHANGE`), NOT inline like x264. The code now reads this attribute and converts AVCC to Annex-B.
  - **NAL format conversion**: Added `ConvertAvccToAnnexB()` helper to convert MF's AVCC output to Annex-B format that Android expects.
  - **Keyframe/IDR**: Set `CODECAPI_AVEncVideoForceKeyFrame` for first frame and `CODECAPI_AVEncMPVGOPSize=60` for periodic keyframes (1 second at 60fps).
  - **Diagnostic logging**: Added Windows-side logging (config packet hex, first 5 frames size/keyframe status) and Android-side logging (csd-0 hex, configure result, decoded frame count) for visibility.
  - Changed files: `windows/src/HwEncoder.cpp`, `android/app/src/main/java/com/pocketdisplay/app/VideoDecoder.kt`.
- x264 GPL dependency removed ✅
  - Gated x264 behind `POCKETDISPLAY_ENABLE_X264` CMake option (default OFF) for commercial builds.
  - Media Foundation H.264 encoder is now default with hardware → software MFT fallback.
  - Updated `Session::Config::hw_enc` default to `true` (MF encoder enabled by default).
  - Added `--sw` CLI flag to force software encoding; removed `--hw` flag (now default).
- PR #24: Touch/cursor accuracy + Windows-style cursor shapes ✅
  - Touch lands exactly at touch point (toNormalized inversion fix, feature/cursor-fixes).
  - 11 Windows-style cursor shapes from CC0 SVG assets converted to VectorDrawables with per-type fractional hotspots in CursorOverlayView. Arrow hotspot at tip; hand hotspot at index fingertip (hx=0.397, hy=0.076); resize cursors centered.
  - Hand cursor regenerated as solid silhouette (fillType nonZero). Resize_h and resize_v regenerated from new dedicated SVGs (resize_v bakes the SVG root rotate(90) into a group android:rotation).
- PR #23: Extended-mode cursor hide + letterbox-fit + touch orientation + nav/About ✅
  - Cursor overlay hides when PC cursor leaves the extended display region (sentinel type=0xFF).
  - Letterbox-fit scaling with correct touch mapping; nav bar cleanup; About page.
- Phase 4f: Disconnect button regression fix ✅
  - Root cause (regression from 4e): stopCurrentSession() always triggered an immediate tryConnect() (added in 4e for fast involuntary reconnect). Since both the Disconnect button and internal hot-switch paths called stopCurrentSession(), manual disconnect instantly auto-reconnected.
  - Fix: introduced @Volatile userDisconnected flag in ConnectionManager. Manual disconnect (Disconnect button / HUD Disconnect) calls the new userDisconnect() which sets userDisconnected=true before calling stopCurrentSession(). retryConnect() (Connect button) clears the flag. All 8 auto-reconnect paths are guarded: stopCurrentSession() immediate trigger, fallbackPollRunnable, onUsbConnected() probe, startWifiDiscovery() callback, tryConnect() main-thread post, onResume(), onSurfaceAvailable(), and onUsbDisconnected()→startWifiDiscovery().
  - Distinction: ONLY userDisconnect() sets the flag. Internal hot-switch paths (onUsbConnected, onUsbDisconnected, surface lifecycle) call stopCurrentSession() directly and do NOT set the flag, so involuntary drops still auto-recover as before.
  - Changed files: android/.../ConnectionManager.kt, android/.../MainActivity.kt
- Phase 4e: Connection Mode label (actual fix) + reconnect speed (actual fix) ✅
  - Label fix (Issue 1) — root cause: stopCurrentSession() called onTransport("—") on every session end, always resetting the dashboard label to "—". Since tvConnectionMode is inside homePanel (only visible when NOT streaming), the user only ever saw "—" (after disconnect) or briefly "USB"/"Wi-Fi" during the <500ms connecting window before showStreamingUi() hid homePanel. The previous attempt (re-emitting onTransport in onSenderIpReceived) fired during connect, but homePanel hid immediately after. Real fix: removed onTransport("—") from stopCurrentSession() — label now retains last-known transport ("USB" or "Wi-Fi") while on the dashboard. Changed file: android/.../ConnectionManager.kt
  - Reconnect speed (Issue 2) — root cause: after stopCurrentSession(), the ONLY automatic reconnect trigger was fallbackPollRunnable (10s interval), causing 0–10s random wait before any reconnect attempt. Touch socket timing (300ms+100ms=400ms/attempt) was not the primary bottleneck. Real fix: added immediate mainHandler.post { Thread { tryConnect() }.start() } at the end of stopCurrentSession(), guarded by !destroyed && receiver==null && surface!=null. Also added @Volatile destroyed flag (set in destroy() before stopCurrentSession()) to prevent spurious reconnect on app exit. onSurfaceDestroyed() already sets surface=null before calling stopCurrentSession(), so that guard also fires correctly. Touch retry further tightened: 200ms+50ms=250ms/attempt. Changed files: android/.../ConnectionManager.kt, android/.../TouchSender.kt
  - Codec-config spam (Item 3 from prior session): Session::ResendLoop sent stream_info + codec_config unconditionally every 2s, causing continuous "Codec config received" log noise on Android even mid-stream. Added !android_ready_.load() guard so resending stops once ACK is received; automatically resumes when android_ready_ is reset (encoder re-init on resolution change, or new Session from reconnect). Changed file: windows/src/Session.cpp
- Phase 4b: WiFi→USB hot-switch + Connection Mode label ✅
  - Root cause: StartUsbMonitorThread was only started when USB was present at app launch. Starting in WiFi mode meant no USB monitor ran, so adb reverse was never set up when a cable was plugged mid-session. Android probed 127.0.0.1:7777 once, failed silently, gave up.
  - Windows fix: StartUsbMonitorThread now called unconditionally after startup (main.cpp), regardless of whether USB was present at launch. Monitor detects absent→present USB transition and runs RunAdbUsbReverse(-s <serial> tcp:7777 + tcp:7778) without touching the active WiFi session.
  - Android fix: onUsbConnected() now retries probeHost("127.0.0.1") every 1 s for up to 5 s (instead of one-shot), catching the window while Windows sets up adb reverse. Each attempt is logged. On success: stopCurrentSession() + startUsbSession() with saved mode (no dialog).
  - Label fix: startWifiDiscovery() now fires onTransport("Wi-Fi") before the early-return guard, ensuring the Connection Mode label always updates to "Wi-Fi" even when discovery was already running.
  - Changed files: windows/src/main.cpp, android/.../ConnectionManager.kt
- #19: Auto-switch USB/WiFi — UsbManager detection on startup + BroadcastReceiver for plug/unplug ✅
  - Follow-up: Fixed black screen/reconnect loop on WiFi→USB switch — race condition where old WiFi StreamReceiver threads were still alive when TcpStreamReceiver started. Fix: explicit stopReceiver() + 600ms postDelayed before setMode(true)+autoStartIfNeeded(). Also added reverse USB→WiFi auto-detection (probe failure → same stop+delay+setMode pattern). ✅
  - Bug 1: Fixed adb reverse lost after USB reconnect — added StartUsbMonitorThread() in AdbUsbSetup.cpp that polls DetectUsbDevice() every 3s and re-runs RunAdbUsbReverse() on device absent→present transition. Started in main.cpp after initial adb reverse. ✅
  - Bug 2: Extended mode logging — added raw mode-line log in TcpVideoServer::AcceptLoop() before parsing; confirmed both WiFi and USB paths parse "extend" correctly. ✅
  - Bug 3: Fixed random Windows crashes — ScreenCapture now stores adapter_idx_/output_idx_ and guards CaptureFrame() with null check on duplication_ (re-inits on DXGI_ERROR_ACCESS_LOST / resolution change). Main loop wrapped in try-catch; crashes logged to PocketDisplay_crash.log. ✅
- #19 post-testing fixes (3 remaining bugs):
  - Bug 1 (CRITICAL): Fixed Windows crash in libx264-164.dll during Extended mode — three-layer fix: (1) ScreenCapture::CaptureFrame validates mapped.pData non-null and RowPitch≥width*4 before memcpy; (2) Encoder::EncodeFrame guards null bgra/handle, delegates x264_encoder_encode to SafeX264Encode() static helper wrapped in __try/__except (C2712 fix); (3) main.cpp capture loop detects resolution change (w/h != stream_w/h), closes and re-inits encoder with new dims, resets android_ready to force codec-config resend. stream_w/stream_h are std::atomic<int> so resend_thread always sends current dims. ✅
  - Bug 2 (HIGH): Fixed adb reverse lost after USB reconnect — removed `receiver?.isRunning != true && tcpReceiver?.isRunning != true` guard from both branches of usbPollRunnable in MainActivity.kt. USB is now probed every 3s regardless of whether a WiFi receiver is actively streaming, so reconnecting USB cable triggers mode-switch even mid-stream. The inner `if (!usbMode)` / `if (usbMode)` guards still prevent double-switching. ✅
  - Bug 3 (MEDIUM): Fixed Extended mode showing Mirror — three-part fix: (1) RunDiscovery in main.cpp no longer requires phase2 (CLIENT received) before accepting POCKETDISPLAY_MODE; if CLIENT was lost, android_ip is inferred from MODE sender IP, prevents 30s timeout → Mirror fallback; added raw mode_str log. (2) TcpVideoServer::AcceptLoop always updates mode_value_ on each connection (removed keep-first guard) so reconnect with different mode is reflected. (3) Added Log.i in MainActivity.sendModeSelection for WiFi path tracing. ✅
- #19 second post-testing session (4 more bugs):
  - Bug 1 (CRITICAL): Fixed Android-first + USB black screen — usbPollRunnable now checks firstFrameReceived before stopReceiver(). If WiFi was idle/connecting (not streaming), goes directly to setMode(true)+autoStartIfNeeded() with no stopReceiver() and no delay. stopReceiver() on a never-connected WiFi receiver was corrupting state and leaving black screen. If WiFi IS streaming (firstFrameReceived=true), still does stopReceiver()+600ms+setMode(true). ✅
  - Bug 2 (CRITICAL): Fixed green/black flicker on Windows-first USB — encoder resize logic now requires 3 consecutive frames at the new size before re-initializing (hysteresis). Single-frame DXGI size changes (common during VDD startup/transitions) were triggering unnecessary encoder re-inits that reset android_ready, causing Android decoder reconfiguration and green/black frames. ✅
  - Bug 3 (MEDIUM): Improved Extended mode → Mirror debugging — added [MODE] logging throughout (sendModeSelection, DiscoveryClient.sendMode, TcpStreamReceiver). Added direct UDP fallback in sendModeSelection() for case where discoverClient is null (e.g., due to race with USB probe). Changed Log.d→Log.i in sendMode(). ✅
  - Bug 4 (MEDIUM): USB reconnect stays WiFi — Bug 1 fix (direct setMode without stopReceiver) also removes the 600ms delay for idle WiFi→USB switch, making reconnect faster and more reliable. Improved AdbUsbSetup log: "[USB Monitor] adb reverse re-run: SUCCESS/FAIL". ✅
- Phase 1 HELLO handshake regressions (2 critical bugs):
  - Bug A (CRITICAL): Fixed HELLO always carrying mode=Mirror — root cause: in TcpStreamReceiver.start() the !sessionStarted block reset pendingMode=null on every reconnect before the first successful write. When Windows' 5 s RCVTIMEO closed the socket after the user tapped the dialog (write not yet sent, so sessionStarted still false), the user's selection was wiped; modeSelected=true then blocked the dialog from re-showing; the wait-loop timed out → modeActual=null → HELLO not sent → WaitForMode defaulted to Mirror. Fix: removed pendingMode=null from the !sessionStarted block. pendingMode is null by default in a freshly constructed TcpStreamReceiver so new sessions remain clean. Changed file: TcpStreamReceiver.kt. ✅
  - Bug B (CRITICAL): Fixed codec-config resend loop breaking streaming — root cause: VideoDecoder.configure() called onConfigured?.invoke() even on duplicate SPS (dedup path). resend_thread sends codec_config every 2 s unconditionally; each duplicate triggered onCodecConfigured(), which for WiFi (after firstFrameReceived=true) called stopReceiver() — killing streaming 2 s after the first frame arrived; for USB it reset firstFrameReceived=false causing continuous spurious ACKs. Fix: removed onConfigured call from the dedup branch (silent return). Windows-restart detection still works because that produces a *different* SPS, bypassing dedup and reaching the full reconfigure + onConfigured path. Changed file: VideoDecoder.kt. ✅
- #19 third post-testing session (logcat-confirmed bugs):
  - Bug 1 (CRITICAL): Fixed Windows closing TCP connection every 3s — Android's usbPollRunnable calls tcpProbe() which creates a real TCP connection to port 7777. TcpVideoServer::AcceptLoop() was accepting probe connections, reading an empty mode line, then: (a) updating mode_value_ to 0 (Mirror), waking WaitForMode() with wrong mode; (b) calling reconnect_cb_() which reset android_ready=false; (c) setting client_sock_ to probe socket, closing the real streaming socket. Fixed in AcceptLoop(): if mode line is empty → log "[USB/video] empty mode line (TCP probe) — discarded" + closesocket + continue, WITHOUT touching client_sock_, mode_value_, or reconnect_cb_. Also added guard for unrecognised (non-POCKETDISPLAY_MODE) lines. ✅
  - Bug 2 (MEDIUM): Fixed Extended mode reverts to Mirror on reconnect — setMode(usb=true) now always resets modeSelected=false, modeDialogShowing=false, selectedMode="mirror" in the USB branch, even when stopReceiver() is not called (idle WiFi→USB switch). Previously, stale modeSelected=true from a prior WiFi session caused autoStartIfNeeded() to skip the mode dialog and start TcpStreamReceiver with the last selectedMode value (which could be "mirror"). ✅
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

## HELLO Handshake (Phase 1 — unified in-band mode selection)
Both USB and WiFi now use the same binary HELLO message to deliver mode.
Frame: [4-byte BE length = 11][type=4][version=1][mode 0/1][w uint32BE][h uint32BE]
- type 4 = kHello, version 1, mode 0=mirror / 1=extend
- w/h = Android screen dimensions
USB: TcpStreamReceiver sends HELLO as the first framed message on the streaming TCP socket.
WiFi: Android opens a short-lived TCP connection to Windows:7777, sends HELLO, closes.
  Windows runs a TcpVideoServer on :7777 in both USB and WiFi modes to accept HELLO.
Probe guard: connections that close without a valid HELLO are discarded silently.
Unknown HELLO version: log + default Mirror (graceful reject).
UDP port 7779 is now ANNOUNCEMENT-ONLY: POCKETDISPLAY_HOST + POCKETDISPLAY_CLIENT.
  POCKETDISPLAY_MODE packets are no longer sent or handled.

## Phase 2 — Session-based server (refactor/unified-connection)
Replaced the linear main.cpp streaming script with a persistent server.
- TcpVideoServer::SetHelloCallback() fires on the AcceptLoop thread for every valid HELLO.
  USB: hands off the accepted SOCKET to DirectSocketStreamer (caller owns it).
  WiFi: also hands off SOCKET to DirectSocketStreamer (Phase 3 change — TCP video for WiFi too).
- Session class owns ScreenCapture + encoder + TouchReceiver + stream/resend/cursor threads.
  Start() → initialises all; Stop() → signals running_=false, closes streamer socket (unblocks
  send()), joins all threads, releases capture.
- On new HELLO: old Session::Stop() called, new Session created.
- On disconnect: Session::StreamLoop detects SendFrame failure → running_=false;
  main loop detects !IsRunning() → Stop() + reset → waits for next HELLO.
- Discovery broadcaster runs continuously on a background thread; pauses while a
  Session is active (prevents spurious WiFi reconnects during USB session).
- WaitForMode() and one-shot RunDiscovery blocking calls removed from main().

## Phase 3 — Unified TCP transport (refactor/unified-connection)
All video and touch now flows over TCP for both USB and WiFi modes.
- Windows: TcpVideoServer hands off accepted SOCKET for WiFi too (not just USB).
  TouchReceiver is TCP-only (UdpLoop removed). UdpStreamer removed from build.
- Android: ConnectionManager replaces the volatile-flag + polling pattern in MainActivity.
  Manages USB/WiFi lifecycle, discovery, session start/stop, mode persistence.
  TcpStreamReceiver gains a `host` parameter (supports WiFi IP, not just 127.0.0.1).
  TouchSender is TCP-only (UDP socket and useTcp param removed).
  MainActivity is now UI-only — all connection logic delegated to ConnectionManager.

## ADB Multi-device Fix
- DetectUsbDevice() / GetUsbSerial() now skip entries whose serial contains ':'
  (wireless-adb format e.g. 192.168.1.5:5555).
- RunAdbUsbReverse() and ClearAdbReverse() pass -s <usb_serial> on every adb
  call, preventing "error: more than one device/emulator" when wireless adb is
  also active alongside a USB device.

## Connection Flow (USB — Phase 2)
1. Windows starts → DetectUsbDevice() → RunAdbUsbReverse(-s <serial>); StartUsbMonitorThread
2. TcpVideoServer always-on HELLO listener on :7777
3. Android connects → sends HELLO; AcceptLoop fires hello_cb_ (hands off SOCKET)
4. Session::Start() → ScreenCapture + encoder init; touch receiver on :7778 (TCP)
5. ResendLoop sends stream_info + codec_config every 2s
6. Android TouchSender connects → ACK received → android_ready=true → frames flow
7. On disconnect: StreamLoop fails → running_=false → main tears down session; loops back to 2

## Connection Flow (WiFi — Phase 3)
1. Windows starts → discovery broadcaster on UDP :7779 (continuous background thread)
2. TcpVideoServer always-on HELLO listener on :7777
3. Android receives POCKETDISPLAY_HOST → user taps mode dialog
4. Android opens short-lived TCP to :7777 → sends HELLO → Windows keeps socket open
5. AcceptLoop fires hello_cb_ (hands off SOCKET for both USB and WiFi)
6. Session::Start() → capture + encoder + DirectSocketStreamer (TCP); touch receiver on :7778 (TCP)
7. ACK received → android_ready=true → frames flow
8. On disconnect/silence: main detects !IsRunning() → Stop(); discovery resumes

## Important Rules
- Never use system() or _popen() for adb — causes cmd popup
- Use CreateProcess() with CREATE_NO_WINDOW for silent adb calls
- Extended mode uses the bundled VirtualDrivers Virtual Display Driver at windows/drivers/virtual-display; install is attempted only when extended mode is selected
- Always test: Windows-first, Android-first, AND reconnect scenarios
- Do not break existing working flows when fixing new issues
