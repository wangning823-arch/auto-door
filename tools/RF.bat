@echo off
title RF Capture Viewer
cd /d "%~dp0"

echo ========================================
echo   RF Capture Viewer (433MHz)
echo ========================================
echo Working dir: %CD%
echo.

set "PY="

REM Priority: MIMO_PYTHON > hardcoded path > py launcher
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
echo Install: https://www.python.org/downloads/
pause
exit /b 1

:found
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
