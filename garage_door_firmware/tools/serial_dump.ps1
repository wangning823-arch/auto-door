param([int]$Seconds = 8)
$port = New-Object System.IO.Ports.SerialPort COM3,115200,None,8,One
$port.ReadTimeout = 500
try {
  $port.Open()
  $port.DiscardInBuffer()
  $t0 = Get-Date
  while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    try {
      $line = $port.ReadLine()
      Write-Output $line
    } catch [System.TimeoutException] {
      # no data
    }
  }
} finally {
  if ($port.IsOpen) { $port.Close() }
}
