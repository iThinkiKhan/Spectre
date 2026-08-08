param(
  [string]$Port = 'COM13',
  [string]$LogFile = 'C:\PlatformIO\Projects\Spectre\diagnostics\enrich-drain-session.log',
  [int]$BootWaitMs = 22000,
  [int]$EnrichReadMs = 300000
)
$ErrorActionPreference = 'Continue'
$sp = New-Object System.IO.Ports.SerialPort($Port, 115200)
$sp.DtrEnable = $true
$sp.RtsEnable = $true
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
$out.WriteLine("=== session opened $(Get-Date -Format o) ===")
# One reset happens on open; let it boot + audit the 10k spool.
Pump $BootWaitMs

# Connect the phone link.
$out.WriteLine("=== send: wio probe ===")
$sp.WriteLine("wio probe")
Pump 10000

# Confirm state.
$out.WriteLine("=== send: wio status ===")
$sp.WriteLine("wio status")
Pump 4000

# Kick the full drain. With the fix this should walk many windows, not stop at ~2.
$out.WriteLine("=== send: wio enrich ===")
$sp.WriteLine("wio enrich")
Pump $EnrichReadMs

$out.WriteLine("=== send: spool enrich (post) ===")
$sp.WriteLine("spool enrich")
Pump 15000

$out.WriteLine("=== session closing $(Get-Date -Format o) ===")
$out.Close()
$sp.Close()
Write-Output "done -> $LogFile"
