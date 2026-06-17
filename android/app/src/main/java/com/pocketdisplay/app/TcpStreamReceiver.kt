package com.pocketdisplay.app

import android.util.Log
import android.view.Surface
import java.io.DataInputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.concurrent.thread

class TcpStreamReceiver(
    surface: Surface,
    private val host: String = "127.0.0.1",
    private val port: Int,
    private val onStatus: (String) -> Unit,
    private val onDimensions: ((Int, Int) -> Unit)? = null,
    // Protocol v2: session_id is passed along with sender IP for ACK validation
    private val onSenderIp: ((String, Int) -> Unit)? = null,
    private val onWindowsSize: ((Int, Int) -> Unit)? = null,
    private val onCursorPos: ((Float, Float, Int) -> Unit)? = null,
    onCodecConfigured: (() -> Unit)? = null,
    onFirstFrame: (() -> Unit)? = null,
    private val onMode: ((Int) -> Unit)? = null,
    // Called (on the receiver thread) when TCP connects and Windows is confirmed ready to receive
    // mode. If null, modeToSend is sent immediately after connect (legacy / reconnect path).
    private val onWindowsReady: (() -> Unit)? = null,
    private val modeToSend: String? = null,
    private val screenW: Int = 0,
    private val screenH: Int = 0
) {
    companion object {
        private const val TAG = "PocketDisplay"
        private const val MAX_MESSAGE = 8 * 1024 * 1024
    }

    @Volatile var isRunning = false
    private var netThread: Thread? = null
    @Volatile private var activeSocket: Socket? = null
    private val decoder = VideoDecoder(surface, onStatus, onDimensions, onCodecConfigured, onFirstFrame)

    private var videoPackets = 0L
    private var streamW = 0
    private var streamH = 0

    // Set by MainActivity after the user taps the mode dialog (setPendingMode).
    // Kept non-null after being read so reconnects can resend without re-showing the dialog.
    @Volatile private var pendingMode: String? = null

    // Set after mode is successfully written to the socket.
    // On reconnect (same TcpStreamReceiver object) this is reused directly,
    // bypassing onWindowsReady so the dialog is not shown again.
    @Volatile private var committedMode: String? = null

    // True once mode has been sent at least once in this receiver's lifetime.
    // Used to distinguish a fresh connection (show dialog) from a within-session
    // reconnect (reuse committedMode without re-showing dialog).
    @Volatile private var sessionStarted = false

    /** Called by MainActivity when the user taps Mirror or Extended. */
    fun setPendingMode(mode: String) {
        pendingMode = mode
    }

    fun start() {
        if (isRunning) return
        isRunning = true
        Log.i(TAG, "TcpStreamReceiver host=$host port=$port")
        netThread = thread(name = "TcpStreamReceiver", isDaemon = true) {
            // Counts failed connect attempts before the first successful mode send.
            var connectAttempt = 0
            var modeEverSent = false

            // Outer reconnect loop: keeps retrying after disconnects.
            while (isRunning) {
                // New session (stopReceiver was called, new receiver created): ensure
                // mode state is fresh so the dialog is shown.  After the first mode
                // send, sessionStarted=true and committedMode is reused on reconnect.
                if (!sessionStarted) {
                    committedMode = null
                    // pendingMode is intentionally NOT cleared here.  If the user
                    // already tapped the mode dialog before the connection dropped
                    // (e.g. Windows' 5 s RCVTIMEO fired), their choice is preserved
                    // so the next connect attempt sends HELLO immediately without
                    // re-showing the dialog.  pendingMode is null by default in a
                    // freshly constructed TcpStreamReceiver, so new sessions always
                    // start clean regardless of this path.
                }

                var socket: Socket? = null
                try {
                    // Connect (retry until success or stopped).
                    while (isRunning && socket == null) {
                        try {
                            val s = Socket()
                            s.tcpNoDelay = true
                            s.connect(InetSocketAddress(host, port), 1500)
                            socket = s
                        } catch (_: Exception) {
                            if (!modeEverSent && modeToSend != null) {
                                // Legacy path: slow retry with user-visible progress.
                                connectAttempt++
                                Log.i(TAG, "[MODE] USB send failed, retrying in 2s (attempt $connectAttempt/30)")
                                onStatus("Connecting to Windows… (attempt $connectAttempt/30)")
                                try { Thread.sleep(2000) } catch (_: InterruptedException) {}
                            } else {
                                try { Thread.sleep(300) } catch (_: InterruptedException) {}
                            }
                        }
                    }
                    if (socket == null) break

                    connectAttempt = 0
                    activeSocket = socket
                    streamW = 0; streamH = 0

                    Log.i(TAG, "TCP connected to $host:$port")

                    // Determine which mode to send.
                    //
                    // Priority order:
                    //   1. modeToSend != null  → caller already knows the mode (legacy / reconnect
                    //      with known mode passed at construction); send immediately.
                    //   2. committedMode != null → same TcpStreamReceiver object reconnecting after
                    //      the mode was already sent once; reuse it without re-showing the dialog.
                    //   3. onWindowsReady path  → first connect with no predetermined mode:
                    //      notify MainActivity so it shows the dialog, then wait for the user to tap
                    //      (via setPendingMode).  Windows's AcceptLoop gives us 5 s before timeout,
                    //      but the dialog appears immediately on TCP connect so the user taps within
                    //      1–2 s.  If the connection times out before the tap, the outer loop
                    //      reconnects; pendingMode persists so the next connection sends immediately.
                    val modeActual: String? = when {
                        modeToSend != null -> modeToSend

                        committedMode != null -> {
                            Log.i(TAG, "[MODE] reconnect — reusing committedMode='$committedMode'")
                            committedMode
                        }

                        else -> {
                            if (pendingMode == null) {
                                // First time: ask MainActivity to show the dialog.
                                Log.i(TAG, "[MODE] TCP connected — firing onWindowsReady for mode dialog")
                                onWindowsReady?.invoke()
                                onStatus("Select display mode…")
                            } else {
                                // Reconnect while dialog was pending (e.g. 5 s Windows timeout
                                // fired before user tapped); pendingMode already set, skip dialog.
                                Log.i(TAG, "[MODE] TCP reconnected — pendingMode already set, sending immediately")
                            }
                            // Wait up to 120 s for user tap (matches Windows WaitForMode timeout).
                            val deadline = System.currentTimeMillis() + 120_000L
                            while (pendingMode == null && isRunning &&
                                   System.currentTimeMillis() < deadline) {
                                try { Thread.sleep(100) } catch (_: InterruptedException) {}
                            }
                            pendingMode  // null on timeout or stop
                        }
                    }

                    if (modeActual != null) {
                        // Send framed HELLO: [4-byte BE len=11][type=4][version=1][mode][w32BE][h32BE]
                        val modeVal  = if (modeActual == "extend") 1.toByte() else 0.toByte()
                        val helloLen = 11  // type(1)+version(1)+mode(1)+w(4)+h(4)
                        val frame    = ByteBuffer.allocate(4 + helloLen).order(ByteOrder.BIG_ENDIAN)
                        frame.putInt(helloLen)   // length prefix
                        frame.put(4.toByte())    // type = kHello
                        frame.put(1.toByte())    // protocol_version = 1
                        frame.put(modeVal)       // mode: 0=mirror, 1=extend
                        frame.putInt(screenW)    // android_screen_w
                        frame.putInt(screenH)    // android_screen_h
                        Log.i(TAG, "[MODE] TCP HELLO: sending mode=$modeActual screen=${screenW}x${screenH} host=$host")
                        socket.getOutputStream().write(frame.array())
                        socket.getOutputStream().flush()
                        // Remember for reconnects; keep pendingMode non-null too so that if the
                        // write succeeded but the socket drops before Windows reads it, the next
                        // reconnect can resend without re-showing the dialog.
                        committedMode = modeActual
                        sessionStarted = true
                        modeEverSent = true
                        Log.i(TAG, "[MODE] TCP HELLO: sent OK")
                    }

                    // Notify touch-sender setup now that mode is settled.
                    // Session_id will be updated when we receive stream_info (type 2).
                    onSenderIp?.invoke(host, 0)  // 0 = unknown until stream_info arrives

                    val input = DataInputStream(socket.getInputStream())
                    val lenBuf = ByteArray(4)

                    while (isRunning) {
                        input.readFully(lenBuf)
                        val msgLen = ByteBuffer.wrap(lenBuf).order(ByteOrder.BIG_ENDIAN).int
                        if (msgLen < 1 || msgLen > MAX_MESSAGE) {
                            Log.e(TAG, "Invalid message length: $msgLen")
                            break
                        }
                        val payload = ByteArray(msgLen)
                        input.readFully(payload)
                        val type = payload[0].toInt() and 0xFF
                        val body = if (msgLen > 1) payload.copyOfRange(1, msgLen) else ByteArray(0)

                        when (type) {
                            1 -> {
                                Log.i(TAG, "Codec config received (stream=${streamW}x${streamH})")
                                decoder.configure(body, streamW, streamH)
                            }
                            2 -> {
                                // Protocol v2: stream_info is 16 bytes: w, h, flags, session_id(2), padding(2)
                                if (body.size >= 8) {
                                    val bb = ByteBuffer.wrap(body).order(ByteOrder.BIG_ENDIAN)
                                    val w = bb.int; val h = bb.int
                                    if (w > 0 && h > 0) {
                                        streamW = w; streamH = h
                                        onWindowsSize?.invoke(w, h)
                                    }
                                    if (body.size >= 12) onMode?.invoke(bb.int)
                                    // Protocol v2: extract session_id from bytes 12-13 (big-endian uint16)
                                    if (body.size >= 14) {
                                        val sessionId = bb.short.toInt() and 0xFFFF
                                        // Re-invoke onSenderIp with the session_id for ACK echoing
                                        onSenderIp?.invoke(host, sessionId)
                                        if (sessionId != 0) {
                                            Log.i(TAG, "[SESSION] Received session_id=$sessionId from stream_info")
                                        }
                                    }
                                }
                            }
                            3 -> {
                                if (body.size >= 8) {
                                    val bb = ByteBuffer.wrap(body).order(ByteOrder.BIG_ENDIAN)
                                    val nx = bb.float; val ny = bb.float
                                    val cursorType = if (body.size >= 9) (body[8].toInt() and 0xFF) else 0
                                    onCursorPos?.invoke(nx, ny, cursorType)
                                }
                            }
                            0 -> {
                                videoPackets++
                                val kf = isLikelyKeyframe(body)
                                decoder.decode(body, kf)
                            }
                            else -> Log.w(TAG, "Unknown type=$type len=$msgLen")
                        }
                    }
                } catch (e: Exception) {
                    if (isRunning) Log.w(TAG, "TCP read error: ${e.message}")
                } finally {
                    activeSocket = null
                    try { socket?.close() } catch (_: Exception) {}
                    decoder.release()
                }
                if (isRunning) {
                    onStatus("Reconnecting…")
                    try { Thread.sleep(500) } catch (_: InterruptedException) {}
                }
            }
        }
    }

    private fun isLikelyKeyframe(data: ByteArray): Boolean {
        var i = 0
        while (i + 4 < data.size) {
            val sc4 = data[i] == 0.toByte() && data[i + 1] == 0.toByte() &&
                data[i + 2] == 0.toByte() && data[i + 3] == 1.toByte()
            val sc3 = !sc4 && data[i] == 0.toByte() && data[i + 1] == 0.toByte() &&
                data[i + 2] == 1.toByte()
            val start = i + if (sc4) 4 else if (sc3) 3 else {
                i++
                continue
            }
            if (start >= data.size) break
            val nalType = data[start].toInt() and 0x1F
            if (nalType == 5) return true
            i = start + 1
        }
        return false
    }

    fun stop() {
        isRunning = false
        try { activeSocket?.close() } catch (_: Exception) {}
        netThread?.interrupt()
        decoder.release()
    }
}
