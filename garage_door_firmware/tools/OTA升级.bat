@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

set PY=
if defined MIMO_PYTHON set "PY=%MIMO_PYTHON%"
if not defined PY (
  where py >nul 2>nul && set "PY=py -3"
)
if not defined PY set "PY=python"

start "车库门 OTA" %PY% "%~dp0ota_client.py"
endlocal
