#include "AdbUsbSetup.h"

#include <windows.h>
#include <shlobj.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

static std::string RunCapture(const char* cmd) {
    std::string out;
    FILE*       pipe = _popen(cmd, "r");
    if (!pipe) return out;
    std::array<char, 512> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        out += buf.data();
    _pclose(pipe);
    return out;
}

static bool FileExists(const fs::path& p) { return fs::is_regular_file(p); }

static fs::path FindAdbExe() {
    // PATH
    {
        char pathBuf[32768] = {};
        if (GetEnvironmentVariableA("PATH", pathBuf, sizeof(pathBuf)) > 0) {
            std::istringstream iss(pathBuf);
            std::string        seg;
            while (std::getline(iss, seg, ';')) {
                if (seg.empty()) continue;
                fs::path cand = fs::path(seg) / "adb.exe";
                if (FileExists(cand)) return cand;
            }
        }
    }
    // ANDROID_HOME / ANDROID_SDK_ROOT
    for (const char* ev : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
        char root[1024] = {};
        if (GetEnvironmentVariableA(ev, root, sizeof(root)) > 0) {
            fs::path cand = fs::path(root) / "platform-tools" / "adb.exe";
            if (FileExists(cand)) return cand;
        }
    }
    char localAppData[MAX_PATH] = {};
    if (SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData) == S_OK) {
        fs::path cand =
            fs::path(localAppData) / "Android" / "Sdk" / "platform-tools" / "adb.exe";
        if (FileExists(cand)) return cand;
    }
    return {};
}

std::string RunAdbUsbReverse(uint16_t video_port, uint16_t touch_port) {
    const fs::path adb = FindAdbExe();
    if (adb.empty()) {
        return "adb.exe not found (install Android SDK platform-tools or add adb to PATH).";
    }

    std::cout << "  [ADB] Using: " << adb.string() << "\n";

    const std::string base = "\"" + adb.string() + "\"";

    const std::string devices_out = RunCapture((base + " devices").c_str());
    std::cout << "  [ADB] devices:\n" << devices_out;

    int device_lines = 0;
    {
        std::istringstream iss(devices_out);
        std::string        line;
        while (std::getline(iss, line)) {
            const auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            std::string state = line.substr(tab + 1);
            while (!state.empty() && (state.back() == '\r' || state.back() == ' '))
                state.pop_back();
            if (state == "device") ++device_lines;
        }
    }
    if (device_lines < 1) {
        return "No Android device in \"device\" state. Enable USB debugging and run \"adb devices\".";
    }

    auto run = [&](const char* args) -> std::string {
        const std::string cmd = base + " " + args;
        std::cout << "  [ADB] " << cmd << "\n";
        FILE* pipe = _popen(cmd.c_str(), "r");
        if (!pipe) return "failed to spawn adb";
        std::array<char, 1024> buf{};
        std::string            err;
        while (fgets(buf.data(), static_cast<int>(buf.size()), pipe)) err += buf.data();
        const int ec = _pclose(pipe);
        if (ec != 0) return "exit code " + std::to_string(ec) + (err.empty() ? "" : (": " + err));
        return {};
    };

    if (std::string e = run("reverse --remove-all"); !e.empty())
        return "adb reverse --remove-all failed: " + e;
    std::cout << "  [ADB] reverse --remove-all: OK\n";

    {
        std::ostringstream oss;
        oss << "reverse tcp:" << video_port << " tcp:" << video_port;
        if (std::string e = run(oss.str().c_str()); !e.empty())
            return "adb reverse (video port) failed: " + e;
    }
    std::cout << "  [ADB] reverse tcp:" << video_port << " tcp:" << video_port << ": OK\n";

    {
        std::ostringstream oss;
        oss << "reverse tcp:" << touch_port << " tcp:" << touch_port;
        if (std::string e = run(oss.str().c_str()); !e.empty())
            return "adb reverse (touch port) failed: " + e;
    }
    std::cout << "  [ADB] reverse tcp:" << touch_port << " tcp:" << touch_port << ": OK\n";

    return {};
}
