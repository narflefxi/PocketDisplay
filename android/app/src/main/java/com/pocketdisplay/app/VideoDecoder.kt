package com.pocketdisplay.app

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer

private const val TAG = "PocketDisplay"

class VideoDecoder(
    private val surface: Surface,
    private val onStatus: (String) -> Unit,
    private val onDimensions: ((width: Int, height: Int) -> Unit)? = null,
    private val onConfigured: (() -> Unit)? = null,
    private val onFirstFrame: (() -> Unit)? = null
) {
    private var codec: MediaCodec? = null
    @Volatile private var running = false
    @Volatile private var firstFrameDispatched = false
    private var lastSpsData: ByteArray? = null
    private var decodedFrameCount = 0
    private var packetCount = 0

    fun configure(spsData: ByteArray, hintW: Int = 0, hintH: Int = 0) {
        Log.d(TAG, "configure() called: spsData size=${spsData.size}, hintW=$hintW, hintH=$hintH")
        Log.d(TAG, "  csd-0 hex: ${spsData.take(16).joinToString(" ") { String.format("%02X", it) }}${if (spsData.size > 16) "..." else ""}")

        // Deduplication: same SPS + decoder already running → no action needed.
        if (lastSpsData != null && spsData.contentEquals(lastSpsData!!)) {
            Log.d(TAG, "configure() dedup: same SPS, skipping")
            return
        }

        release()  // stop previous codec; also clears lastSpsData
        decodedFrameCount = 0
        packetCount = 0

        var c: MediaCodec? = null
        try {
            val (sps, pps) = parseSpsAndPps(spsData)
            if (sps == null) {
                Log.e(TAG, "configure() failed: no SPS found in codec config")
                onStatus("Config error: no SPS found")
                return
            }
            Log.d(TAG, "  Parsed SPS: ${sps.size} bytes, PPS: ${if (pps != null) "${pps.size} bytes" else "null"}")

            val w = if (hintW > 0) hintW else 1280
            val h = if (hintH > 0) hintH else 720
            val format = MediaFormat.createVideoFormat("video/avc", w, h)
            format.setByteBuffer("csd-0", ByteBuffer.wrap(sps))
            if (pps != null) format.setByteBuffer("csd-1", ByteBuffer.wrap(pps))

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }

            c = MediaCodec.createDecoderByType("video/avc")
            Log.d(TAG, "  Calling MediaCodec.configure()...")
            c.configure(format, surface, null, 0)
            Log.d(TAG, "  MediaCodec.configure() SUCCESS")

            c.start()
            Log.d(TAG, "  MediaCodec.start() SUCCESS")

            codec                = c
            c                    = null
            running              = true
            firstFrameDispatched = false
            startOutputDrain()
            lastSpsData = spsData.copyOf()
            onStatus("Decoder ready")
            onConfigured?.invoke()
        } catch (e: Exception) {
            Log.e(TAG, "configure() FAILED: ${e.message}", e)
            try { c?.release() } catch (_: Exception) {}
            onStatus("Decoder error: ${e.message}")
        }
    }

    fun decode(frameData: ByteArray, isKeyframe: Boolean) {
        val c = codec ?: return
        packetCount++
        if (packetCount <= 5) {
            Log.d(TAG, "decode() packet #$packetCount: size=${frameData.size}, keyframe=$isKeyframe")
        }
        try {
            val idx = c.dequeueInputBuffer(8_000L)
            if (idx >= 0) {
                val buf = c.getInputBuffer(idx) ?: return
                buf.clear()
                buf.put(frameData)
                val flags = if (isKeyframe) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0
                c.queueInputBuffer(idx, 0, frameData.size, System.nanoTime() / 1000L, flags)
            }
        } catch (e: Exception) {
            if (packetCount <= 5) {
                Log.e(TAG, "decode() packet #$packetCount FAILED: ${e.message}")
            }
        }
    }

    private fun startOutputDrain() {
        Thread({
            val info  = MediaCodec.BufferInfo()
            val info2 = MediaCodec.BufferInfo()
            while (running) {
                try {
                    when (val idx = codec?.dequeueOutputBuffer(info, 10_000L) ?: break) {
                        MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                            codec?.outputFormat?.let { fmt ->
                                val w = fmt.getInteger(MediaFormat.KEY_WIDTH)
                                val h = fmt.getInteger(MediaFormat.KEY_HEIGHT)
                                Log.d(TAG, "Output format changed: ${w}x${h}")
                                if (w > 0 && h > 0) onDimensions?.invoke(w, h)
                            }
                        }
                        in 0..Int.MAX_VALUE -> {
                            decodedFrameCount++
                            if (decodedFrameCount <= 5) {
                                Log.d(TAG, "Decoded frame #$decodedFrameCount (pts=${info.presentationTimeUs})")
                            }
                            var renderIdx = idx
                            while (true) {
                                val peekIdx = codec?.dequeueOutputBuffer(info2, 0L) ?: break
                                if (peekIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                                    codec?.outputFormat?.let { fmt ->
                                        val w = fmt.getInteger(MediaFormat.KEY_WIDTH)
                                        val h = fmt.getInteger(MediaFormat.KEY_HEIGHT)
                                        if (w > 0 && h > 0) onDimensions?.invoke(w, h)
                                    }
                                    continue
                                }
                                if (peekIdx < 0) break
                                codec?.releaseOutputBuffer(renderIdx, false)
                                renderIdx = peekIdx
                            }
                            codec?.releaseOutputBuffer(renderIdx, true)
                            if (!firstFrameDispatched) {
                                firstFrameDispatched = true
                                Log.d(TAG, "First frame dispatched to surface!")
                                onFirstFrame?.invoke()
                            }
                        }
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Output drain error: ${e.message}")
                    break
                }
            }
            Log.d(TAG, "Output drain thread exited. Total decoded frames: $decodedFrameCount")
        }, "VideoDecoder-Output").also { it.isDaemon = true }.start()
    }

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

    fun resetForReconnect() {
        lastSpsData = null
    }

    fun release() {
        running = false
        lastSpsData = null
        try { Thread.sleep(50) } catch (_: InterruptedException) {}
        try { codec?.stop() } catch (_: Exception) {}
        try { codec?.release() } catch (_: Exception) {}
        codec = null
    }
}