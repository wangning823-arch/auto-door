@echo off
title RF Capture Viewer
cd /d "%~dp0"

echo ========================================
echo   RF Capture Viewer (433MHz)
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
echo Starting GUI... (window should open)
echo.

"%PY%" "%~dp0rf_capture_viewer.py"
set ERR=%ERRORLEVEL%

echo.
echo Python exited with code: %ERR%
if %ERR% neq 0 (
    echo.
    echo [ERROR] Script failed!
    echo Make sure dependencies installed:
    echo   pip install pyserial matplotlib
)
pause
