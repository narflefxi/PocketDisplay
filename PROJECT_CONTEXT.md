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
- **Video rendering**: `setTransform(null)` + `scaleX/scaleY` (set in `applyFillTransform`) — buffer stretched to fill the TextureView, then letterbox-fit scale applied from center pivot. DXGI captures upright in Landscape mode; no rotation needed.
- **Touch → Windows** (`toNormalized`): Divide `event.x/y` by `textureView.width/height`.
  `applyFillTransform` sets `textureView.scaleX/scaleY` with pivot at center; the parent
  ConstraintLayout applies the inverse of that transform when dispatching touch events, so
  `event.x/y` arrive in the TextureView's LOCAL coordinate space.  Due to the symmetric
  scale-from-center pivot the entire video content maps to `[0, vw] × [0, vh]` in local
  space — no offset subtraction, no `videoScaledW/H` divisor:
  ```kotlin
  nx = (tx / textureView.width).coerceIn(0f, 1f)
  ny = (ty / textureView.height).coerceIn(0f, 1f)
  ```
  Using `videoOffsetX/Y` (screen-space) with local-space `event.x/y` was the bug that
  caused the cursor to land above the touched point (feature/cursor-fixes).
- **Touch cursor overlay** (`toScreenPosition`): Inverse of `toNormalized` — overlay appears at finger position:
  ```kotlin
  sx = videoOffsetX + nx * videoScaledW
  sy = videoOffsetY + ny * videoScaledH
  ```
- **Windows cursor → overlay** (`onCursorPos`): Same direct mapping within the letterboxed video area:
  ```kotlin
  sx = videoOffsetX + nx * videoScaledW
  sy = videoOffsetY + ny * videoScaledH
  ```
- `CursorOverlayView.kt`: Draws a Windows-style arrow cursor. Hotspot at `(0,0)` = tip; path scaled by `s = displayMetrics.density` (dp units). No rotation applied to the overlay itself.

---

## Do Not Change
- WiFi UDP mode
- TCP framing format
- `textureView.rotation = 180f` (corrects OpenGL y-flip; must stay)
- MediaCodec decoder logic