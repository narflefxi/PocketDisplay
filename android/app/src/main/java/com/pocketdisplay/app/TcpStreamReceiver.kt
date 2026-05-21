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
    private val port: Int,
    private val onStatus: (String) -> Unit,
    private val onDimensions: ((Int, Int) -> Unit)? = null,
    private val onSenderIp: ((String) -> Unit)? = null,
    private val onWindowsSize: ((Int, Int) -> Unit)? = null,
    private val onCursorPos: ((Float, Float, Int) -> Unit)? = null,
    onCodecConfigured: (() -> Unit)? = null,
    onFirstFrame: (() -> Unit)? = null,
    private val onMode: ((Int) -> Unit)? = null,
    private val modeToSend: String? = null
) {
    companion object {
        private const val TAG = "PocketDisplay"
        private const val HOST = "127.0.0.1"
        private const val MAX_MESSAGE = 8 * 1024 * 1024
    }

    @Volatile var isRunning = false
    private var netThread: Thread? = null
    @Volatile private var activeSocket: Socket? = null
    private val decoder = VideoDecoder(surface, onStatus, onDimensions, onCodecConfigured, onFirstFrame)

    private var videoPackets = 0L
    // Stream dimensions received from Windows type-2 (stream info) packet;
    // forwarded to decoder.configure() so MediaFormat uses the correct size.
    private var streamW = 0
    private var streamH = 0

    fun start() {
        if (isRunning) return
        isRunning = true
        Log.i(TAG, "Mode=USB host=$HOST transport=TCP port=$port")
        netThread = thread(name = "TcpStreamReceiver", isDaemon = true) {
            // Outer reconnect loop: keeps retrying after disconnects.
            while (isRunning) {
                var socket: Socket? = null
                try {
                    // Connect (retry until success or stopped).
                    while (isRunning && socket == null) {
                        try {
                            val s = Socket()
                            s.tcpNoDelay = true
                            s.connect(InetSocketAddress(HOST, port), 1500)
                            socket = s
                        } catch (_: Exception) {
                            try { Thread.sleep(300) } catch (_: InterruptedException) {}
                        }
                    }
                    if (socket == null) break

                    activeSocket = socket
                    streamW = 0; streamH = 0  // reset dims for fresh connection
                    Log.i(TAG, "TCP connected to $HOST:$port")
                    onSenderIp?.invoke(HOST)

                    // Send mode selection as first message so Windows reads it before streaming.
                    if (modeToSend != null) {
                        Log.i(TAG, "Sending mode: $modeToSend")
                        socket.getOutputStream().write("POCKETDISPLAY_MODE:$modeToSend\n".toByteArray(Charsets.US_ASCII))
                        socket.getOutputStream().flush()
                    }

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
                                if (body.size >= 8) {
                                    val bb = ByteBuffer.wrap(body).order(ByteOrder.BIG_ENDIAN)
                                    val w = bb.int; val h = bb.int
                                    if (w > 0 && h > 0) {
                                        streamW = w; streamH = h
                                        onWindowsSize?.invoke(w, h)
                                    }
                                    if (body.size >= 12) onMode?.invoke(bb.int)
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
                    // Release decoder so it re-configures cleanly on next connect.
                    decoder.release()
                }
                // Brief pause before reconnect attempt.
                if (isRunning) {
                    onStatus("USB: reconnecting\u2026")
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