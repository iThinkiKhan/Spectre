param(
  [string]$Port = 'COM13',
  [string]$LogFile = 'C:\PlatformIO\Projects\Spectre\diagnostics\entity-session.log',
  [int]$ObserveMs = 45000
)

$ErrorActionPreference = 'Stop'
$sp = [System.IO.Ports.SerialPort]::new($Port, 115200)
$sp.DtrEnable = $true
$sp.RtsEnable = $false
$sp.ReadTimeout = 200
$sp.NewLine = "`n"
$out = [System.IO.StreamWriter]::new($LogFile, $false)
$out.AutoFlush = $true

function Pump([int]$Milliseconds) {
  $deadline = (Get-Date).AddMilliseconds($Milliseconds)
  while ((Get-Date) -lt $deadline) {
    $chunk = $sp.ReadExisting()
    if ($chunk) { $out.Write($chunk) }
    Start-Sleep -Milliseconds 80
  }
}

$sp.Open()
try {
  $out.WriteLine("=== Entity session opened $(Get-Date -Format o) ===")
  Pump $ObserveMs
  $out.WriteLine('=== send: entity status ===')
  $sp.WriteLine('entity status')
  Pump 1500
  $out.WriteLine('=== send: heap status ===')
  $sp.WriteLine('heap status')
  Pump 1500
  $out.WriteLine("=== Entity session closing $(Get-Date -Format o) ===")
} finally {
  $out.Close()
  try {
    $sp.DtrEnable = $false
    Start-Sleep -Milliseconds 100
  } catch {}
  $sp.Close()
  $sp.Dispose()
}

Write-Output "done -> $LogFile"
