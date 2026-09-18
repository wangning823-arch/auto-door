@echo off
chcp 65001 >nul
title 蓝牙 RSSI 监控
cd /d "%~dp0"

echo ========================================
echo   蓝牙 RSSI 监控
echo ========================================
echo.

set "PY="
if defined MIMO_PYTHON if exist "%MIMO_PYTHON%" set "PY=%MIMO_PYTHON%"
if not defined PY where python >nul 2>nul && set "PY=python"
if not defined PY where py >nul 2>nul && set "PY=py -3"

if not defined PY (
    echo [错误] 未找到 Python，请安装 Python 3
    pause
    exit /b 1
)

echo 使用: %PY%
echo.

%PY% rssi_monitor.py
if errorlevel 1 (
    echo.
    echo [错误] 启动失败
    echo 如缺依赖请运行: pip install pyserial matplotlib
    pause
)
