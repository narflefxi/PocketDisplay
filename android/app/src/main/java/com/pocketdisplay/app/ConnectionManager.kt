package com.pocketdisplay.app

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Surface
import java.net.InetSocketAddress

/**
 * Owns all connection state for PocketDisplay.
 *
 * Transport preference: probe USB (127.0.0.1:7777) first; fall back to WiFi discovery.
 * USB plug/unplug: detected via android.hardware.usb.action.USB_STATE broadcast.
 * Fallback re-check: 10-second interval, only when no active session.
 * Mode persistence: saved in SharedPreferences. Dialog shown only when no saved mode exists.
 */
class ConnectionManager(private val context: Context) {

    companion object {
        private const val TAG              = "PocketDisplay"
        private const val PREF_NAME        = "pocketdisplay"
        private const val PREF_DISPLAY_MODE = "display_mode"
        private const val VIDEO_PORT       = 7777
        private const val TOUCH_PORT       = 7778
    }

    // ── Observable callbacks (invoked on main thread) ─────────────────────────

    var onStatus: ((String) -> Unit)? = null
    var onTransport: ((String) -> Unit)? = null         // "USB" | "Wi-Fi" | "—"
    var onConnected: ((Boolean) -> Unit)? = null
    var onExtendedBadge: ((Boolean) -> Unit)? = null
    var onDisplayMode: ((String) -> Unit)? = null       // "mirror" | "extend"
    var onNeedModeDialog: ((hostLabel: String) -> Unit)? = null

    // Video pipeline callbacks forwarded from TcpStreamReceiver
    var onVideoDimensions: ((Int, Int) -> Unit)? = null
    var onWindowsSize: ((Int, Int) -> Unit)? = null
    var onCursorPos: ((Float, Float, Int) -> Unit)? = null
    var onFirstFrame: (() -> Unit)? = null
    var onStreamMode: ((Int) -> Unit)? = null

    // ── Session state ──────────────────────────────────────────────────────────

    private enum class Transport { NONE, USB, WIFI }

    @Volatile private var currentTransport = Transport.NONE
    @Volatile private var destroyed = false

    private var surface: Surface? = null
    private var screenW = 0
    private var screenH = 0

    private var receiver: TcpStreamReceiver? = null
    var touchSender: TouchSender? = null
        private set

    private var discoverClient: DiscoveryClient? = null
    @Volatile private var discoveredHostIp: String? = null

    /** True when a TcpStreamReceiver is active (connecting or streaming). */
    val isSessionActive: Boolean get() = receiver != null

    // ── Preferences ───────────────────────────────────────────────────────────

    private val prefs = context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)

    val savedDisplayMode: String? get() = prefs.getString(PREF_DISPLAY_MODE, null)
    val displayModeName: String    get() = savedDisplayMode ?: "mirror"

    fun saveDisplayMode(mode: String) {
        prefs.edit().putString(PREF_DISPLAY_MODE, mode).apply()
        mainHandler.post { onDisplayMode?.invoke(mode) }
    }

    // ── USB broadcast receiver ─────────────────────────────────────────────────

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context, intent: Intent) {
            Log.i(TAG, "[CM] USB broadcast: ${intent.action}")
            if (intent.action == "android.hardware.usb.action.USB_STATE") {
                val connected = intent.getBooleanExtra("connected", false)
                if (connected) onUsbConnected() else onUsbDisconnected()
            }
        }
    }
    private var usbReceiverRegistered = false

    // ── Fallback poll (10 s, only when no active session) ─────────────────────

    private val mainHandler = Handler(Looper.getMainLooper())

    private val fallbackPollRunnable = object : Runnable {
        override fun run() {
            if (receiver == null && surface != null) {
                Log.d(TAG, "[CM] Fallback poll: probing…")
                Thread { tryConnect() }.start()
            }
            mainHandler.postDelayed(this, 10_000)
        }
    }

    // ── ACK retry (cancelled on first frame) ──────────────────────────────────

    private var codecAckRunnable: Runnable? = null

    // ── Lifecycle ──────────────────────────────────────────────────────────────

    fun onSurfaceAvailable(surface: Surface, w: Int, h: Int) {
        this.surface = surface
        this.screenW = w
        this.screenH = h
        Log.i(TAG, "[CM] Surface available ${w}x${h}")
        Thread { tryConnect() }.start()
    }

    fun onSurfaceDestroyed() {
        Log.i(TAG, "[CM] Surface destroyed")
        surface = null
        stopCurrentSession()
    }

    fun onResume() {
        if (!usbReceiverRegistered) {
            val filter = IntentFilter("android.hardware.usb.action.USB_STATE")
            context.registerReceiver(usbReceiver, filter)
            usbReceiverRegistered = true
            Log.d(TAG, "[CM] USB BroadcastReceiver registered")
        }
        if (receiver == null && surface != null) {
            Thread { tryConnect() }.start()
        }
        mainHandler.removeCallbacks(fallbackPollRunnable)
        mainHandler.postDelayed(fallbackPollRunnable, 10_000)
    }

    fun onPause() {
        // Keep streaming through pause (notification shade, screen rotation, etc.)
    }

    fun destroy() {
        Log.i(TAG, "[CM] destroy()")
        destroyed = true
        mainHandler.removeCallbacks(fallbackPollRunnable)
        codecAckRunnable?.let { mainHandler.removeCallbacks(it) }
        if (usbReceiverRegistered) {
            try { context.unregisterReceiver(usbReceiver) } catch (_: Exception) {}
            usbReceiverRegistered = false
        }
        stopCurrentSession()
    }

    // ── Transport selection ────────────────────────────────────────────────────

    /** Probe USB then WiFi to pick a transport. Must be called off the main thread. */
    private fun tryConnect() {
        val usbOk = probeHost("127.0.0.1")
        mainHandler.post {
            if (receiver != null || surface == null) return@post
            if (usbOk) {
                Log.i(TAG, "[CM] USB reachable — starting USB session")
                startUsbSession()
            } else {
                val wifiIp = discoveredHostIp
                if (wifiIp != null) {
                    Log.i(TAG, "[CM] USB not reachable, known WiFi host $wifiIp — reconnecting")
                    startWifiSession(wifiIp)
                } else {
                    Log.i(TAG, "[CM] USB not reachable — starting WiFi discovery")
                    startWifiDiscovery()
                }
            }
        }
    }

    /** Public entry point for manual retries (e.g. Connect button). */
    fun retryConnect() {
        if (receiver != null || surface == null) return
        Thread { tryConnect() }.start()
    }

    private fun onUsbConnected() {
        Log.i(TAG, "[CM] USB attached — probing (will retry up to 5 s)…")
        Thread {
            var usbOk = false
            val deadline = System.currentTimeMillis() + 5_000L
            var attempt = 0
            while (!usbOk && System.currentTimeMillis() < deadline) {
                attempt++
                usbOk = probeHost("127.0.0.1")
                Log.i(TAG, "[CM] USB probe attempt $attempt: ${if (usbOk) "reachable" else "not reachable"}")
                if (!usbOk && System.currentTimeMillis() < deadline) {
                    try { Thread.sleep(1_000) } catch (_: InterruptedException) { break }
                }
            }
            mainHandler.post {
                if (!usbOk || surface == null) {
                    if (!usbOk) Log.i(TAG, "[CM] USB probe failed after $attempt attempt(s) — staying on current transport")
                    return@post
                }
                if (currentTransport != Transport.USB) {
                    Log.i(TAG, "[CM] USB reachable after attach — switching to USB")
                    stopCurrentSession()
                    startUsbSession()
                }
            }
        }.start()
    }

    private fun onUsbDisconnected() {
        mainHandler.post {
            Log.i(TAG, "[CM] USB detached")
            if (currentTransport == Transport.USB) {
                stopCurrentSession()
                startWifiDiscovery()
            }
        }
    }

    // ── Session management ─────────────────────────────────────────────────────

    private fun startUsbSession() {
        val surf = surface ?: return
        Log.i(TAG, "[CM] Starting USB session (mode=${savedDisplayMode ?: "ask"})")
        currentTransport = Transport.USB
        onTransport?.invoke("USB")
        onStatus?.invoke("USB: connecting to Windows…")

        val savedMode = savedDisplayMode
        receiver = TcpStreamReceiver(
            surface          = surf,
            host             = "127.0.0.1",
            port             = VIDEO_PORT,
            onStatus         = { s -> mainHandler.post { onStatus?.invoke(s) } },
            onDimensions     = { w, h -> mainHandler.post { onVideoDimensions?.invoke(w, h) } },
            onSenderIp       = { ip -> mainHandler.post { onSenderIpReceived(ip) } },
            onWindowsSize    = { w, h -> mainHandler.post { onWindowsSize?.invoke(w, h) } },
            onCursorPos      = { nx, ny, t -> onCursorPos?.invoke(nx, ny, t) },
            onCodecConfigured = { mainHandler.post { onCodecConfiguredInternal() } },
            onFirstFrame     = { mainHandler.post { onFirstFrameInternal() } },
            onMode           = { flags -> mainHandler.post { onStreamMode?.invoke(flags) } },
            onWindowsReady   = if (savedMode == null) {
                { mainHandler.post { onNeedModeDialog?.invoke("USB device") } }
            } else null,
            modeToSend       = savedMode,
            screenW          = screenW,
            screenH          = screenH
        ).also { it.start() }
    }

    private fun startWifiDiscovery() {
        onTransport?.invoke("Wi-Fi")  // Always update label, even if discovery is already running
        if (discoverClient?.isRunning == true) return
        Log.i(TAG, "[CM] Starting WiFi discovery")
        onStatus?.invoke("Searching for PocketDisplay host…")

        discoverClient?.stop()
        discoverClient = DiscoveryClient { hostIp, _ ->
            discoveredHostIp = hostIp
            mainHandler.post {
                discoverClient?.stop()
                discoverClient = null
                if (receiver == null && surface != null) {
                    Log.i(TAG, "[CM] Host discovered: $hostIp")
                    startWifiSession(hostIp)
                }
            }
        }.also { it.start() }
    }

    private fun startWifiSession(hostIp: String) {
        val surf = surface ?: return
        Log.i(TAG, "[CM] Starting WiFi session to $hostIp (mode=${savedDisplayMode ?: "ask"})")
        currentTransport = Transport.WIFI
        onTransport?.invoke("Wi-Fi")
        onStatus?.invoke("Connecting to $hostIp…")

        val savedMode = savedDisplayMode
        receiver = TcpStreamReceiver(
            surface          = surf,
            host             = hostIp,
            port             = VIDEO_PORT,
            onStatus         = { s -> mainHandler.post { onStatus?.invoke(s) } },
            onDimensions     = { w, h -> mainHandler.post { onVideoDimensions?.invoke(w, h) } },
            onSenderIp       = { ip -> mainHandler.post { onSenderIpReceived(ip) } },
            onWindowsSize    = { w, h -> mainHandler.post { onWindowsSize?.invoke(w, h) } },
            onCursorPos      = { nx, ny, t -> onCursorPos?.invoke(nx, ny, t) },
            onCodecConfigured = { mainHandler.post { onCodecConfiguredInternal() } },
            onFirstFrame     = { mainHandler.post { onFirstFrameInternal() } },
            onMode           = { flags -> mainHandler.post { onStreamMode?.invoke(flags) } },
            onWindowsReady   = if (savedMode == null) {
                { mainHandler.post { onNeedModeDialog?.invoke("Host: $hostIp") } }
            } else null,
            modeToSend       = savedMode,
            screenW          = screenW,
            screenH          = screenH
        ).also { it.start() }
    }

    fun stopCurrentSession() {
        Log.i(TAG, "[CM] Stopping current session (transport=$currentTransport)")
        codecAckRunnable?.let { mainHandler.removeCallbacks(it) }
        codecAckRunnable = null
        discoverClient?.stop(); discoverClient = null
        receiver?.stop(); receiver = null
        touchSender?.close(); touchSender = null
        currentTransport = Transport.NONE
        onConnected?.invoke(false)
        onExtendedBadge?.invoke(false)
        onStatus?.invoke("Disconnected")
        // Transport label intentionally NOT reset — keeps last-known transport visible on dashboard.
        // Trigger an immediate reconnect attempt instead of waiting for the 10 s fallback poll.
        mainHandler.post {
            if (!destroyed && receiver == null && surface != null) Thread { tryConnect() }.start()
        }
    }

    // ── Session event handlers ─────────────────────────────────────────────────

    private fun onSenderIpReceived(ip: String) {
        // Re-confirm transport label on every TCP stream connect (covers reconnects and edge cases).
        if (currentTransport != Transport.NONE)
            onTransport?.invoke(if (currentTransport == Transport.USB) "USB" else "Wi-Fi")

        if (touchSender != null) return  // already created (auto-reconnect within same session)
        val touchHost = if (currentTransport == Transport.USB) "127.0.0.1" else ip
        Log.i(TAG, "[CM] Creating TouchSender → $touchHost:$TOUCH_PORT")
        touchSender = TouchSender(touchHost, port = TOUCH_PORT)
        onConnected?.invoke(true)
        val label = if (currentTransport == Transport.USB) "USB" else "Wi-Fi ↔ $ip"
        onStatus?.invoke("Connected via $label")
    }

    private fun onCodecConfiguredInternal() {
        codecAckRunnable?.let { mainHandler.removeCallbacks(it) }
        val r = object : Runnable {
            override fun run() {
                if (receiver == null) return
                touchSender?.sendAck()
                mainHandler.postDelayed(this, 1000)
            }
        }
        codecAckRunnable = r
        mainHandler.post(r)
    }

    private fun onFirstFrameInternal() {
        codecAckRunnable?.let { mainHandler.removeCallbacks(it) }
        codecAckRunnable = null
        onFirstFrame?.invoke()
    }

    // ── Mode handling ──────────────────────────────────────────────────────────

    /** Called by MainActivity when the user selects a mode from the dialog. */
    fun onModeSelected(mode: String) {
        saveDisplayMode(mode)
        receiver?.setPendingMode(mode)
        Log.i(TAG, "[CM] Mode selected: $mode")
    }

    /** Toggle saved display mode (mirror ↔ extend). Applies to the next connection. */
    fun toggleDisplayMode() {
        val next = if (savedDisplayMode == "extend") "mirror" else "extend"
        saveDisplayMode(next)
        Log.i(TAG, "[CM] Display mode toggled to: $next")
    }

    // ── Helpers ────────────────────────────────────────────────────────────────

    private fun probeHost(host: String, timeoutMs: Int = 300): Boolean =
        try {
            java.net.Socket().use { s ->
                s.connect(InetSocketAddress(host, VIDEO_PORT), timeoutMs)
                true
            }
        } catch (_: Exception) { false }
}
