param(
  [string]$Port = 'COM13',
  [string]$LogFile = 'C:\PlatformIO\Projects\Spectre\diagnostics\upload-resume-session.log',
  [int]$FirstLegMs = 180000,
  [int]$ResumeLegMs = 600000
)

$ErrorActionPreference = 'Stop'
$sp = [System.IO.Ports.SerialPort]::new($Port, 115200)
$sp.DtrEnable = $true
$sp.RtsEnable = $false
$sp.ReadTimeout = 200
$sp.NewLine = "`n"
$out = [System.IO.StreamWriter]::new($LogFile, $false)
$out.AutoFlush = $true

function PumpUntil([int]$Milliseconds, [string]$Pattern) {
  $deadline = (Get-Date).AddMilliseconds($Milliseconds)
  $tail = ''
  while ((Get-Date) -lt $deadline) {
    $chunk = $sp.ReadExisting()
    if ($chunk) {
      $out.Write($chunk)
      $tail += $chunk
      if ($tail.Length -gt 24000) {
        $tail = $tail.Substring($tail.Length - 24000)
      }
      if ($Pattern -and $tail -match $Pattern) {
        return $true
      }
    }
    Start-Sleep -Milliseconds 80
  }
  return $false
}

$sp.Open()
try {
  $out.WriteLine("=== upload/resume session opened $(Get-Date -Format o) ===")
  [void](PumpUntil 1000 '')

  $out.WriteLine('=== send: upload resume ===')
  $sp.WriteLine('upload resume')
  [void](PumpUntil 1500 '')

  $out.WriteLine('=== send: upload now (first leg) ===')
  $sp.WriteLine('upload now')
  $firstProgress = PumpUntil $FirstLegMs 'Dump progress pub=|upload_session_summary|Dump complete|Dump failed'

  if ($firstProgress) {
    $out.WriteLine('=== send: upload stop ===')
    $sp.WriteLine('upload stop')
    [void](PumpUntil 20000 'Dump stopped by request|automatic uploads paused; no active upload')

    $out.WriteLine('=== send: upload now while paused ===')
    $sp.WriteLine('upload now')
    [void](PumpUntil 3000 '\[UPLOAD\] paused')

    $out.WriteLine('=== send: upload resume ===')
    $sp.WriteLine('upload resume')
    [void](PumpUntil 2000 'automatic uploads resumed|already enabled')

    $out.WriteLine('=== send: upload now (resume leg) ===')
    $sp.WriteLine('upload now')
    [void](PumpUntil $ResumeLegMs 'upload_session_summary|Dump complete|Dump failed')
  }

  $out.WriteLine('=== send: spool count ===')
  $sp.WriteLine('spool count')
  [void](PumpUntil 20000 '\[SPOOL\]')
  $out.WriteLine("=== upload/resume session closing $(Get-Date -Format o) ===")
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
