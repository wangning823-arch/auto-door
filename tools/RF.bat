@echo off
title RF Capture Tool
cd /d "%~dp0"

echo ========================================
echo   RF Capture Viewer (433MHz)
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
    echo Install: https://www.python.org/downloads/
    pause
    exit /b 1
)

echo Using: %PY%
echo.

"%PY%" rf_capture_viewer.py
if errorlevel 1 (
    echo.
    echo [ERROR] Launch failed
    echo Run: pip install pyserial matplotlib
    pause
)
