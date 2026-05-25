package com.pocketdisplay.app

import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.Executors

/**
 * Sends touch and keyboard events to the Windows host.
 *
 * WiFi mode: UDP datagrams to windowsIp:7778
 * USB mode:  TCP connection to localhost:7778 (adb reverse tcp:7778 tcp:7778)
 *
 * Packet layout (16 bytes, big-endian):
 *   [0-3]  'P','D','T','I'
 *   [4]    type: MOVE=0 DOWN=1 UP=2 RIGHT_CLICK=3 SCROLL=4
 *                KEY_CHAR=5  KEY_DOWN=6  KEY_UP=7
 *   [5-7]  reserved
 *   [8-11] payload: float nx / uint32 unicode codepoint / uint16 VK code
 *   [12-15] payload: float ny / padding
 */
open class TouchSender(
    targetIp: String,
    private val port: Int = 7778,
    private val useTcp: Boolean = false
) {
    enum class EventType(val code: Byte) {
        MOVE(0), DOWN(1), UP(2), RIGHT_CLICK(3), SCROLL(4)
    }

    // Windows virtual key codes for special keys
    object WinVK {
        const val BACK        = 0x08
        const val TAB         = 0x09
        const val RETURN      = 0x0D
        const val ESCAPE      = 0x1B
        const val PRIOR       = 0x21  // Page Up
        const val NEXT        = 0x22  // Page Down
        const val END         = 0x23
        const val HOME        = 0x24
        const val LEFT        = 0x25
        const val UP          = 0x26
        const val RIGHT       = 0x27
        const val DOWN        = 0x28
        const val DELETE      = 0x2E
        const val SHIFT       = 0x10
        const val CONTROL     = 0x11
        const val MENU        = 0x12  // Alt
    }

    private val udpSocket    = if (!useTcp) DatagramSocket() else null
    private val targetAddr   = if (!useTcp) InetAddress.getByName(targetIp) else null
    @Volatile private var tcpSocket: Socket? = null
    private val executor     = Executors.newSingleThreadExecutor()
    // ACK may arrive before the touch socket connects; buffer it and flush on connect.
    @Volatile private var pendingAck  = false
    @Volatile private var closed      = false
    private var connectThread: Thread? = null

    init {
        if (useTcp) {
            // IMPORTANT: run the connect loop in a dedicated daemon thread, NOT inside
            // executor.submit().  The executor is a single-thread pool; submitting an
            // infinite retry loop to it permanently blocks the queue — sendAck() and
            // all touch/keyboard tasks would wait forever and never execute.
            val t = Thread({
                val addr = InetSocketAddress(InetAddress.getByName(targetIp), port)
                Log.i("PocketDisplay", "[DBG#16] TouchSender connect thread started -> $targetIp:$port")
                var attempt = 0
                while (!closed) {
                    if (tcpSocket != null) {
                        // Socket appears healthy; wake every 500 ms to re-check.
                        try { Thread.sleep(500) } catch (_: InterruptedException) { break }
                        continue
                    }
                    var s: Socket? = null
                    try {
                        attempt++
                        Log.i("PocketDisplay", "[DBG#16] Touch TCP connect attempt #$attempt -> $targetIp:$port")
                        s = Socket()
                        s.tcpNoDelay = true
                        s.connect(addr, 600)
                        // Probe write detects phantom adb-reverse connections (PC port not
                        // yet open).  SCROLL(0,0) is a no-op on Windows.
                        Log.i("PocketDisplay", "[DBG#16] Touch TCP connected, probing attempt #$attempt")
                        s.getOutputStream().write(buildTouchPacket(EventType.SCROLL.code, 0f, 0f))
                        tcpSocket = s
                        Log.i("PocketDisplay", "[DBG#16] Touch TCP probe OK — connection #$attempt")
                        executor.submit {
                            if (pendingAck) {
                                pendingAck = false
                                rawSend(buildAckPacket())
                            }
                        }
                        // Do NOT break — keep looping so we detect future disconnections.
                    } catch (e: Exception) {
                        Log.d("PocketDisplay", "[DBG#16] Touch TCP attempt #$attempt failed: ${e.message}")
                        try { s?.close() } catch (_: Exception) {}
                        try { Thread.sleep(200) } catch (_: InterruptedException) { break }
                    }
                }
                Log.i("PocketDisplay", "[DBG#16] TouchSender connect thread exiting (closed=$closed)")
            }, "TouchSender-Connect")
            t.isDaemon = true
            t.start()
            connectThread = t
        }
    }

    // ── Touch events ─────────────────────────────────────────────────────────

    fun send(type: EventType, nx: Float, ny: Float) {
        executor.submit { rawSend(buildTouchPacket(type.code, nx, ny)) }
    }

    private fun buildTouchPacket(type: Byte, nx: Float, ny: Float): ByteArray {
        val buf = ByteBuffer.allocate(16).order(ByteOrder.BIG_ENDIAN)
        buf.put('P'.code.toByte()).put('D'.code.toByte())
           .put('T'.code.toByte()).put('I'.code.toByte())
        buf.put(type)
        buf.put(0); buf.put(0); buf.put(0)
        buf.putFloat(nx)
        buf.putFloat(ny)
        return buf.array()
    }

    // ── Keyboard events ───────────────────────────────────────────────────────

    /** Send a printable Unicode character (type 5). */
    fun sendKeyChar(c: Char) {
        val cp = c.code
        if (cp == 0) return
        executor.submit {
            val buf = ByteBuffer.allocate(16).order(ByteOrder.BIG_ENDIAN)
            buf.put('P'.code.toByte()).put('D'.code.toByte())
               .put('T'.code.toByte()).put('I'.code.toByte())
            buf.put(5.toByte())                   // KEY_CHAR
            buf.put(0); buf.put(0); buf.put(0)    // reserved
            buf.putInt(cp)                        // bytes [8-11]: codepoint
            buf.putInt(0)                         // bytes [12-15]: padding
            rawSend(buf.array())
        }
    }

    private fun buildAckPacket(): ByteArray {
        val buf = ByteBuffer.allocate(16).order(ByteOrder.BIG_ENDIAN)
        buf.put('P'.code.toByte()).put('D'.code.toByte())
           .put('T'.code.toByte()).put('I'.code.toByte())
        buf.put(8.toByte())                   // CODEC_READY_ACK
        buf.put(0); buf.put(0); buf.put(0)    // reserved
        buf.putLong(0)                        // bytes [8-15]: padding
        return buf.array()
    }

    /** Notify Windows that Android's codec is ready (type 8). */
    fun sendAck() {
        executor.submit {
            val sock = tcpSocket
            Log.i("PocketDisplay", "[ACK] sendAck: useTcp=$useTcp tcpSocket=${if (sock != null) "connected" else "null"}")
            if (!useTcp || sock != null) {
                // WiFi (UDP): socket always ready — send immediately.
                // USB (TCP): socket already connected — send directly.
                rawSend(buildAckPacket())
            } else {
                // USB TCP touch socket not yet connected; buffer and flush when it connects.
                Log.i("PocketDisplay", "[ACK] sendAck: buffered as pendingAck")
                pendingAck = true
            }
        }
    }

    /** Send a Windows virtual key press or release (types 6/7). */
    fun sendKeyVk(vk: Int, down: Boolean) {
        executor.submit {
            val buf = ByteBuffer.allocate(16).order(ByteOrder.BIG_ENDIAN)
            buf.put('P'.code.toByte()).put('D'.code.toByte())
               .put('T'.code.toByte()).put('I'.code.toByte())
            buf.put(if (down) 6.toByte() else 7.toByte())
            buf.put(0); buf.put(0); buf.put(0)    // reserved
            buf.putShort(vk.toShort())            // bytes [8-9]: VK code
            buf.putShort(0)                       // bytes [10-11]: padding
            buf.putInt(0)                         // bytes [12-15]: padding
            rawSend(buf.array())
        }
    }

    // ── Transport ─────────────────────────────────────────────────────────────

    private fun rawSend(data: ByteArray) {
        try {
            if (useTcp) {
                tcpSocket?.getOutputStream()?.write(data)
            } else {
                udpSocket?.send(DatagramPacket(data, data.size, targetAddr, port))
            }
        } catch (e: Exception) {
            if (useTcp) {
                Log.w("PocketDisplay", "[ACK] rawSend TCP failed: ${e.message}")
                // Mark socket as dead so the connect thread reconnects.
                val dead = tcpSocket; tcpSocket = null
                try { dead?.close() } catch (_: Exception) {}
            }
        }
    }

    fun close() {
        closed = true
        connectThread?.interrupt()
        executor.shutdown()
        udpSocket?.close()
        tcpSocket?.close()
    }
}
