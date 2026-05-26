#include "AdbUsbSetup.h"

#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static bool FileExists(const fs::path& p) { return fs::is_regular_file(p); }

static fs::path FindAdbExe() {
    {
        char pathBuf[32768] = {};
        if (GetEnvironmentVariableA("PATH", pathBuf, sizeof(pathBuf)) > 0) {
            std::istringstream iss(pathBuf);
            std::string seg;
            while (std::getline(iss, seg, ';')) {
                if (seg.empty()) continue;
                fs::path cand = fs::path(seg) / "adb.exe";
                if (FileExists(cand)) return cand;
            }
        }
    }
    for (const char* ev : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
        char root[1024] = {};
        if (GetEnvironmentVariableA(ev, root, sizeof(root)) > 0) {
            fs::path cand = fs::path(root) / "platform-tools" / "adb.exe";
            if (FileExists(cand)) return cand;
        }
    }
    char localAppData[MAX_PATH] = {};
    if (SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData) == S_OK) {
        fs::path cand = fs::path(localAppData) / "Android" / "Sdk" / "platform-tools" / "adb.exe";
        if (FileExists(cand)) return cand;
    }
    return {};
}

static std::wstring Quote(const fs::path& p) {
    return L"\"" + p.wstring() + L"\"";
}

static std::string Narrow(const std::wstring& s) {
    if (s.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

static std::string RunAdb(const fs::path& adb, const std::wstring& args, DWORD& exit_code) {
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return {};
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = Quote(adb) + L" " + args;
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};

    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        exit_code = 1;
        std::ostringstream oss;
        oss << "failed to spawn adb (Win32 error " << GetLastError() << ")";
        return oss.str();
    }

    std::string out;
    char buf[1024];
    DWORD n = 0;
    while (ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, buf + n);
    CloseHandle(readPipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return out;
}

static std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

bool DetectUsbDevice() {
    const fs::path adb = FindAdbExe();
    if (adb.empty()) return false;
    DWORD ec = 1;
    const std::string out = RunAdb(adb, L"devices", ec);
    if (ec != 0) return false;
    std::istringstream iss(out);
    std::string line;
    bool header_done = false;
    while (std::getline(iss, line)) {
        if (!header_done) { header_done = true; continue; }
        if (line.find("\tdevice") != std::string::npos) return true;
    }
    return false;
}

std::string RunAdbUsbReverse(uint16_t video_port, uint16_t touch_port) {
    const fs::path adb = FindAdbExe();
    if (adb.empty()) return "adb.exe not found (install Android SDK platform-tools or add adb to PATH).";

    std::cout << "  [ADB] Using: " << adb.string() << "\n";

    DWORD ec = 1;
    const std::string devices_out = RunAdb(adb, L"devices", ec);
    std::cout << "  [ADB] devices:\n" << devices_out;
    if (ec != 0) return "adb devices failed: " + Trim(devices_out);

    int device_lines = 0;
    {
        std::istringstream iss(devices_out);
        std::string line;
        while (std::getline(iss, line)) {
            const auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            std::string state = line.substr(tab + 1);
            while (!state.empty() && (state.back() == '\r' || state.back() == ' ')) state.pop_back();
            if (state == "device") ++device_lines;
        }
    }
    if (device_lines < 1) return "No Android device in \"device\" state. Enable USB debugging and run \"adb devices\".";

    auto run = [&](const std::wstring& args) -> std::string {
        std::cout << "  [ADB] " << adb.string() << " " << Narrow(args) << "\n";
        DWORD code = 1;
        const std::string out = RunAdb(adb, args, code);
        if (code != 0) return "exit code " + std::to_string(code) + (out.empty() ? "" : (": " + Trim(out)));
        return {};
    };

    if (std::string e = run(L"reverse --remove-all"); !e.empty()) return "adb reverse --remove-all failed: " + e;
    std::cout << "  [ADB] reverse --remove-all: OK\n";

    {
        std::wostringstream oss;
        oss << L"reverse tcp:" << video_port << L" tcp:" << video_port;
        if (std::string e = run(oss.str()); !e.empty()) return "adb reverse (video port) failed: " + e;
    }
    std::cout << "  [ADB] reverse tcp:" << video_port << " tcp:" << video_port << ": OK\n";

    {
        std::wostringstream oss;
        oss << L"reverse tcp:" << touch_port << L" tcp:" << touch_port;
        if (std::string e = run(oss.str()); !e.empty()) return "adb reverse (touch port) failed: " + e;
    }
    std::cout << "  [ADB] reverse tcp:" << touch_port << " tcp:" << touch_port << ": OK\n";

    return {};
}
