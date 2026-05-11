# PocketDisplay Windows build script
# Usage: .\build.ps1 [Release|Debug]
param([string]$Config = "Release")

$Root      = $PSScriptRoot
$BuildDir  = "$Root\build"
$Toolchain = "C:\vcpkg\scripts\buildsystems\vcpkg.cmake"
$CMake     = "C:\Program Files\CMake\bin\cmake.exe"

if (-not (Test-Path $Toolchain)) {
    Write-Error "vcpkg toolchain not found at $Toolchain. Run: git clone https://github.com/microsoft/vcpkg C:\vcpkg && C:\vcpkg\bootstrap-vcpkg.bat"
    exit 1
}

Write-Host "==> Configuring ($Config)..." -ForegroundColor Cyan
& $CMake -B $BuildDir -S $Root `
    -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$Toolchain" `
    -DVCPKG_TARGET_TRIPLET=x64-windows
if ($LASTEXITCODE -ne 0) { Write-Error "Configure failed"; exit 1 }

Write-Host "==> Building..." -ForegroundColor Cyan
& $CMake --build $BuildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { Write-Error "Build failed"; exit 1 }

$Exe = "$BuildDir\$Config\PocketDisplay.exe"
Write-Host ""
Write-Host "==> Done: $Exe" -ForegroundColor Green
Write-Host "    WiFi: & '$Exe' <android_ip> [--hw]"
Write-Host "    USB:  & '$Exe' --usb [--hw]   (run usb_connect.bat first)"
