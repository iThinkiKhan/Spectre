param(
  [string]$Port = 'COM13',
  [string]$LogFile = 'C:\PlatformIO\Projects\Spectre\diagnostics\upload-session.log',
  [int]$SettleMs = 3000,
  [int]$UploadReadMs = 300000
)
$ErrorActionPreference = 'Continue'
$sp = New-Object System.IO.Ports.SerialPort($Port, 115200)
$sp.DtrEnable = $true
$sp.RtsEnable = $false
$sp.ReadTimeout = 200
$sp.NewLine = "`n"
$out = New-Object System.IO.StreamWriter($LogFile, $false)
$out.AutoFlush = $true

function Pump([int]$ms) {
  $deadline = (Get-Date).AddMilliseconds($ms)
  while ((Get-Date) -lt $deadline) {
    try { $c = $sp.ReadExisting(); if ($c) { $out.Write($c) } } catch {}
    Start-Sleep -Milliseconds 80
  }
}

$sp.Open()
$out.WriteLine("=== upload session opened $(Get-Date -Format o) ===")
Pump $SettleMs
try { $sp.ReadExisting() | Out-Null } catch {}

# Confirm upload isn't paused, then kick it off.
$out.WriteLine("=== send: upload resume ===")
$sp.WriteLine("upload resume")
Pump 1500
$out.WriteLine("=== send: upload now ===")
$sp.WriteLine("upload now")
Pump $UploadReadMs

$out.WriteLine("=== upload session closing $(Get-Date -Format o) ===")
$out.Close()
try {
  $sp.DtrEnable = $false
  Start-Sleep -Milliseconds 100
} catch {}
$sp.Close()
$sp.Dispose()
Write-Output "done -> $LogFile"
