# 每 30 分钟拉取 VPS 上 1388 的 panic 监控摘要，并本地追加一份
# 由 Windows 计划任务调用
$ErrorActionPreference = "Continue"
$sshKey = Join-Path $env:USERPROFILE ".ssh\id_ed25519"
$host = "root@101.37.175.30"
$sshArgs = @("-i", $sshKey, "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "-o", "IdentitiesOnly=yes")

$localLog = "D:\mimo\车库门自动化\.tmp_diag\watch1388_local.log"
$dir = Split-Path -Parent $localLog
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }

$ts = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
try {
    $remote = ssh @sshArgs $host "tail -n 40 /opt/garage-gate/logs/watch1388.log 2>/dev/null; echo '---LIVE---'; python3 -c \"
from datetime import datetime
p='/opt/garage-gate/logs/device-garage-1388-20260930.log'
# also try tomorrow file
import os,glob
files=sorted(glob.glob('/opt/garage-gate/logs/device-garage-1388-*.log'))
files=[f for f in files if '2026093' in f or '202610' in f]
up=datetime(2026,9,30,10,40,25)
panics=0; last=None; nfc=None
for path in files[-2:]:
  for line in open(path,errors='replace'):
    if '2026-' not in line[:20]: continue
    try: ts=datetime.strptime(line[:19],'%Y-%m-%d %H:%M:%S')
    except: continue
    if ts>=up and '[BOOT] rst=4' in line: panics+=1
    if ts>=up and 'nfc=' in line:
      if 'nfc=ok' in line: nfc='ok'
      elif 'nfc=defer' in line: nfc='defer'
      elif 'nfc=wait' in line: nfc='wait'
      last=line.strip()[:120]
print('LIVE panics_after_upgrade=%d last_nfc=%s'%(panics,nfc))
if last: print('LIVE',last)
\""
} catch {
    $remote = "SSH_ERROR: $($_.Exception.Message)"
}

$entry = @"

[$ts]
$remote
"@
Add-Content -Path $localLog -Value $entry -Encoding UTF8
Write-Host "watch1388 logged -> $localLog"
