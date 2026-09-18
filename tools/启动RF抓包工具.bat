@echo off
chcp 65001 >nul
title 433MHz RF 抓包查看器
cd /d "%~dp0"

echo ========================================
echo   433MHz RF 抓包查看器
echo ========================================
echo.

REM 按优先级找 Python
set "PY="
if defined MIMO_PYTHON if exist "%MIMO_PYTHON%" set "PY=%MIMO_PYTHON%"
if not defined PY if exist "C:\Program Files\Python*\python.exe" (
    for /d %%P in ("C:\Program Files\Python*") do set "PY=%%P\python.exe"
)
if not defined PY where python >nul 2>nul && set "PY=python"
if not defined PY where py >nul 2>nul && set "PY=py -3"

if not defined PY (
    echo [错误] 未找到 Python，请安装 Python 3
    echo 下载: https://www.python.org/downloads/
    pause
    exit /b 1
)

echo 使用: %PY%
echo.

%PY% rf_capture_viewer.py
if errorlevel 1 (
    echo.
    echo [错误] 启动失败
    echo 如缺依赖请运行: pip install pyserial matplotlib
    pause
)
