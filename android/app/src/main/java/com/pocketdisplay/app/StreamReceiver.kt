package com.pocketdisplay.app

import android.view.Surface
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.concurrent.thread

/** WiFi: UDP listener on [PORT] for PDSM-framed packets. USB uses [TcpStreamReceiver]. */
class StreamReceiver(
    surface: Surface,
    private val onStatus: (String) -> Unit,
    onDimensions: ((Int, Int) -> Unit)? = null,
    private val onSenderIp: ((String) -> Unit)? = null,
    private val onWindowsSize: ((Int, Int) -> Unit)? = null,
    private val onCursorPos: ((Float, Float) -> Unit)? = null,
    onCodecConfigured: (() -> Unit)? = null
) {
    companion object {
        const val PORT             = 7777
        const val MAX_PACKET_SIZE  = 1500
        const val HEADER_SIZE      = 20
        val MAGIC                  = byteArrayOf('P'.code.toByte(), 'D'.code.toByte(),
                                                  'S'.code.toByte(), 'M'.code.toByte())
        const val FLAG_CODEC_CONFIG = 0x01.toByte()
        const val FLAG_KEYFRAME     = 0x02.toByte()
        const val FLAG_STREAM_INFO  = 0x04.toByte()
        const val FLAG_CURSOR_POS   = 0x08.toByte()
        const val MAX_FRAME_GAP    = 30
    }

    @Volatile var isRunning = false
    private var udpSocket: DatagramSocket? = null
    private val decoder = VideoDecoder(surface, onStatus, onDimensions, onCodecConfigured)

    private val frameBuffer = LinkedHashMap<Int, FrameAssembly>()
    private var latestFrameId = 0

    private data class FrameAssembly(
        val totalPackets: Int,
        val frameSize: Int,
        val flags: Byte,
        val packets: Array<ByteArray?> = arrayOfNulls(totalPackets),
        var received: Int = 0
    )

    fun start() {
        isRunning = true
        thread(name = "StreamReceiver-UDP", isDaemon = true) {
            try {
                val sock = DatagramSocket(PORT)
                sock.receiveBufferSize = 4 * 1024 * 1024
                udpSocket = sock
                onStatus("Listening (WiFi) on :$PORT")

                val buf = ByteArray(MAX_PACKET_SIZE)
                val pkt = DatagramPacket(buf, buf.size)
                var senderReported = false

                while (isRunning) {
                    sock.receive(pkt)
                    if (!senderReported) {
                        pkt.address?.hostAddress?.let { onSenderIp?.invoke(it) }
                        senderReported = true
                    }
                    processPacket(buf, pkt.length)
                }
            } catch (e: Exception) {
                if (isRunning) onStatus("UDP error: ${e.message}")
            }
        }
    }

    private fun processPacket(data: ByteArray, length: Int) {
        if (length < HEADER_SIZE) return
        if (data[0] != MAGIC[0] || data[1] != MAGIC[1] ||
            data[2] != MAGIC[2] || data[3] != MAGIC[3]) return

        val bb = ByteBuffer.wrap(data, 4, length - 4).order(ByteOrder.BIG_ENDIAN)
        val frameId      = bb.int
        val packetIdx    = bb.short.toInt() and 0xFFFF
        val totalPackets = bb.short.toInt() and 0xFFFF
        val frameSize    = bb.int
        val flags        = bb.get()

        val payloadStart = HEADER_SIZE
        val payloadLen   = length - HEADER_SIZE
        if (payloadLen <= 0 || totalPackets == 0 || packetIdx >= totalPackets) return

        val payload = data.copyOfRange(payloadStart, payloadStart + payloadLen)

        if (flags == FLAG_STREAM_INFO) {
            if (payloadLen >= 8) {
                val infoBuf = ByteBuffer.wrap(payload).order(ByteOrder.BIG_ENDIAN)
                val w = infoBuf.int; val h = infoBuf.int
                if (w > 0 && h > 0) onWindowsSize?.invoke(w, h)
            }
            return
        }

        if (flags == FLAG_CURSOR_POS) {
            if (payloadLen >= 8) {
                val cb = ByteBuffer.wrap(payload).order(ByteOrder.BIG_ENDIAN)
                onCursorPos?.invoke(cb.float, cb.float)
            }
            return
        }

        if (flags == FLAG_CODEC_CONFIG) {
            decoder.configure(payload)
            return
        }

        if (frameId > latestFrameId) latestFrameId = frameId
        frameBuffer.keys.filter { latestFrameId - it > MAX_FRAME_GAP }
                        .forEach { frameBuffer.remove(it) }

        val assembly = frameBuffer.getOrPut(frameId) {
            FrameAssembly(totalPackets, frameSize, flags)
        }
        if (assembly.packets[packetIdx] == null) {
            assembly.packets[packetIdx] = payload
            assembly.received++
        }
        if (assembly.received == assembly.totalPackets) {
            frameBuffer.remove(frameId)
            deliverFrame(assembly)
        }
    }

    private fun deliverFrame(assembly: FrameAssembly) {
        val frameData = ByteArray(assembly.frameSize)
        var offset = 0
        for (pkt in assembly.packets) {
            pkt?.let { it.copyInto(frameData, offset); offset += it.size }
        }
        val isKeyframe = (assembly.flags.toInt() and FLAG_KEYFRAME.toInt()) != 0
        decoder.decode(frameData, isKeyframe)
    }

    fun stop() {
        isRunning = false
        udpSocket?.close(); udpSocket = null
        decoder.release()
        frameBuffer.clear()
    }
}
