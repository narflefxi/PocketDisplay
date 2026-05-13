package com.pocketdisplay.app

/** USB: TCP touch to host port 7778 (`adb reverse`). Same wire format as [TouchSender]. */
class TcpTouchSender(targetIp: String) : TouchSender(targetIp, port = 7778, useTcp = true)
