@echo off
title RSSI Monitor
cd /d "%~dp0"

echo ========================================
echo   Bluetooth RSSI Monitor
echo ========================================
echo.

set "PY="
if defined MIMO_PYTHON if exist "%MIMO_PYTHON%" set "PY=%MIMO_PYTHON%"
if not defined PY (
    where python >nul 2>nul && set "PY=python"
)
if not defined PY (
    where py >nul 2>nul && set "PY=py -3"
)

if not defined PY (
    echo [ERROR] Python not found!
    pause
    exit /b 1
)

echo Using: %PY%
echo.

"%PY%" rssi_monitor.py
if errorlevel 1 (
    echo.
    echo [ERROR] Launch failed
    echo Run: pip install pyserial matplotlib
    pause
)
