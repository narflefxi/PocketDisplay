@echo off
title PocketDisplay USB (adb reverse)
echo.
echo  PocketDisplay - USB / ADB reverse (optional manual refresh)
echo  =============================================================
echo.
echo  Windows PocketDisplay.exe --usb now runs "adb reverse" for you.
echo  Use this script only if you need to refresh reverse without restarting the PC app.
echo.

where adb >nul 2>nul
if errorlevel 1 (
    echo  ERROR: adb not found in PATH.
    echo  Install Android SDK platform-tools or add adb to PATH.
    echo  https://developer.android.com/tools/releases/platform-tools
    pause
    exit /b 1
)

echo  Checking ADB device...
adb devices
echo.

echo  adb reverse tcp:7777 tcp:7777   (device 127.0.0.1:7777 -^> PC video server)
adb reverse tcp:7777 tcp:7777
if errorlevel 1 ( echo  WARNING: adb reverse 7777 failed. & goto :done )

echo  adb reverse tcp:7778 tcp:7778   (device 127.0.0.1:7778 -^> PC touch server)
adb reverse tcp:7778 tcp:7778
if errorlevel 1 ( echo  WARNING: adb reverse 7778 failed. & goto :done )

echo.
echo  Done. On Android: USB mode -^> Connect. On PC: PocketDisplay.exe --usb
echo.

:done
pause
