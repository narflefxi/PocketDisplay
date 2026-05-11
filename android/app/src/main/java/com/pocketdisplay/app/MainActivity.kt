package com.pocketdisplay.app

import android.graphics.Matrix
import android.graphics.SurfaceTexture
import android.os.Bundle
import android.view.MotionEvent
import android.view.Surface
import android.view.TextureView
import android.view.View
import android.view.WindowManager
import androidx.appcompat.app.AppCompatActivity
import com.pocketdisplay.app.databinding.ActivityMainBinding
import java.net.Inet4Address
import java.net.NetworkInterface

class MainActivity : AppCompatActivity(), TextureView.SurfaceTextureListener {

    private lateinit var binding: ActivityMainBinding
    private var receiver: StreamReceiver? = null

    // Decoded video dimensions — set once INFO_OUTPUT_FORMAT_CHANGED fires.
    private var videoW = 0
    private var videoH = 0

    // Touch forwarding — created as soon as we learn the Windows host IP.
    @Volatile private var touchSender: TouchSender? = null

    // Tap detection: if finger lifts close to where it pressed, treat as tap.
    private var touchDownX = 0f
    private var touchDownY = 0f
    private val TAP_SLOP_PX = 20f

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_FULLSCREEN
            or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
            or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.textureView.surfaceTextureListener = this
        binding.btnConnect.isEnabled = false
        binding.tvDeviceIp.text = "This device IP: ${getLocalIp()}"

        binding.btnConnect.setOnClickListener { toggleReceiver() }

        binding.textureView.setOnTouchListener { _, event ->
            handleTouch(event)
            true   // consume so the system doesn't also fire a click
        }
    }

    // ── Streaming control ────────────────────────────────────────────────────

    private fun toggleReceiver() {
        if (receiver?.isRunning == true) stopReceiver() else startReceiver()
    }

    private fun startReceiver() {
        val st = binding.textureView.surfaceTexture ?: return
        val surface = Surface(st)
        receiver = StreamReceiver(
            surface,
            onStatus     = ::updateStatus,
            onDimensions = ::onVideoDimensions,
            onSenderIp   = ::onWindowsIpKnown
        )
        receiver?.start()
        binding.btnConnect.text = "Stop"
        updateStatus("Waiting for stream on port ${StreamReceiver.PORT}…")
    }

    private fun stopReceiver() {
        receiver?.stop()
        receiver = null
        touchSender?.close()
        touchSender = null
        videoW = 0; videoH = 0
        binding.btnConnect.text = "Start Listening"
        updateStatus("Stopped")
    }

    // ── Callbacks from background threads ───────────────────────────────────

    private fun onVideoDimensions(w: Int, h: Int) {
        videoW = w
        videoH = h
        binding.textureView.surfaceTexture?.setDefaultBufferSize(w, h)
        runOnUiThread { applyFillTransform() }
    }

    private fun onWindowsIpKnown(ip: String) {
        // Called once from the receiver thread when the first stream packet arrives.
        if (touchSender == null) {
            touchSender = TouchSender(ip)
            updateStatus("Streaming ↔ touch active")
        }
    }

    private fun updateStatus(msg: String) {
        runOnUiThread { binding.tvStatus.text = msg }
    }

    // ── Fill-mode transform (matches applyFillTransform in the previous fix) ─

    private fun applyFillTransform() {
        if (videoW == 0 || videoH == 0) return
        val vw = binding.textureView.width.toFloat()
        val vh = binding.textureView.height.toFloat()
        if (vw == 0f || vh == 0f) return

        val fill = maxOf(vw / videoW, vh / videoH)
        val matrix = Matrix()
        matrix.setScale(fill * videoW / vw, fill * videoH / vh, vw / 2f, vh / 2f)
        binding.textureView.setTransform(matrix)
    }

    // ── Touch forwarding ────────────────────────────────────────────────────

    private fun handleTouch(event: MotionEvent) {
        val sender = touchSender
        val hasVideo = videoW > 0 && videoH > 0

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                touchDownX = event.x
                touchDownY = event.y
                if (sender != null && hasVideo) {
                    val (nx, ny) = toNormalized(event.x, event.y)
                    sender.send(TouchSender.EventType.DOWN, nx, ny)
                }
            }
            MotionEvent.ACTION_MOVE -> {
                if (sender != null && hasVideo) {
                    val (nx, ny) = toNormalized(event.x, event.y)
                    sender.send(TouchSender.EventType.MOVE, nx, ny)
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (sender != null && hasVideo) {
                    val (nx, ny) = toNormalized(event.x, event.y)
                    sender.send(TouchSender.EventType.UP, nx, ny)
                }
                // Tap = lift close to where we pressed → toggle control panel.
                val dx = event.x - touchDownX
                val dy = event.y - touchDownY
                if (dx * dx + dy * dy < TAP_SLOP_PX * TAP_SLOP_PX) {
                    binding.controlPanel.visibility =
                        if (binding.controlPanel.visibility == View.VISIBLE) View.GONE
                        else View.VISIBLE
                }
            }
        }
    }

    /**
     * Reverse the fill transform to map screen touch → normalized Windows coord.
     *
     * The video is center-cropped: it's scaled by fill = max(vw/videoW, vh/videoH)
     * and centered. We undo that to get [0,1] coords over the actual video content.
     */
    private fun toNormalized(tx: Float, ty: Float): Pair<Float, Float> {
        val vw   = binding.textureView.width.toFloat()
        val vh   = binding.textureView.height.toFloat()
        val fill = maxOf(vw / videoW, vh / videoH)
        val rendW    = videoW * fill
        val rendH    = videoH * fill
        val offsetX  = (vw - rendW) / 2f
        val offsetY  = (vh - rendH) / 2f
        val nx = ((tx - offsetX) / rendW).coerceIn(0f, 1f)
        val ny = ((ty - offsetY) / rendH).coerceIn(0f, 1f)
        return Pair(nx, ny)
    }

    // ── TextureView.SurfaceTextureListener ──────────────────────────────────

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

    override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}

    // ────────────────────────────────────────────────────────────────────────

    private fun getLocalIp(): String {
        return try {
            NetworkInterface.getNetworkInterfaces()
                .asSequence()
                .flatMap { it.inetAddresses.asSequence() }
                .filterIsInstance<Inet4Address>()
                .filterNot { it.isLoopbackAddress }
                .firstOrNull()
                ?.hostAddress ?: "unknown"
        } catch (_: Exception) { "unknown" }
    }

    override fun onDestroy() {
        super.onDestroy()
        stopReceiver()
    }
}
