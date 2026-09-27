param(
    [switch]$TailLogs,
    [switch]$SkipTrigger,
    [int]$TailLines = 40
)
$ErrorActionPreference = "Stop"
$scriptPath = $MyInvocation.MyCommand.Path
if (-not $scriptPath -and $PSCommandPath) { $scriptPath = $PSCommandPath }
if (-not $scriptPath) { throw "cannot resolve script path" }
$p = $scriptPath
for ($i = 0; $i -lt 5; $i++) { $p = Split-Path -Parent $p }
$fwRoot = Join-Path $p "garage_door_firmware"
$bin = Join-Path $fwRoot ".pio\build\esp32dev\firmware.bin"
$verH = Join-Path $fwRoot "src\fw_version.h"
$sshKey = Join-Path $env:USERPROFILE ".ssh\id_ed25519"
$sshHost = "root@101.37.175.30"
$sshArgs = @("-i", $sshKey, "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", "-o", "IdentitiesOnly=yes")

Write-Host ("root=" + $p)
Write-Host ("bin exists=" + (Test-Path $bin))

if (-not (Test-Path $bin)) {
    Write-Host "build firmware.bin ..."
    Push-Location $fwRoot
    & $env:MIMO_PYTHON -m platformio run -e esp32dev
    Pop-Location
}
if (-not (Test-Path $bin)) { throw ("missing " + $bin) }

$fwVer = "unknown"
foreach ($line in Get-Content $verH) {
    if ($line -match 'FW_VERSION\s+"([^"]+)"') { $fwVer = $Matches[1]; break }
}

Write-Host ("publish FW_VERSION=" + $fwVer)
& scp @sshArgs $bin ($sshHost + ":/opt/garage-gate/ota/firmware.bin")
$hash = (Get-FileHash -Algorithm SHA256 $bin).Hash.ToLower()
$verPath = Join-Path $env:TEMP "garage-version.json"
$json = '{"version":"' + $fwVer + '","sha256":"' + $hash + '","url":"/ota/firmware.bin"}'
Set-Content -Path $verPath -Value $json -Encoding ASCII
& scp @sshArgs $verPath ($sshHost + ":/opt/garage-gate/ota/version.json")
Remove-Item $verPath -ErrorAction SilentlyContinue
& ssh @sshArgs $sshHost "cat /opt/garage-gate/ota/version.json"

if (-not $SkipTrigger) {
    # 默认不自动下发 update：升级只由网页「立即更新」触发
    Write-Host "skip auto-trigger (manual only) — 在设备页点「立即更新」才会升级"
}

& ssh @sshArgs $sshHost "journalctl -u garage-gate -n 12 --no-pager"
if ($TailLogs) {
    $cmd = 'tail -n ' + $TailLines + ' /opt/garage-gate/logs/device-$(date +%Y%m%d).log'
    & ssh @sshArgs $sshHost $cmd
}
Write-Host "done."