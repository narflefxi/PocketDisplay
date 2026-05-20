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

    fun start() {
        if (isRunning) return
        isRunning = true
        Log.i(TAG, "Mode=USB host=$HOST transport=TCP port=$port")
        netThread = thread(name = "TcpStreamReceiver", isDaemon = true) {
            while (isRunning) {  // ← OUTER retry loop
                var socket: Socket? = null
                try {
                    // Inner connect retry
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
                    Log.i(TAG, "TCP connected to $HOST:$port")
                    onSenderIp?.invoke(HOST)

                    // Send mode selection
                    if (modeToSend != null) {
                        Log.i(TAG, "Sending mode to Windows: $modeToSend")
                        socket.getOutputStream().write(
                            "POCKETDISPLAY_MODE:$modeToSend\n".toByteArray(Charsets.US_ASCII))
                        socket.getOutputStream().flush()
                        Log.i(TAG, "Mode sent OK")
                    }

                    val input = DataInputStream(socket.getInputStream())
                    val lenBuf = ByteArray(4)

                    while (isRunning) {
                        input.readFully(lenBuf)
                        val msgLen = ByteBuffer.wrap(lenBuf).order(ByteOrder.BIG_ENDIAN).int
                        if (msgLen < 1 || msgLen > MAX_MESSAGE) {
                            Log.e(TAG, "Invalid USB TCP message length: $msgLen")
                            break
                        }
                        val payload = ByteArray(msgLen)
                        input.readFully(payload)
                        val type = payload[0].toInt() and 0xFF
                        val body = if (msgLen > 1) payload.copyOfRange(1, msgLen) else ByteArray(0)
                        Log.d(TAG, "USB TCP received type=$type payload=${body.size}")

                        when (type) {
                            1 -> {
                                Log.i(TAG, "Decoder started (codec config)")
                                decoder.configure(body)
                            }
                            2 -> {
                                if (body.size >= 8) {
                                    val bb = ByteBuffer.wrap(body).order(ByteOrder.BIG_ENDIAN)
                                    val w = bb.int; val h = bb.int
                                    if (w > 0 && h > 0) onWindowsSize?.invoke(w, h)
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