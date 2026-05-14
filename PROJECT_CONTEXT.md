# PocketDisplay

SuperDisplay-like low latency display streaming app.

Platforms:
- Windows host (C++)
- Android client (Kotlin)

## Status: Fully Working

- WiFi UDP streaming ✓
- USB TCP streaming ✓
- Video rendering correct (upright) ✓
- Touch input Android → Windows correct ✓
- Windows cursor overlay on Android correct ✓

---

## USB TCP Protocol

```
[4-byte big-endian payload length][1-byte message type][payload bytes]
```

Message types:
- `0` = video frame (kVideo)
- `1` = codec config / SPS+PPS (kCodec)
- `2` = display size metadata (kStreamInfo)
- `3` = cursor position (kCursor)

---

## Key Architecture Decisions

### Windows sender (`windows/src/`)
- `TcpVideoServer.cpp`: `client_mu_` lock covers the **entire** message send (length + type + payload) to prevent interleaved concurrent sends.
- `main.cpp`:
  - Display size (type=2) sent **once** per session via `dims_sent` flag.
  - Codec config (type=1) sent only while `!android_ready`; stops after Android ACK.
  - Cursor position (type=3) sent only when position actually changes.

### Android receiver (`android/app/src/main/java/com/pocketdisplay/app/`)
- `MainActivity.kt`: Uses `DataInputStream.readFully()` for strict framing reads.
- **Video rendering**: `setTransform(null)` + `textureView.rotation = 180f` — buffer is stretched to fill the full TextureView then rotated 180°. No custom matrix.
- **Touch → Windows** (`toNormalized`): Inverts both axes to match 180° rotation:
  ```kotlin
  nx = (1f - tx / vw).coerceIn(0f, 1f)
  ny = (1f - ty / vh).coerceIn(0f, 1f)
  ```
- **Touch cursor overlay** (`toScreenPosition`): Inverse of `toNormalized` so overlay appears where finger is:
  ```kotlin
  sx = (1f - nx) * vw
  sy = (1f - ny) * vh
  ```
- **Windows cursor → overlay** (`onWindowsCursorPos`): Raw mapping against full view size (no inversion — `setTransform(null)` stretches buffer to fill view, no letterbox offset):
  ```kotlin
  sx = nx * vw
  sy = ny * vh
  ```
- `CursorOverlayView.kt`: Draws a Windows-style arrow cursor. Hotspot at `(0,0)` = tip; path scaled by `s = displayMetrics.density` (dp units). No rotation applied to the overlay itself.

---

## Do Not Change
- WiFi UDP mode
- TCP framing format
- `toNormalized()` touch mapping
- `textureView.rotation = 180f`
- MediaCodec decoder logic