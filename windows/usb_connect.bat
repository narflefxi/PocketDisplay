@echo off
title PocketDisplay USB Setup
echo.
echo  PocketDisplay - USB/ADB Setup
echo  ================================
echo.

where adb >nul 2>&1
if errorlevel 1 (
    echo  ERROR: adb not found in PATH.
    echo  Install Android Platform Tools and add to PATH.
    echo  https://developer.android.com/tools/releases/platform-tools
    pause
    exit /b 1
)

echo  Checking ADB device...
adb devices
echo.

echo  Setting up port forwarding...
adb forward tcp:7777 tcp:7777
if errorlevel 1 ( echo  WARNING: adb forward failed. & goto :done )

adb reverse tcp:7778 tcp:7778
if errorlevel 1 ( echo  WARNING: adb reverse failed. & goto :done )

echo  Done.
echo.
echo  Stream port (Windows->Android): 7777  [adb forward]
echo  Touch port  (Android->Windows): 7778  [adb reverse]
echo.
echo  Steps:
echo    1. Tap "USB" then "Start" in the Android app
echo    2. Run:  PocketDisplay.exe --usb [--hw]
echo.

:done
pause
