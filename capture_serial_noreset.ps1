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
# No EN/IO0 pulse: attach to the running firmware without rebooting it.
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
