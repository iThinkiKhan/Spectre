param(
  [string]$Port = 'COM13',
  [string]$LogFile = 'C:\PlatformIO\Projects\Spectre\diagnostics\companion-enrich-session.log',
  [int]$MaxEnrichMs = 1200000,
  [switch]$PreDumpFieldVault,
  [int]$HoldAfterMs = 0
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
    try {
      $chunk = $sp.ReadExisting()
      if ($chunk) { $out.Write($chunk) }
    } catch {
      $out.WriteLine("=== serial read failed $(Get-Date -Format o): $($_.Exception.Message) ===")
      throw
    }
    Start-Sleep -Milliseconds 80
  }
}

$sp.Open()
try {
  $out.WriteLine("=== session opened $(Get-Date -Format o) ===")
  Pump 1000

  if ($PreDumpFieldVault) {
    $out.WriteLine('=== send: fieldvault dump ===')
    $sp.WriteLine('fieldvault dump')
    Pump 20000
  }

  $out.WriteLine('=== send: companion status ===')
  $sp.WriteLine('companion status')
  Pump 3000

  $out.WriteLine('=== send: companion enrich ===')
  $sp.WriteLine('companion enrich')

  $deadline = (Get-Date).AddMilliseconds($MaxEnrichMs)
  $tail = ''
  $probeRetries = 0
  while ((Get-Date) -lt $deadline) {
    try {
      $chunk = $sp.ReadExisting()
      if ($chunk) {
        $out.Write($chunk)
        $tail += $chunk
        if ($tail.Length -gt 12000) {
          $tail = $tail.Substring($tail.Length - 12000)
        }
        if ($tail -match 'Phone enrichment finished successfully|Manual enrich complete: backlog resolved|Phone enrichment failed') {
          break
        }
        if ($tail -match 'probe timeout: phone not seen' -and $probeRetries -lt 10) {
          $probeRetries++
          $tail = ''
          Start-Sleep -Milliseconds 1200
          $out.WriteLine("=== retry companion enrich $probeRetries ===")
          $sp.WriteLine('companion enrich')
        }
      }
    } catch {
      $out.WriteLine("=== serial read failed $(Get-Date -Format o): $($_.Exception.Message) ===")
      throw
    }
    Start-Sleep -Milliseconds 80
  }

  Pump 3000
  $out.WriteLine('=== send: spool enrich ===')
  $sp.WriteLine('spool enrich')
  Pump 15000

  $out.WriteLine('=== send: companion status ===')
  $sp.WriteLine('companion status')
  Pump 5000
  if ($HoldAfterMs -gt 0) {
    $out.WriteLine("=== holding serial open for $HoldAfterMs ms ===")
    Pump $HoldAfterMs
  }
  $out.WriteLine("=== session closing $(Get-Date -Format o) ===")
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
