package com.pocketdisplay.app

import android.util.Log
import android.view.Surface
import java.io.DataInputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.concurrent.thread

/**
 * USB mode: client TCP to 127.0.0.1:[port] (adb reverse to Windows).
 *
 * Wire format per message (big-endian):
 *   [4-byte uint32 length L][L bytes: uint8 type + payload]
 * type 0 = H.264 access unit
 * type 1 = codec config (Annex-B SPS+PPS)
 * type 2 = stream info (8 bytes: w,h uint32 BE)
 * type 3 = cursor (8 bytes: float BE nx, ny)
 */
class TcpStreamReceiver(
    surface: Surface,
    private val port: Int,
    private val onStatus: (String) -> Unit,
    private val onDimensions: ((Int, Int) -> Unit)? = null,
    private val onSenderIp: ((String) -> Unit)? = null,
    private val onWindowsSize: ((Int, Int) -> Unit)? = null,
    private val onCursorPos: ((Float, Float) -> Unit)? = null,
    onCodecConfigured: (() -> Unit)? = null
) {
    companion object {
        private const val TAG = "PocketDisplay"
        private const val HOST = "127.0.0.1"
        private const val MAX_MESSAGE = 8 * 1024 * 1024
    }

    @Volatile var isRunning = false
    private var netThread: Thread? = null
    @Volatile private var activeSocket: Socket? = null
    private val decoder = VideoDecoder(surface, onStatus, onDimensions, onCodecConfigured)

    private var videoPackets = 0L

    fun start() {
        if (isRunning) return
        isRunning = true
        Log.i(TAG, "Mode=USB host=$HOST transport=TCP port=$port")
        netThread = thread(name = "TcpStreamReceiver", isDaemon = true) {
            var socket: Socket? = null
            try {
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
                if (socket == null) return@thread

                activeSocket = socket
                Log.i(TAG, "TCP connected to $HOST:$port")
                onSenderIp?.invoke(HOST)

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

                    when (type) {
                        1 -> {
                            Log.i(TAG, "Decoder started (codec config)")
                            decoder.configure(body)
                        }
                        2 -> {
                            if (body.size >= 8) {
                                val bb = ByteBuffer.wrap(body).order(ByteOrder.BIG_ENDIAN)
                                val w = bb.int
                                val h = bb.int
                                if (w > 0 && h > 0) onWindowsSize?.invoke(w, h)
                            }
                        }
                        3 -> {
                            if (body.size >= 8) {
                                val bb = ByteBuffer.wrap(body).order(ByteOrder.BIG_ENDIAN)
                                onCursorPos?.invoke(bb.float, bb.float)
                            }
                        }
                        0 -> {
                            videoPackets++
                            if (videoPackets == 1L || videoPackets % 120L == 0L) {
                                Log.d(TAG, "Received frame packet (count=$videoPackets, bytes=${body.size})")
                            }
                            val kf = isLikelyKeyframe(body)
                            decoder.decode(body, kf)
                        }
                        else -> Log.w(TAG, "Unknown USB TCP payload type=$type len=$msgLen")
                    }
                }
            } catch (e: Exception) {
                if (isRunning) onStatus("USB TCP error: ${e.message}")
            } finally {
                activeSocket = null
                try { socket?.close() } catch (_: Exception) {}
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
