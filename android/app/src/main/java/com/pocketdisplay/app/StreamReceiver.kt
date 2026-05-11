package com.pocketdisplay.app

import android.view.Surface
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.concurrent.thread

class StreamReceiver(
    surface: Surface,
    private val onStatus: (String) -> Unit,
    onDimensions: ((Int, Int) -> Unit)? = null,
    private val onSenderIp: ((String) -> Unit)? = null
) {
    companion object {
        const val PORT             = 7777
        const val MAX_PACKET_SIZE  = 1500
        const val HEADER_SIZE      = 20
        val MAGIC                  = byteArrayOf('P'.code.toByte(), 'D'.code.toByte(),
                                                  'S'.code.toByte(), 'M'.code.toByte())
        const val FLAG_CODEC_CONFIG = 0x01.toByte()
        const val FLAG_KEYFRAME     = 0x02.toByte()
        // Drop frames older than this many frames behind the latest seen
        const val MAX_FRAME_GAP    = 30
    }

    @Volatile var isRunning = false
    private var socket: DatagramSocket? = null
    private val decoder = VideoDecoder(surface, onStatus, onDimensions)

    // frameId -> accumulated frame data
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
        thread(name = "StreamReceiver", isDaemon = true) {
            try {
                val sock = DatagramSocket(PORT)
                sock.receiveBufferSize = 4 * 1024 * 1024
                socket = sock
                onStatus("Listening on :$PORT")

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
                if (isRunning) onStatus("Receiver error: ${e.message}")
            }
        }
    }

    private fun processPacket(data: ByteArray, length: Int) {
        if (length < HEADER_SIZE) return

        // Validate magic
        if (data[0] != MAGIC[0] || data[1] != MAGIC[1] ||
            data[2] != MAGIC[2] || data[3] != MAGIC[3]) return

        val bb = ByteBuffer.wrap(data, 4, length - 4).order(ByteOrder.BIG_ENDIAN)
        val frameId      = bb.int
        val packetIdx    = bb.short.toInt() and 0xFFFF
        val totalPackets = bb.short.toInt() and 0xFFFF
        val frameSize    = bb.int
        val flags        = bb.get()
        // skip reserved[3]

        val payloadStart = HEADER_SIZE
        val payloadLen   = length - HEADER_SIZE
        if (payloadLen <= 0 || totalPackets == 0 || packetIdx >= totalPackets) return

        val payload = data.copyOfRange(payloadStart, payloadStart + payloadLen)

        if (flags == FLAG_CODEC_CONFIG) {
            decoder.configure(payload)
            return
        }

        // Track latest frame for stale eviction
        if (frameId > latestFrameId) latestFrameId = frameId

        // Evict frames that are too far behind
        val staleIds = frameBuffer.keys.filter { latestFrameId - it > MAX_FRAME_GAP }
        staleIds.forEach { frameBuffer.remove(it) }

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
            pkt?.let {
                it.copyInto(frameData, offset)
                offset += it.size
            }
        }
        val isKeyframe = (assembly.flags.toInt() and FLAG_KEYFRAME.toInt()) != 0
        decoder.decode(frameData, isKeyframe)
    }

    fun stop() {
        isRunning = false
        socket?.close()
        socket = null
        decoder.release()
        frameBuffer.clear()
    }
}
