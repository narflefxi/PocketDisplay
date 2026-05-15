package com.pocketdisplay.app

import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface

/**
 * Listens on UDP port 7779 for a POCKETDISPLAY_HOST broadcast from the Windows host.
 * On receipt, sends a POCKETDISPLAY_CLIENT unicast reply and fires [onHostFound] once.
 */
class DiscoveryClient(private val onHostFound: (hostIp: String, videoPort: Int) -> Unit) {

    companion object {
        private const val TAG           = "DiscoveryClient"
        const val  DISCOVERY_PORT       = 7779
        private const val HOST_PREFIX   = "POCKETDISPLAY_HOST:"
        private const val CLIENT_PREFIX = "POCKETDISPLAY_CLIENT:"
    }

    @Volatile var isRunning = false
        private set

    private var socket: DatagramSocket? = null
    private var thread: Thread?         = null

    fun start() {
        if (isRunning) return
        isRunning = true
        thread = Thread({
            try {
                val sock = DatagramSocket(DISCOVERY_PORT)
                sock.soTimeout  = 200   // ms — keeps loop responsive to stop()
                sock.broadcast  = true
                socket = sock

                val buf      = ByteArray(512)
                var notified = false

                while (isRunning && !notified) {
                    try {
                        val pkt = DatagramPacket(buf, buf.size)
                        sock.receive(pkt)

                        val msg = String(pkt.data, 0, pkt.length).trim()
                        if (!msg.startsWith(HOST_PREFIX)) continue

                        val payload   = msg.removePrefix(HOST_PREFIX)
                        val colonIdx  = payload.lastIndexOf(':')
                        if (colonIdx <= 0) continue

                        val hostIp    = payload.substring(0, colonIdx)
                        val videoPort = payload.substring(colonIdx + 1).toIntOrNull() ?: 7777

                        // Reply so Windows learns our IP.
                        val localIp = getLocalIp()
                        if (localIp != null) {
                            val reply      = "$CLIENT_PREFIX$localIp"
                            val replyBytes = reply.toByteArray()
                            sock.send(DatagramPacket(
                                replyBytes, replyBytes.size,
                                InetAddress.getByName(hostIp), DISCOVERY_PORT
                            ))
                        }

                        notified = true
                        Log.d(TAG, "Host discovered: $hostIp:$videoPort")
                        onHostFound(hostIp, videoPort)

                    } catch (_: java.net.SocketTimeoutException) {
                        // Expected — just keeps the loop alive for stop() checks.
                    }
                }
                sock.close()
            } catch (e: Exception) {
                Log.e(TAG, "Discovery thread error: ${e.message}")
            } finally {
                isRunning = false
            }
        }, "DiscoveryClient").also { it.isDaemon = true; it.start() }
    }

    fun stop() {
        isRunning = false
        socket?.close()
        thread?.join(500)
        thread = null
    }

    /** Sends the user's display mode choice to Windows on port [DISCOVERY_PORT]. */
    fun sendMode(hostIp: String, mode: String) {
        Thread {
            try {
                DatagramSocket().use { s ->
                    val bytes = "POCKETDISPLAY_MODE:$mode".toByteArray()
                    s.send(DatagramPacket(bytes, bytes.size,
                        InetAddress.getByName(hostIp), DISCOVERY_PORT))
                    Log.d(TAG, "Mode sent: $mode -> $hostIp")
                }
            } catch (e: Exception) {
                Log.e(TAG, "sendMode error: ${e.message}")
            }
        }.also { it.isDaemon = true; it.start() }
    }

    private fun getLocalIp(): String? = try {
        NetworkInterface.getNetworkInterfaces()
            ?.asSequence()
            ?.filter { !it.isLoopback && it.isUp }
            ?.flatMap { it.inetAddresses.asSequence() }
            ?.filterIsInstance<Inet4Address>()
            ?.firstOrNull()
            ?.hostAddress
    } catch (_: Exception) { null }
}
