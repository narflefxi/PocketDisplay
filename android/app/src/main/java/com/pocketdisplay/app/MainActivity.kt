package com.pocketdisplay.app

import android.graphics.SurfaceTexture
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.Editable
import android.text.TextWatcher
import android.util.Log
import android.view.HapticFeedbackConstants
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.TextureView
import android.view.View
import android.view.WindowManager
import android.net.wifi.WifiManager
import android.view.inputmethod.InputMethodManager
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.pocketdisplay.app.databinding.ActivityMainBinding
import java.net.Inet4Address
import java.net.NetworkInterface

class MainActivity : AppCompatActivity(), TextureView.SurfaceTextureListener {

    companion object {
        private const val TAG = "PocketDisplay"
    }

    private lateinit var binding: ActivityMainBinding
    private lateinit var cm: ConnectionManager

    private var multicastLock: WifiManager.MulticastLock? = null

    @Volatile private var videoW = 0
    @Volatile private var videoH = 0
    @Volatile private var windowsW = 0
    @Volatile private var windowsH = 0

    // Computed in applyFillTransform(); read in cursor callbacks from any thread.
    @Volatile private var videoScaledW = 0f
    @Volatile private var videoScaledH = 0f
    @Volatile private var videoOffsetX = 0f
    @Volatile private var videoOffsetY = 0f

    // ── Tap / gesture state ─────────────────────────────────────────────────
    private var touchDownX = 0f
    private var touchDownY = 0f
    private val TAP_SLOP_PX = 20f

    // Long-press → right-click
    private val longPressHandler = Handler(Looper.getMainLooper())
    private var longPressRunnable: Runnable? = null
    private var longPressConsumed = false
    private val LONG_PRESS_MS     = 500L
    private val LONG_PRESS_SLOP   = 15f

    // Gesture state
    private enum class GestureState { IDLE, DRAGGING, SCROLLING }
    private var gestureState = GestureState.IDLE
    private var scrollPrevX  = 0f
    private var scrollPrevY  = 0f
    private val SCROLL_SCALE = 120f / 40f

    // Keyboard capture
    private var prevInputText = ""
    private var keyboardVisible = false

    private var transformLogged = false

    // First-frame loading cover
    private val firstFrameHandler = Handler(Looper.getMainLooper())
    private val firstFrameTimeoutRunnable = Runnable {
        updateStatus("Connecting\u2026 (waiting for video)")
    }
    @Volatile private var videoShowPending = false

    /** Coalesced UI update: decoder / network callbacks can race with layout & [setDefaultBufferSize]. */
    private val applyFillTransformRunnable = Runnable { applyFillTransform() }

    private fun scheduleApplyFillTransform() {
        val tv = binding.textureView
        tv.removeCallbacks(applyFillTransformRunnable)
        tv.post(applyFillTransformRunnable)
    }

    // Stats
    private var framesDecoded = 0L
    private var statsHandler  = Handler(Looper.getMainLooper())
    private val statsRunnable = object : Runnable {
        override fun run() {
            if (cm.isSessionActive) {
                val fps = "↓ ${framesDecoded / 5} fps"
                binding.tvStats.text = fps
                binding.hudStats.text = fps
                framesDecoded = 0
            }
            statsHandler.postDelayed(this, 5000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_FULLSCREEN
            or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
            or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // Acquire multicast lock so WiFi doesn't filter out multicast discovery packets.
        val wm = applicationContext.getSystemService(WIFI_SERVICE) as WifiManager
        multicastLock = wm.createMulticastLock("PocketDisplay").also { it.acquire() }

        binding.textureView.surfaceTextureListener = this
        binding.tvDeviceIp.text = "IP: ${getLocalIp()}"

        cm = ConnectionManager(this)
        setupConnectionManager()

        binding.btnConnect.setOnClickListener {
            if (cm.isSessionActive) cm.userDisconnect() else cm.retryConnect()
        }
        binding.navAbout.setOnClickListener {
            binding.panelDashboard.visibility = View.GONE
            binding.panelAbout.visibility = View.VISIBLE
        }
        binding.navDashboard.setOnClickListener {
            binding.panelAbout.visibility = View.GONE
            binding.panelDashboard.visibility = View.VISIBLE
        }
        binding.hudDisconnect.setOnClickListener { cm.userDisconnect() }
        binding.hudKeyboard.setOnClickListener { toggleKeyboard() }
        binding.btnModeToggle.setOnClickListener {
            cm.toggleDisplayMode()
            updateModeToggleButton()
        }

        binding.textureView.setOnTouchListener { _, event ->
            handleTouch(event)
            true
        }

        setupKeyboardCapture()
        updateModeToggleButton()
        statsHandler.postDelayed(statsRunnable, 5000)
    }

    private fun setupConnectionManager() {
        cm.onStatus    = { msg -> runOnUiThread { updateStatus(msg) } }
        cm.onTransport = { label -> runOnUiThread { binding.tvConnectionMode.text = label } }
        cm.onConnected = { connected ->
            runOnUiThread {
                setStatusDot(connected)
                if (connected) {
                    binding.btnConnect.text = "Disconnect"
                    showStreamingUi()
                    binding.videoLoadingCover.visibility = View.VISIBLE
                    firstFrameHandler.removeCallbacks(firstFrameTimeoutRunnable)
                    firstFrameHandler.postDelayed(firstFrameTimeoutRunnable, 5000)
                } else {
                    binding.btnConnect.text = "Connect"
                    showHomeUi()
                }
            }
        }
        cm.onExtendedBadge = { extended ->
            runOnUiThread {
                val vis = if (extended) View.VISIBLE else View.GONE
                binding.tvExtendedBadge.visibility = vis
                binding.hudExtBadge.visibility     = vis
            }
        }
        cm.onDisplayMode = { _ -> runOnUiThread { updateModeToggleButton() } }
        cm.onNeedModeDialog = { hostLabel -> runOnUiThread { showModeDialog(hostLabel) } }
        cm.onVideoDimensions = { w, h ->
            videoW = w; videoH = h
            runOnUiThread {
                binding.textureView.surfaceTexture?.setDefaultBufferSize(w, h)
                scheduleApplyFillTransform()
            }
        }
        cm.onWindowsSize = { w, h ->
            windowsW = w; windowsH = h
            transformLogged = false
            runOnUiThread {
                binding.textureView.surfaceTexture?.setDefaultBufferSize(w, h)
                scheduleApplyFillTransform()
            }
        }
        cm.onCursorPos = { nx, ny, type ->
            val vw = if (videoScaledW > 0f) videoScaledW else binding.textureView.width.toFloat()
            val vh = if (videoScaledH > 0f) videoScaledH else binding.textureView.height.toFloat()
            if (vw > 0f && vh > 0f) {
                val sx = videoOffsetX + nx * vw
                val sy = videoOffsetY + ny * vh
                runOnUiThread { binding.cursorOverlay.moveTo(sx, sy, type) }
            }
        }
        cm.onFirstFrame = {
            runOnUiThread {
                firstFrameHandler.removeCallbacks(firstFrameTimeoutRunnable)
                binding.videoLoadingCover.visibility = View.GONE
                binding.textureView.animate().alpha(1f).setDuration(200).start()
                videoShowPending = false
            }
        }
        cm.onStreamMode = { flags ->
            val extended = (flags and 1) != 0
            runOnUiThread {
                val vis = if (extended) View.VISIBLE else View.GONE
                binding.tvExtendedBadge.visibility = vis
                binding.hudExtBadge.visibility     = vis
            }
        }
    }

    // ── Mode dialog ───────────────────────────────────────────────────────────

    private fun showModeDialog(hostLabel: String) {
        AlertDialog.Builder(this)
            .setTitle("Select Display Mode")
            .setMessage("$hostLabel\n\nHow should Windows share its screen?")
            .setPositiveButton("Mirror")   { _, _ -> cm.onModeSelected("mirror") }
            .setNegativeButton("Extended") { _, _ -> cm.onModeSelected("extend") }
            .setCancelable(false)
            .show()
    }

    private fun updateModeToggleButton() {
        val mode = cm.displayModeName
        binding.btnModeToggle.text = if (mode == "extend") "Mode: Extended" else "Mode: Mirror"
    }

    // ── Streaming UI ──────────────────────────────────────────────────────────

    private fun showHomeUi() {
        hideKeyboard()
        binding.cursorOverlay.hide()
        binding.homePanel.visibility = View.VISIBLE
        binding.streamHud.visibility = View.GONE
        binding.textureView.animate().cancel()
        binding.textureView.alpha = 1f
        binding.textureView.scaleX = 1f
        binding.textureView.scaleY = 1f
        binding.textureView.rotation = 0f
        binding.textureView.removeCallbacks(applyFillTransformRunnable)
        videoW = 0; videoH = 0; windowsW = 0; windowsH = 0
        videoScaledW = 0f; videoScaledH = 0f; videoOffsetX = 0f; videoOffsetY = 0f
        binding.tvExtendedBadge.visibility = View.GONE
        binding.hudKeyboard.isEnabled = false
        videoShowPending = false
        firstFrameHandler.removeCallbacks(firstFrameTimeoutRunnable)
        binding.videoLoadingCover.visibility = View.GONE
    }

    private fun showStreamingUi() {
        // DXGI desktop capture is always upside-down; pre-apply the 180° rotation.
        binding.textureView.animate().cancel()
        binding.textureView.rotation = 180f
        binding.textureView.alpha = 0f
        videoShowPending = true
        binding.homePanel.visibility = View.GONE
        binding.streamHud.visibility = View.GONE  // revealed on tap
        binding.hudKeyboard.isEnabled = true
        binding.hudIp.text = binding.tvDeviceIp.text
    }

    private fun updateStatus(msg: String) {
        binding.tvStatus.text = msg
        binding.hudStatus.text = msg
    }

    private fun setStatusDot(connected: Boolean) {
        val res = if (connected) R.drawable.dot_connected else R.drawable.dot_disconnected
        binding.statusDot.setBackgroundResource(res)
        binding.hudDot.setBackgroundResource(res)
    }

    // ── Video transform ──────────────────────────────────────────────────────

    private fun applyFillTransform() {
        val bufW = videoW.toFloat()
        val bufH = videoH.toFloat()
        val vw = binding.textureView.width.toFloat()
        val vh = binding.textureView.height.toFloat()

        // Log every attempt so we can see which guard fires.
        if (!transformLogged) {
            Log.d(TAG, "=== applyFillTransform attempt ===")
            Log.d(TAG, "  TextureView : ${vw.toInt()} x ${vh.toInt()}")
            Log.d(TAG, "  Buffer      : ${bufW.toInt()} x ${bufH.toInt()}  windows=${windowsW}x${windowsH}")
        }

        if (bufW == 0f || bufH == 0f) return
        if (vw == 0f || vh == 0f) return

        // Same contain math for cursor overlay (Windows norm → view); use logical desktop
        // size when known so cursor matches GetCursorPos, even if the encoded frame is padded.
        val contentW = if (windowsW > 0) windowsW.toFloat() else bufW
        val contentH = if (windowsH > 0) windowsH.toFloat() else bufH
        val scale = minOf(vw / contentW, vh / contentH)
        // Snap to integer pixels — fractional coordinates cause bilinear sub-pixel ghosting.
        val scaledW = kotlin.math.round(contentW * scale).toFloat()
        val scaledH = kotlin.math.round(contentH * scale).toFloat()
        val offsetX = kotlin.math.round((vw - scaledW) / 2f).toFloat()
        val offsetY = kotlin.math.round((vh - scaledH) / 2f).toFloat()

        videoScaledW = scaledW
        videoScaledH = scaledH
        videoOffsetX = offsetX
        videoOffsetY = offsetY

        binding.textureView.pivotX = vw / 2f
        binding.textureView.pivotY = vh / 2f
        binding.textureView.scaleX = scaledW / vw
        binding.textureView.scaleY = scaledH / vh
        binding.textureView.setTransform(null)
        binding.textureView.rotation = 180f

        if (!transformLogged) {
            transformLogged = true
            Log.d(TAG, "=== applyFillTransform SUCCESS ===")
            Log.d(TAG, "  scale     : $scale")
            Log.d(TAG, "  scaledW/H : ${scaledW.toInt()} x ${scaledH.toInt()}")
            Log.d(TAG, "  offsetX/Y : ${offsetX.toInt()} , ${offsetY.toInt()}")
        }
    }

    // ── Touch handling ───────────────────────────────────────────────────────

    private fun handleTouch(event: MotionEvent) {
        val sender = cm.touchSender

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                gestureState = GestureState.DRAGGING
                touchDownX = event.x; touchDownY = event.y
                longPressConsumed = false
                val (nx, ny) = toNormalized(event.x, event.y)
                moveCursorTo(toScreenPosition(nx, ny))
                sender?.send(TouchSender.EventType.DOWN, nx, ny)
                armLongPress(sender)
            }

            MotionEvent.ACTION_POINTER_DOWN -> {
                if (event.pointerCount == 2) {
                    cancelLongPress()
                    if (gestureState == GestureState.DRAGGING) {
                        val (nx, ny) = toNormalized(touchDownX, touchDownY)
                        sender?.send(TouchSender.EventType.UP, nx, ny)
                    }
                    gestureState = GestureState.SCROLLING
                    scrollPrevX = (event.getX(0) + event.getX(1)) / 2f
                    scrollPrevY = (event.getY(0) + event.getY(1)) / 2f
                }
            }

            MotionEvent.ACTION_MOVE -> {
                when (gestureState) {
                    GestureState.DRAGGING -> {
                        val (nx, ny) = toNormalized(event.x, event.y)
                        moveCursorTo(toScreenPosition(nx, ny))
                        sender?.send(TouchSender.EventType.MOVE, nx, ny)
                        val dx = event.x - touchDownX; val dy = event.y - touchDownY
                        if (dx*dx + dy*dy > LONG_PRESS_SLOP * LONG_PRESS_SLOP) cancelLongPress()
                    }
                    GestureState.SCROLLING -> {
                        if (event.pointerCount >= 2) {
                            val cx = (event.getX(0) + event.getX(1)) / 2f
                            val cy = (event.getY(0) + event.getY(1)) / 2f
                            val dx = cx - scrollPrevX; val dy = cy - scrollPrevY
                            scrollPrevX = cx; scrollPrevY = cy
                            val sv = (-dy * SCROLL_SCALE).toInt()
                            val sh = (dx  * SCROLL_SCALE).toInt()
                            if (sv != 0 || sh != 0)
                                sender?.send(TouchSender.EventType.SCROLL, sh.toFloat(), sv.toFloat())
                        }
                    }
                    GestureState.IDLE -> Unit
                }
            }

            MotionEvent.ACTION_POINTER_UP -> {
                if (gestureState == GestureState.SCROLLING) gestureState = GestureState.IDLE
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                cancelLongPress()
                if (gestureState == GestureState.DRAGGING) {
                    val (nx, ny) = toNormalized(event.x, event.y)
                    moveCursorTo(toScreenPosition(nx, ny))
                    sender?.send(TouchSender.EventType.UP, nx, ny)
                    if (!longPressConsumed) {
                        val dx = event.x - touchDownX; val dy = event.y - touchDownY
                        if (dx*dx + dy*dy < TAP_SLOP_PX * TAP_SLOP_PX) {
                            // Tap: toggle streaming HUD
                            binding.streamHud.visibility =
                                if (binding.streamHud.visibility == View.VISIBLE) View.GONE
                                else View.VISIBLE
                        }
                    }
                }
                gestureState = GestureState.IDLE
            }
        }
    }

    private fun armLongPress(sender: TouchSender?) {
        val lp = Runnable {
            longPressConsumed = true
            val (lnx, lny) = toNormalized(touchDownX, touchDownY)
            sender?.send(TouchSender.EventType.RIGHT_CLICK, lnx, lny)
            binding.cursorOverlay.performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
        }
        longPressRunnable = lp
        longPressHandler.postDelayed(lp, LONG_PRESS_MS)
    }

    private fun cancelLongPress() {
        longPressRunnable?.let { longPressHandler.removeCallbacks(it) }
        longPressRunnable = null
    }

    // ── Keyboard capture ─────────────────────────────────────────────────────

    private fun setupKeyboardCapture() {
        binding.hiddenInput.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence, start: Int, before: Int, count: Int) {}
            override fun afterTextChanged(s: Editable) {
                val curr = s.toString()
                val prev = prevInputText
                if (curr.length > prev.length) {
                    curr.substring(prev.length).forEach { c -> cm.touchSender?.sendKeyChar(c) }
                } else if (curr.length < prev.length) {
                    repeat(prev.length - curr.length) {
                        cm.touchSender?.sendKeyVk(TouchSender.WinVK.BACK, true)
                        cm.touchSender?.sendKeyVk(TouchSender.WinVK.BACK, false)
                    }
                }
                prevInputText = curr
            }
        })

        // Capture special keys (Enter, Tab, Arrows, Escape) before IME consumes them
        binding.hiddenInput.setOnKeyListener { _, keyCode, event ->
            val vk = androidToWinVk(keyCode) ?: return@setOnKeyListener false
            val down = event.action == KeyEvent.ACTION_DOWN
            cm.touchSender?.sendKeyVk(vk, down)
            true
        }
    }

    private fun toggleKeyboard() {
        if (keyboardVisible) hideKeyboard() else showKeyboard()
    }

    private fun showKeyboard() {
        keyboardVisible = true
        binding.hiddenInput.requestFocus()
        prevInputText = ""
        binding.hiddenInput.text?.clear()
        val imm = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager
        imm.showSoftInput(binding.hiddenInput, InputMethodManager.SHOW_FORCED)
    }

    private fun hideKeyboard() {
        keyboardVisible = false
        val imm = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager
        imm.hideSoftInputFromWindow(binding.hiddenInput.windowToken, 0)
    }

    private fun androidToWinVk(keyCode: Int): Int? = when (keyCode) {
        KeyEvent.KEYCODE_ENTER             -> TouchSender.WinVK.RETURN
        KeyEvent.KEYCODE_DEL               -> TouchSender.WinVK.BACK
        KeyEvent.KEYCODE_FORWARD_DEL       -> TouchSender.WinVK.DELETE
        KeyEvent.KEYCODE_TAB               -> TouchSender.WinVK.TAB
        KeyEvent.KEYCODE_ESCAPE            -> TouchSender.WinVK.ESCAPE
        KeyEvent.KEYCODE_DPAD_UP           -> TouchSender.WinVK.UP
        KeyEvent.KEYCODE_DPAD_DOWN         -> TouchSender.WinVK.DOWN
        KeyEvent.KEYCODE_DPAD_LEFT         -> TouchSender.WinVK.LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT        -> TouchSender.WinVK.RIGHT
        KeyEvent.KEYCODE_MOVE_HOME         -> TouchSender.WinVK.HOME
        KeyEvent.KEYCODE_MOVE_END          -> TouchSender.WinVK.END
        KeyEvent.KEYCODE_PAGE_UP           -> TouchSender.WinVK.PRIOR
        KeyEvent.KEYCODE_PAGE_DOWN         -> TouchSender.WinVK.NEXT
        KeyEvent.KEYCODE_SHIFT_LEFT,
        KeyEvent.KEYCODE_SHIFT_RIGHT       -> TouchSender.WinVK.SHIFT
        KeyEvent.KEYCODE_CTRL_LEFT,
        KeyEvent.KEYCODE_CTRL_RIGHT        -> TouchSender.WinVK.CONTROL
        KeyEvent.KEYCODE_ALT_LEFT,
        KeyEvent.KEYCODE_ALT_RIGHT         -> TouchSender.WinVK.MENU
        else -> null
    }

    // ── Coordinate helpers ────────────────────────────────────────────────────

    private fun toNormalized(tx: Float, ty: Float): Pair<Float, Float> {
        val vw = if (videoScaledW > 0f) videoScaledW else binding.textureView.width.toFloat()
        val vh = if (videoScaledH > 0f) videoScaledH else binding.textureView.height.toFloat()
        if (vw == 0f || vh == 0f) return Pair(0f, 0f)
        val x = ((tx - videoOffsetX) / vw).coerceIn(0f, 1f)
        val y = ((ty - videoOffsetY) / vh).coerceIn(0f, 1f)
        // TextureView is rotated 180°; invert both axes so Windows coords are correct.
        return Pair(1f - x, 1f - y)
    }

    private fun toScreenPosition(nx: Float, ny: Float): Pair<Float, Float> {
        val vw = if (videoScaledW > 0f) videoScaledW else binding.textureView.width.toFloat()
        val vh = if (videoScaledH > 0f) videoScaledH else binding.textureView.height.toFloat()
        // Inverse of toNormalized: map Windows-normalized coords back to physical screen pixels.
        return Pair(videoOffsetX + (1f - nx) * vw, videoOffsetY + (1f - ny) * vh)
    }

    private fun moveCursorTo(p: Pair<Float, Float>) =
        binding.cursorOverlay.moveTo(p.first, p.second)


    // -- TextureView / lifecycle --------------------------------------------------

    override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
        binding.btnConnect.isEnabled = true
        scheduleApplyFillTransform()
        cm.onSurfaceAvailable(Surface(st), w, h)
    }

    override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {
        runOnUiThread { scheduleApplyFillTransform() }
    }

    override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
        cm.onSurfaceDestroyed()
        runOnUiThread { binding.btnConnect.isEnabled = false }
        return true
    }

    override fun onSurfaceTextureUpdated(st: SurfaceTexture) {
        framesDecoded++
        if (videoShowPending) {
            videoShowPending = false
            firstFrameHandler.removeCallbacks(firstFrameTimeoutRunnable)
            binding.videoLoadingCover.visibility = View.GONE
            binding.textureView.animate().alpha(1f).setDuration(200).start()
        }
    }

    override fun onResume() {
        super.onResume()
        cm.onResume()
    }

    override fun onPause() {
        super.onPause()
        cm.onPause()
    }

    override fun onDestroy() {
        super.onDestroy()
        statsHandler.removeCallbacks(statsRunnable)
        cm.destroy()
        multicastLock?.release()
        multicastLock = null
    }

    // -- Helpers ------------------------------------------------------------------

    private fun getLocalIp(): String = try {
        NetworkInterface.getNetworkInterfaces()
            .asSequence()
            .flatMap { it.inetAddresses.asSequence() }
            .filterIsInstance<Inet4Address>()
            .filterNot { it.isLoopbackAddress }
            .firstOrNull()?.hostAddress ?: "unknown"
    } catch (_: Exception) { "unknown" }
}
