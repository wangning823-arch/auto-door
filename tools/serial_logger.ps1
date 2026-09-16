$log = "D:\mimo\车库门自动化\drive_test3_open_then_closed.log"
Add-Content -Path $log -Value ("BOOT " + (Get-Date -Format o))
$p = New-Object System.IO.Ports.SerialPort "COM3",115200
$p.ReadTimeout = 2000
try {
  $p.Open()
  Add-Content -Path $log -Value ("OPENED " + (Get-Date -Format o))
  $end = (Get-Date).AddMinutes(25)
  while ((Get-Date) -lt $end) {
    try {
      $c = $p.ReadExisting()
      if ($c) { Add-Content -Path $log -Value $c }
    } catch {}
    Start-Sleep -Milliseconds 150
  }
  Add-Content -Path $log -Value "=== END ==="
} catch {
  Add-Content -Path $log -Value ("ERR " + $_.Exception.Message)
} finally {
  if ($p.IsOpen) { $p.Close() }
}
