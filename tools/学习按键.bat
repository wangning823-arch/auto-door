@echo off
title RF Learn GUI
cd /d "%~dp0"
echo Starting RF Learn GUI...
set "PY="
if defined MIMO_PYTHON if exist "%MIMO_PYTHON%" set "PY=%MIMO_PYTHON%"
if not defined PY if exist "C:\Program Files\Xiaomi MiMo\resources\runtimes\win32-x64\python\python.exe" set "PY=C:\Program Files\Xiaomi MiMo\resources\runtimes\win32-x64\python\python.exe"
if not defined PY (
    where python >nul 2>nul && set "PY=python"
)
if not defined PY (
    echo Python not found
    pause
    exit /b 1
)
"%PY%" "%~dp0rf_learn_gui.py"
if errorlevel 1 (
    echo.
    echo [ERROR] launch failed: pip install pyserial
    pause
)
