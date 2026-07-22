param(
  [string]$Port = "COM10",
  [int]$Seconds = 45,
  [string]$Out = "serial_$Port.log"
)
$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, None, 8, One
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.ReadTimeout = 500
try {
  $sp.Open()
} catch {
  "OPEN_FAILED $Port : $($_.Exception.Message)" | Out-File -Encoding utf8 $Out
  exit 1
}
# Pulse EN (RTS) to reboot the ESP32 into run mode so we capture a clean boot.
# DTR (IO0) stays de-asserted so it boots the app, not the bootloader.
$sp.DtrEnable = $false
$sp.RtsEnable = $true
Start-Sleep -Milliseconds 120
$sp.RtsEnable = $false
$end = (Get-Date).AddSeconds($Seconds)
$sw = New-Object System.IO.StreamWriter($Out, $false, [System.Text.Encoding]::UTF8)
while ((Get-Date) -lt $end) {
  try {
    $line = $sp.ReadLine()
    $sw.WriteLine($line)
    $sw.Flush()
  } catch [TimeoutException] { }
}
$sw.Close()
$sp.Close()
