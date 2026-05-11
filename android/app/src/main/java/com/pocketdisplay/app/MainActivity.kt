package com.pocketdisplay.app

import android.graphics.Matrix
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
import android.view.inputmethod.InputMethodManager
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.pocketdisplay.app.databinding.ActivityMainBinding
import java.net.Inet4Address
import java.net.NetworkInterface

class MainActivity : AppCompatActivity(), TextureView.SurfaceTextureListener {

    companion object {
        private const val TAG = "PocketDisplay"
        private const val PREF_NAME = "pocketdisplay"
        private const val PREF_IP   = "last_ip"
        private const val PREF_MODE = "mode"   // "wifi" | "usb"
    }

    private lateinit var binding: ActivityMainBinding
    private var receiver: StreamReceiver? = null

    @Volatile private var videoW = 0
    @Volatile private var videoH = 0
    @Volatile private var windowsW = 0
    @Volatile private var windowsH = 0
    @Volatile private var touchSender: TouchSender? = null

    private var usbMode = false

    // Tap detection
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

    // Stats
    private var framesDecoded = 0L
    private var statsHandler  = Handler(Looper.getMainLooper())
    private val statsRunnable = object : Runnable {
        override fun run() {
            if (receiver?.isRunning == true) {
                binding.tvStats.text = "↓ ${framesDecoded / 5} fps"
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

        // Restore saved mode + IP
        val prefs = getSharedPreferences(PREF_NAME, MODE_PRIVATE)
        usbMode = (prefs.getString(PREF_MODE, "wifi") == "usb")
        binding.etWindowsIp.setText(prefs.getString(PREF_IP, ""))

        binding.textureView.surfaceTextureListener = this
        binding.tvDeviceIp.text = "IP: ${getLocalIp()}"
        updateModeUi()

        binding.btnModeWifi.setOnClickListener { setMode(false) }
        binding.btnModeUsb.setOnClickListener  { setMode(true)  }
        binding.btnConnect.setOnClickListener  { toggleReceiver() }
        binding.btnKeyboard.setOnClickListener { toggleKeyboard() }

        binding.textureView.setOnTouchListener { _, event ->
            // Single tap on video (outside HUD) → toggle HUD visibility
            handleTouch(event)
            true
        }

        setupKeyboardCapture()
        statsHandler.postDelayed(statsRunnable, 5000)
    }

    // ── Mode toggle ──────────────────────────────────────────────────────────

    private fun setMode(usb: Boolean) {
        usbMode = usb
        getSharedPreferences(PREF_NAME, MODE_PRIVATE).edit()
            .putString(PREF_MODE, if (usb) "usb" else "wifi").apply()
        updateModeUi()
    }

    private fun updateModeUi() {
        val alpha = 1f
        binding.btnModeWifi.alpha = if (usbMode) 0.4f else alpha
        binding.btnModeUsb.alpha  = if (usbMode) alpha else 0.4f
        binding.etWindowsIp.visibility = if (usbMode) View.GONE else View.VISIBLE
    }

    // ── Streaming control ────────────────────────────────────────────────────

    private fun toggleReceiver() {
        if (receiver?.isRunning == true) stopReceiver() else startReceiver()
    }

    private fun startReceiver() {
        val st = binding.textureView.surfaceTexture ?: return

        if (!usbMode) {
            val ip = binding.etWindowsIp.text.toString().trim()
            if (ip.isEmpty()) {
                Toast.makeText(this, "Enter Windows IP address", Toast.LENGTH_SHORT).show()
                return
            }
            getSharedPreferences(PREF_NAME, MODE_PRIVATE).edit().putString(PREF_IP, ip).apply()
        }

        val surface = Surface(st)
        receiver = StreamReceiver(
            surface,
            onStatus      = ::updateStatus,
            onDimensions  = ::onVideoDimensions,
            onSenderIp    = ::onWindowsIpKnown,
            onWindowsSize = ::onWindowsSizeKnown,
            onCursorPos   = ::onWindowsCursorPos
        )
        receiver?.start(useTcp = usbMode)

        binding.btnConnect.text = "Disconnect"
        binding.btnKeyboard.isEnabled = true
        setStatusDot(connected = false)
        updateStatus(if (usbMode) "Waiting for Windows (USB)…"
                     else "Waiting for stream on :${StreamReceiver.PORT}…")
    }

    private fun stopReceiver() {
        receiver?.stop(); receiver = null
        touchSender?.close(); touchSender = null
        videoW = 0; videoH = 0; windowsW = 0; windowsH = 0
        binding.cursorOverlay.hide()
        binding.btnConnect.text = "Connect"
        binding.btnKeyboard.isEnabled = false
        hideKeyboard()
        setStatusDot(connected = false)
        updateStatus("Disconnected")
    }

    // ── Callbacks ────────────────────────────────────────────────────────────

    private fun onVideoDimensions(w: Int, h: Int) {
        videoW = w; videoH = h
        binding.textureView.surfaceTexture?.setDefaultBufferSize(w, h)
        runOnUiThread { applyFillTransform() }
    }

    private fun onWindowsIpKnown(ip: String) {
        if (touchSender == null) {
            val senderIp = if (usbMode) "127.0.0.1" else ip
            touchSender = TouchSender(senderIp, useTcp = usbMode)
            runOnUiThread {
                setStatusDot(connected = true)
                updateStatus(if (usbMode) "Streaming via USB" else "Streaming via WiFi ↔ $ip")
            }
        }
    }

    private fun onWindowsSizeKnown(w: Int, h: Int) {
        windowsW = w; windowsH = h
        Log.d(TAG, "Windows: ${w}x${h}")
        runOnUiThread { applyFillTransform() }
    }

    private fun onWindowsCursorPos(nx: Float, ny: Float) {
        val vw = binding.textureView.width.toFloat()
        val vh = binding.textureView.height.toFloat()
        // Video is displayed with a horizontal flip (negative X scale in applyFillTransform),
        // so mirror the cursor X to keep it aligned with the visible content.
        val sx = (1f - nx) * vw
        val sy = ny * vh
        runOnUiThread { binding.cursorOverlay.moveTo(sx, sy) }
    }

    private fun updateStatus(msg: String) {
        runOnUiThread { binding.tvStatus.text = msg }
    }

    private fun setStatusDot(connected: Boolean) {
        runOnUiThread {
            binding.statusDot.setBackgroundResource(
                if (connected) R.drawable.dot_connected
                else           R.drawable.dot_disconnected
            )
        }
    }

    // ── Video transform ──────────────────────────────────────────────────────

    private fun applyFillTransform() {
        val contentW = if (windowsW > 0) windowsW else videoW
        val contentH = if (windowsH > 0) windowsH else videoH
        if (contentW == 0 || contentH == 0) return
        val vw = binding.textureView.width.toFloat()
        val vh = binding.textureView.height.toFloat()
        if (vw == 0f || vh == 0f) return

        val fill = maxOf(vw / contentW, vh / contentH)
        val matrix = Matrix()
        matrix.setScale(-(fill * contentW / vw), -(fill * contentH / vh), vw / 2f, vh / 2f)
        binding.textureView.setTransform(matrix)
    }

    // ── Touch handling ───────────────────────────────────────────────────────

    private fun handleTouch(event: MotionEvent) {
        val sender = touchSender

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
                            // Tap: toggle HUD
                            binding.controlPanel.visibility =
                                if (binding.controlPanel.visibility == View.VISIBLE) View.GONE
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
                    // Characters added
                    curr.substring(prev.length).forEach { c ->
                        touchSender?.sendKeyChar(c)
                    }
                } else if (curr.length < prev.length) {
                    // Backspace(s)
                    repeat(prev.length - curr.length) {
                        touchSender?.sendKeyVk(TouchSender.WinVK.BACK, true)
                        touchSender?.sendKeyVk(TouchSender.WinVK.BACK, false)
                    }
                }
                prevInputText = curr
            }
        })

        // Capture special keys (Enter, Tab, Arrows, Escape) before IME consumes them
        binding.hiddenInput.setOnKeyListener { _, keyCode, event ->
            val vk = androidToWinVk(keyCode) ?: return@setOnKeyListener false
            val down = event.action == KeyEvent.ACTION_DOWN
            touchSender?.sendKeyVk(vk, down)
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
        binding.btnKeyboard.alpha = 1f
    }

    private fun hideKeyboard() {
        keyboardVisible = false
        val imm = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager
        imm.hideSoftInputFromWindow(binding.hiddenInput.windowToken, 0)
        binding.btnKeyboard.alpha = 0.5f
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
        val vw = binding.textureView.width.toFloat()
        val vh = binding.textureView.height.toFloat()
        if (vw == 0f || vh == 0f) return Pair(0f, 0f)
        return Pair((tx / vw).coerceIn(0f, 1f), (ty / vh).coerceIn(0f, 1f))
    }

    private fun toScreenPosition(nx: Float, ny: Float): Pair<Float, Float> {
        val vw = binding.textureView.width.toFloat()
        val vh = binding.textureView.height.toFloat()
        return Pair(nx * vw, ny * vh)
    }

    private fun moveCursorTo(p: Pair<Float, Float>) =
        binding.cursorOverlay.moveTo(p.first, p.second)

    // ── TextureView listener ──────────────────────────────────────────────────

    override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
        runOnUiThread { binding.btnConnect.isEnabled = true }
    }

    override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {
        runOnUiThread { applyFillTransform() }
    }

    override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
        stopReceiver()
        runOnUiThread { binding.btnConnect.isEnabled = false }
        return true
    }

    override fun onSurfaceTextureUpdated(st: SurfaceTexture) { framesDecoded++ }

    // ── Helpers ───────────────────────────────────────────────────────────────

    private fun getLocalIp(): String = try {
        NetworkInterface.getNetworkInterfaces()
            .asSequence()
            .flatMap { it.inetAddresses.asSequence() }
            .filterIsInstance<Inet4Address>()
            .filterNot { it.isLoopbackAddress }
            .firstOrNull()?.hostAddress ?: "unknown"
    } catch (_: Exception) { "unknown" }

    override fun onDestroy() {
        super.onDestroy()
        statsHandler.removeCallbacks(statsRunnable)
        stopReceiver()
    }
}
