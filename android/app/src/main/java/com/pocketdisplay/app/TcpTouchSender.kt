package com.pocketdisplay.app

/** Retained for compatibility. Phase 3: TouchSender is always TCP; this is a direct alias. */
class TcpTouchSender(targetIp: String) : TouchSender(targetIp, port = 7778)
