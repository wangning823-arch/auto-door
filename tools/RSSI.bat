@echo off
title RSSI Monitor
cd /d "%~dp0"

echo ========================================
echo   Bluetooth RSSI Monitor
echo ========================================
echo Working dir: %CD%
echo.

set "PY="

if defined MIMO_PYTHON if exist "%MIMO_PYTHON%" (
    set "PY=%MIMO_PYTHON%"
    goto :found
)

if exist "C:\Program Files\Xiaomi MiMo\resources\runtimes\win32-x64\python\python.exe" (
    set "PY=C:\Program Files\Xiaomi MiMo\resources\runtimes\win32-x64\python\python.exe"
    goto :found
)

if exist "D:\Python37\python.exe" (
    set "PY=D:\Python37\python.exe"
    goto :found
)

py -3 --version >nul 2>nul && (
    set "PY=py -3"
    goto :found
)

echo [ERROR] Python not found!
pause
exit /b 1

:found
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
