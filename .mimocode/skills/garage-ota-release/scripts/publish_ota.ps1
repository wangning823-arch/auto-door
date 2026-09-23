# 发布车库门固件到 VPS，并触发设备 OTA
# 用法：
#   powershell -File publish_ota.ps1
#   powershell -File publish_ota.ps1 -TailLogs
#   powershell -File publish_ota.ps1 -SkipTrigger   # 只上传，不催 update
param(
    [switch]$TailLogs,
    [switch]$SkipTrigger,
    [int]$TailLines = 40
)

$ErrorActionPreference = "Stop"
$fwRoot = "D:\mimo\车库门自动化\garage_door_firmware"
$bin = Join-Path $fwRoot ".pio\build\esp32dev\firmware.bin"
$verH = Join-Path $fwRoot "src\fw_version.h"
$sshKey = Join-Path $env:USERPROFILE ".ssh\id_ed25519"
$sshHost = "root@101.37.175.30"

function Ssh([string]$cmd) {
    ssh -i $sshKey -o BatchMode=yes -o ConnectTimeout=8 -o IdentitiesOnly=yes $sshHost $cmd
}

if (-not (Test-Path $bin)) {
    Write-Host "编译 firmware.bin ..."
    Push-Location $fwRoot
    & $env:MIMO_PYTHON -m platformio run -e esp32dev
    Pop-Location
}

if (-not (Test-Path $bin)) { throw "缺少 $bin" }

$fwVer = "unknown"
if (Test-Path $verH) {
    $m = Select-String -Path $verH -Pattern '#define FW_VERSION "([^"]+)"'
    if ($m) { $fwVer = $m.Matches[0].Groups[1].Value }
}

Write-Host "==> 发布 FW_VERSION=$fwVer"
Write-Host "==> scp firmware.bin -> VPS ota/"
scp -i $sshKey -o BatchMode=yes -o ConnectTimeout=8 -o IdentitiesOnly=yes $bin "${sshHost}:/opt/garage-gate/ota/firmware.bin"

Write-Host "==> 写 version.json（sha256 以线上 bin 为准）"
Ssh "python3 -c `"import hashlib,json;p='/opt/garage-gate/ota/firmware.bin';h=hashlib.sha256(open(p,'rb').read()).hexdigest();info={'version':'$fwVer','sha256':h,'url':'/ota/firmware.bin'};json.dump(info,open('/opt/garage-gate/ota/version.json','w'));print(info)`""

if (-not $SkipTrigger) {
    Write-Host "==> 触发 update 令 (POST /xiaoai/update)"
    try {
        $r = Invoke-WebRequest -Uri "https://door.wzx.homes/xiaoai/update" -Method POST -TimeoutSec 10
        Write-Host $r.Content
    } catch {
        Write-Host "触发失败（设备仍可在 poll 比对后自动升级）: $_"
    }
}

Write-Host "==> 网关最近日志"
Ssh "journalctl -u garage-gate -n 15 --no-pager | tail -15"

if ($TailLogs) {
    Write-Host "==> 设备上报日志"
    Ssh "tail -n $TailLines /opt/garage-gate/logs/device-`$(date +%Y%m%d).log 2>/dev/null || echo (暂无)"
}

Write-Host ""
Write-Host "完成。设备通常 1～2 分钟内 OTA 并重启。"
Write-Host "查看设备日志:  powershell -File `"$PSCommandPath`" -TailLogs"
Write-Host "再次催更新:    Invoke-WebRequest -Method POST https://door.wzx.homes/xiaoai/update"
