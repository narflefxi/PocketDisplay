#include "VirtualDisplayDriver.h"

#include <windows.h>
#include <urlmon.h>
#include <newdev.h>
#include <setupapi.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static bool IsVirtualDisplayDevice(const DISPLAY_DEVICEW& dd) {
    return (dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0
        || wcsstr(dd.DeviceString, L"Virtual")  != nullptr
        || wcsstr(dd.DeviceString, L"Indirect") != nullptr
        || wcsstr(dd.DeviceString, L"IDD")      != nullptr
        || wcsstr(dd.DeviceString, L"Dummy")    != nullptr
        || wcsstr(dd.DeviceString, L"MttVDD")   != nullptr;
}

bool HasActiveVirtualDisplay() {
    for (DWORD i = 0; ; ++i) {
        DISPLAY_DEVICEW dd = {};
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) break;
        if ((dd.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0) continue;
        if (IsVirtualDisplayDevice(dd)) return true;
    }
    return false;
}

static fs::path GetExeDir() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) return {};
        if (len < buf.size() - 1) return fs::path(buf.data()).parent_path();
        buf.resize(buf.size() * 2);
    }
}

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation = {};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

static fs::path FindDriverInf() {
    const fs::path exe_dir = GetExeDir();
    const fs::path bundled = exe_dir / "drivers" / "virtual-display" / "MttVDD.inf";
    if (fs::is_regular_file(bundled)) return bundled;

    const fs::path dev = exe_dir.parent_path().parent_path().parent_path()
        / "drivers" / "virtual-display" / "MttVDD.inf";
    if (fs::is_regular_file(dev)) return dev;

    return {};
}

static fs::path GetTempInstallerPath() {
    wchar_t temp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temp) == 0) return {};
    return fs::path(temp) / "PocketDisplay-VDD-setup-x64.exe";
}

static std::string LastWin32Error(const char* what) {
    std::ostringstream oss;
    oss << what << " failed with Win32 error " << GetLastError();
    return oss.str();
}

static bool RootDeviceExists(const wchar_t* hardware_id) {
    HDEVINFO devs = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    for (DWORD i = 0; ; ++i) {
        SP_DEVINFO_DATA data = {};
        data.cbSize = sizeof(data);
        if (!SetupDiEnumDeviceInfo(devs, i, &data)) break;

        wchar_t ids[4096] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(
                devs, &data, SPDRP_HARDWAREID, nullptr,
                reinterpret_cast<PBYTE>(ids), sizeof(ids), nullptr)) {
            continue;
        }

        for (const wchar_t* p = ids; *p; p += wcslen(p) + 1) {
            if (_wcsicmp(p, hardware_id) == 0) {
                found = true;
                break;
            }
        }
        if (found) break;
    }

    SetupDiDestroyDeviceInfoList(devs);
    return found;
}

static std::string CreateRootDevice(const wchar_t* hardware_id) {
    const GUID display_class = {0x4D36E968, 0xE325, 0x11CE, {0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18}};
    HDEVINFO devs = SetupDiCreateDeviceInfoList(&display_class, nullptr);
    if (devs == INVALID_HANDLE_VALUE) return LastWin32Error("SetupDiCreateDeviceInfoList");

    SP_DEVINFO_DATA data = {};
    data.cbSize = sizeof(data);
    if (!SetupDiCreateDeviceInfoW(
            devs, L"Virtual Display Driver", &display_class, nullptr, nullptr,
            DICD_GENERATE_ID, &data)) {
        const std::string e = LastWin32Error("SetupDiCreateDeviceInfo");
        SetupDiDestroyDeviceInfoList(devs);
        return e;
    }

    wchar_t multi_sz[64] = {};
    wcscpy_s(multi_sz, hardware_id);
    if (!SetupDiSetDeviceRegistryPropertyW(
            devs, &data, SPDRP_HARDWAREID,
            reinterpret_cast<const BYTE*>(multi_sz),
            static_cast<DWORD>((wcslen(hardware_id) + 2) * sizeof(wchar_t)))) {
        const std::string e = LastWin32Error("SetupDiSetDeviceRegistryProperty");
        SetupDiDestroyDeviceInfoList(devs);
        return e;
    }

    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devs, &data)) {
        const DWORD err = GetLastError();
        SetupDiDestroyDeviceInfoList(devs);
        if (err != ERROR_DI_DO_DEFAULT) {
            std::ostringstream oss;
            oss << "SetupDiCallClassInstaller(DIF_REGISTERDEVICE) failed with Win32 error " << err;
            return oss.str();
        }
    }

    SetupDiDestroyDeviceInfoList(devs);
    return {};
}

static std::string RunHiddenInstaller(const fs::path& exe) {
    std::wstring cmd = L"\"" + exe.wstring() + L"\" /quiet /norestart";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, exe.parent_path().wstring().c_str(), &si, &pi);
    if (!ok) return LastWin32Error("CreateProcess");
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exit_code == 0 || exit_code == 3010) return {};
    std::ostringstream oss;
    oss << "VDD Control installer failed with exit code " << exit_code;
    return oss.str();
}

static std::string DownloadAndRunVddControlInstaller() {
    if (!IsProcessElevated()) {
        return "First-time VDD setup requires Administrator. Please run PocketDisplay as Administrator once.";
    }
    const fs::path installer = GetTempInstallerPath();
    if (installer.empty()) return "Could not determine temp path for VDD installer.";
    constexpr wchar_t kUrl[] = L"https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.5.2/Virtual.Display.Driver-v25.05.03-setup-x64.exe";
    const HRESULT hr = URLDownloadToFileW(nullptr, kUrl, installer.wstring().c_str(), 0, nullptr);
    if (FAILED(hr)) {
        std::ostringstream oss;
        oss << "VDD installer download failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << ")";
        return oss.str();
    }
    if (std::string e = RunHiddenInstaller(installer); !e.empty()) return e;
    Sleep(2500);
    return {};
}

std::string EnsureVirtualDisplayDriverForExtendedMode() {
    if (HasActiveVirtualDisplay()) return {};

    const fs::path inf = FindDriverInf();
    if (inf.empty()) {
        return "Extended mode requires the VirtualDrivers Virtual Display Driver. Missing bundled driver file: drivers\\virtual-display\\MttVDD.inf";
    }
    if (!IsProcessElevated()) {
        return "Extended mode requires installing the bundled virtual display driver. Please run PocketDisplay as Administrator once.";
    }

    BOOL reboot = FALSE;
    if (!DiInstallDriverW(nullptr, inf.wstring().c_str(), DIIRFLAG_FORCE_INF, &reboot)) {
        return LastWin32Error("DiInstallDriver");
    }

    constexpr wchar_t kHardwareId[] = L"Root\\MttVDD";
    if (!RootDeviceExists(kHardwareId)) {
        if (const std::string e = CreateRootDevice(kHardwareId); !e.empty()) return e;
    }

    if (!UpdateDriverForPlugAndPlayDevicesW(
            nullptr, kHardwareId, inf.wstring().c_str(), INSTALLFLAG_FORCE, &reboot)) {
        std::ostringstream oss;
        oss << "UpdateDriverForPlugAndPlayDevices failed with Win32 error " << GetLastError();
        return oss.str();
    }

    Sleep(2500);
    if (HasActiveVirtualDisplay()) return {};
    return "Virtual display driver installed, but no active virtual display appeared. Open Windows Display Settings or reboot, then try Extended mode again.";
}

std::string EnsureVirtualDisplayDriverInstalled() {
    if (HasActiveVirtualDisplay()) return {};
    if (const fs::path inf = FindDriverInf(); !inf.empty()) {
        if (const std::string e = EnsureVirtualDisplayDriverForExtendedMode(); e.empty() || HasActiveVirtualDisplay()) return e;
    }
    if (const std::string e = DownloadAndRunVddControlInstaller(); !e.empty()) return e;
    if (HasActiveVirtualDisplay()) return {};
    return "VDD setup completed, but no active virtual display is visible yet. Reboot may be required.";
}
