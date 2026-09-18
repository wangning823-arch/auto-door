@echo off
title RSSI Monitor
cd /d "%~dp0"

echo ========================================
echo   Bluetooth RSSI Monitor
echo ========================================
echo Working dir: %CD%
echo.

set "PY="
if defined MIMO_PYTHON if exist "%MIMO_PYTHON%" set "PY=%MIMO_PYTHON%"
if not defined PY (
    where python >nul 2>nul && set "PY=python"
)

if not defined PY (
    echo [ERROR] Python not found!
    pause
    exit /b 1
)

echo Python: %PY%
echo Starting GUI...
echo.

"%PY%" "%~dp0rssi_monitor.py"
set ERR=%ERRORLEVEL%

echo.
echo Python exited with code: %ERR%
if %ERR% neq 0 (
    echo [ERROR] Script failed!
    echo Run: pip install pyserial matplotlib
)
pause
