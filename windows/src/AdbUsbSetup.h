#pragma once

#include <string>
#include <atomic>

// Runs `adb reverse` for PocketDisplay USB streaming. Returns error message if setup failed.
// On success returns empty string.
std::string RunAdbUsbReverse(uint16_t video_port, uint16_t touch_port);
bool DetectUsbDevice();
// Starts a background thread that re-runs adb reverse whenever a USB device reconnects.
void StartUsbMonitorThread(uint16_t video_port, uint16_t touch_port, std::atomic<bool>& running);
