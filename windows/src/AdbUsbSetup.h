#pragma once

#include <string>

// Runs `adb reverse` for PocketDisplay USB streaming. Returns error message if setup failed.
// On success returns empty string.
std::string RunAdbUsbReverse(uint16_t video_port, uint16_t touch_port);
