@echo off
title Garage Door Tools
cd /d "%~dp0"

echo.
echo ========================================
echo        Garage Door Debug Tools
echo ========================================
echo.
echo   1. RF Capture Viewer (433MHz)
echo   2. RSSI Monitor (Bluetooth)
echo   3. Serial Monitor (COM3)
echo   0. Exit
echo.
echo ========================================
echo.

set /p choice=Select (0-3): 

if "%choice%"=="1" call "%~dp0RF.bat"
if "%choice%"=="2" call "%~dp0RSSI.bat"
if "%choice%"=="3" goto serial
if "%choice%"=="0" exit /b 0
goto :eof

:serial
echo.
echo Opening serial monitor 115200 COM3...
set "PY="
if defined MIMO_PYTHON set "PY=%MIMO_PYTHON%"
if not defined PY set "PY=python"
"%PY%" -m platformio device monitor -b 115200 -p COM3
pause
