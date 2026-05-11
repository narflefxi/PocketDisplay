package com.pocketdisplay.app

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.Executors

/**
 * Sends touch events to the Windows host over UDP port 7778.
 *
 * Packet layout (16 bytes, big-endian):
 *   [0-3]  magic  'P','D','T','I'
 *   [4]    type   0=MOVE  1=DOWN  2=UP
 *   [5-7]  reserved
 *   [8-11] nx     normalized X in [0,1]
 *   [12-15] ny    normalized Y in [0,1]
 */
class TouchSender(targetIp: String, private val port: Int = 7778) {

    enum class EventType(val code: Byte) {
        MOVE(0), DOWN(1), UP(2)
    }

    private val socket     = DatagramSocket()
    private val targetAddr = InetAddress.getByName(targetIp)
    private val executor   = Executors.newSingleThreadExecutor()

    fun send(type: EventType, nx: Float, ny: Float) {
        executor.submit {
            try {
                val buf = ByteBuffer.allocate(16).order(ByteOrder.BIG_ENDIAN)
                buf.put('P'.code.toByte())
                buf.put('D'.code.toByte())
                buf.put('T'.code.toByte())
                buf.put('I'.code.toByte())
                buf.put(type.code)
                buf.put(0); buf.put(0); buf.put(0)   // reserved
                buf.putFloat(nx)
                buf.putFloat(ny)
                socket.send(DatagramPacket(buf.array(), 16, targetAddr, port))
            } catch (_: Exception) {}
        }
    }

    fun close() {
        executor.shutdown()
        socket.close()
    }
}
