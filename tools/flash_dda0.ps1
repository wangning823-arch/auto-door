# dda0 USB 烧录脚本（坏 OTA 后恢复 / 新版直刷）
# 用法: .\flash_dda0.ps1 -Port COM3 [-App .\path\firmware.bin]
# 分区: app0@0x10000 / app1@0x200000 / otadata@0xe000
# 双槽写入同一 app + 擦除 otadata，保证不论 ota 选择哪个槽都启动新固件。
param(
    [Parameter(Mandatory=$true)][string]$Port,
    [string]$App = "$PSScriptRoot\..\garage_door_firmware\.pio\build\esp32dev\firmware.bin",
    [string]$Bootloader = "$PSScriptRoot\..\garage_door_firmware\.pio\build\esp32dev\bootloader.bin",
    [string]$Partitions = "$PSScriptRoot\..\garage_door_firmware\.pio\build\esp32dev\partitions.bin"
)

$ErrorActionPreference = "Stop"
foreach ($f in @($App, $Bootloader, $Partitions)) {
    if (-not (Test-Path $f)) { throw "missing file: $f" }
}
$sha = (Get-FileHash $App -Algorithm SHA256).Hash.ToLower()
Write-Host "app sha256 = $sha"
if ($sha -ne "0f55d09034c86520f0a036ea4fd0d52e0e17e10c51226a00d4e60bdc6727e46f") {
    Write-Warning "app hash != VPS 0.2.202610011246, continue only if intentional"
}

$py = if ($env:MIMO_PYTHON) { $env:MIMO_PYTHON } else { "python" }

Write-Host "=== step1: erase otadata (0xe000, 8KB) ==="
& $py -m esptool --chip esp32 --port $Port erase-region 0xe000 8192
if ($LASTEXITCODE -ne 0) { throw "erase-region failed" }

Write-Host "=== step2: write bootloader/partitions/app0/app1 ==="
& $py -m esptool --chip esp32 --port $Port write-flash `
    0x1000 $Bootloader `
    0x8000 $Partitions `
    0x10000 $App `
    0x200000 $App
if ($LASTEXITCODE -ne 0) { throw "write-flash failed" }

Write-Host "=== done. reset device (EN/USB) and watch serial ==="
