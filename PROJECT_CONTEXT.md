# PocketDisplay

SuperDisplay-like low latency display streaming app.

Platforms:
- Windows host (C++)
- Android client (Kotlin)

Working:
- WiFi UDP streaming
- Touch/cursor return
- USB TCP connection established

Current Issue:
USB TCP framing mismatch.
Android receives corrupted frame lengths.

Current desired USB TCP protocol:
4-byte big-endian payload length
1-byte message type
payload bytes

Message types:
1 = codec config
2 = video frame
3 = display size metadata

Important:
- Keep WiFi UDP mode unchanged
- Only fix USB TCP framing
- Do not rewrite architecture