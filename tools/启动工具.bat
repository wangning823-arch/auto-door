@echo off
chcp 65001 >nul
title 车库门调试工具
cd /d "%~dp0"

echo.
echo ========================================
echo        车库门调试工具
echo ========================================
echo.
echo   1. RF 抓包查看器 (433MHz)
echo   2. 蓝牙 RSSI 监控
echo   3. 打开串口监视器
echo   0. 退出
echo.
echo ========================================

set /p choice=请选择 (0-3): 

if "%choice%"=="1" call "启动RF抓包工具.bat"
if "%choice%"=="2" call "启动RSSI监控.bat"
if "%choice%"=="3" goto serial
if "%choice%"=="0" exit /b 0

goto :eof

:serial
echo.
echo 打开串口监视器 (115200 baud, COM3)...
set "PY="
if defined MIMO_PYTHON set "PY=%MIMO_PYTHON%"
if not defined PY set "PY=python"
%PY% -m platformio device monitor -b 115200 -p COM3
pause
