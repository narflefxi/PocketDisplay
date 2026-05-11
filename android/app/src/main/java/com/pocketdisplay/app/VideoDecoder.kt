package com.pocketdisplay.app

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.view.Surface
import java.nio.ByteBuffer

class VideoDecoder(
    private val surface: Surface,
    private val onStatus: (String) -> Unit,
    private val onDimensions: ((width: Int, height: Int) -> Unit)? = null
) {
    private var codec: MediaCodec? = null
    @Volatile private var running = false

    fun configure(spsData: ByteArray) {
        release()
        try {
            val (sps, pps) = parseSpsAndPps(spsData)
            if (sps == null) {
                onStatus("Config error: no SPS found")
                return
            }

            val format = MediaFormat.createVideoFormat("video/avc", 1920, 1080)
            format.setByteBuffer("csd-0", ByteBuffer.wrap(sps))
            if (pps != null) format.setByteBuffer("csd-1", ByteBuffer.wrap(pps))

            // Minimize decode latency (API 30+)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }

            val c = MediaCodec.createDecoderByType("video/avc")
            c.configure(format, surface, null, 0)
            c.start()
            codec   = c
            running = true
            startOutputDrain()
            onStatus("Decoder ready")
        } catch (e: Exception) {
            onStatus("Decoder error: ${e.message}")
        }
    }

    fun decode(frameData: ByteArray, isKeyframe: Boolean) {
        val c = codec ?: return
        try {
            val idx = c.dequeueInputBuffer(8_000L)
            if (idx >= 0) {
                val buf = c.getInputBuffer(idx) ?: return
                buf.clear()
                buf.put(frameData)
                val flags = if (isKeyframe) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0
                c.queueInputBuffer(idx, 0, frameData.size, System.nanoTime() / 1000L, flags)
            }
        } catch (_: Exception) {}
    }

    private fun startOutputDrain() {
        Thread({
            val info = MediaCodec.BufferInfo()
            while (running) {
                try {
                    when (val idx = codec?.dequeueOutputBuffer(info, 10_000L) ?: break) {
                        MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                            codec?.outputFormat?.let { fmt ->
                                val w = fmt.getInteger(MediaFormat.KEY_WIDTH)
                                val h = fmt.getInteger(MediaFormat.KEY_HEIGHT)
                                if (w > 0 && h > 0) onDimensions?.invoke(w, h)
                            }
                        }
                        in 0..Int.MAX_VALUE -> codec?.releaseOutputBuffer(idx, /*render=*/true)
                    }
                } catch (_: Exception) { break }
            }
        }, "VideoDecoder-Output").also { it.isDaemon = true }.start()
    }

    // Split annexb stream (0x00 0x00 0x00 0x01 or 0x00 0x00 0x01 start codes)
    // and return the raw SPS and PPS NAL payloads (without start code).
    private fun parseSpsAndPps(data: ByteArray): Pair<ByteArray?, ByteArray?> {
        var sps: ByteArray? = null
        var pps: ByteArray? = null
        var i = 0
        while (i < data.size - 3) {
            val sc4 = i + 3 < data.size &&
                      data[i] == 0.toByte() && data[i+1] == 0.toByte() &&
                      data[i+2] == 0.toByte() && data[i+3] == 1.toByte()
            val sc3 = !sc4 &&
                      data[i] == 0.toByte() && data[i+1] == 0.toByte() && data[i+2] == 1.toByte()
            if (!sc4 && !sc3) { i++; continue }

            val payloadStart = i + (if (sc4) 4 else 3)
            if (payloadStart >= data.size) break

            // Find end of this NAL (next start code)
            var j = payloadStart + 1
            while (j < data.size - 2) {
                if (data[j] == 0.toByte() && data[j+1] == 0.toByte() &&
                    (data[j+2] == 1.toByte() || (j+3 < data.size && data[j+2] == 0.toByte() && data[j+3] == 1.toByte()))) {
                    break
                }
                j++
            }
            val nalEnd = if (j >= data.size - 2) data.size else j

            val nal = data.copyOfRange(payloadStart, nalEnd)
            if (nal.isNotEmpty()) {
                when (nal[0].toInt() and 0x1F) {
                    7 -> sps = nal
                    8 -> pps = nal
                }
            }
            i = payloadStart
        }
        return Pair(sps, pps)
    }

    fun release() {
        running = false
        try { codec?.stop() } catch (_: Exception) {}
        try { codec?.release() } catch (_: Exception) {}
        codec = null
    }
}
