param(
  [string]$Port = 'COM13',
  [string]$LogFile = 'C:\PlatformIO\Projects\Spectre\diagnostics\upload-ack-session.log',
  [int]$CaptureSettleMs = 30000,
  [int]$UploadTimeoutMs = 180000
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
  $out.WriteLine("=== upload/PUBACK session opened $(Get-Date -Format o) ===")
  [void](PumpUntil $CaptureSettleMs '')
  $sp.WriteLine('upload resume')
  [void](PumpUntil 1500 '')
  $out.WriteLine('=== send: upload now ===')
  $sp.WriteLine('upload now')
  $completed = PumpUntil $UploadTimeoutMs 'upload_session_summary|Dump failed'
  $out.WriteLine("=== upload terminal observed=$completed ===")
  $sp.WriteLine('spool count')
  [void](PumpUntil 20000 '\[SPOOL\] totalRecords=')
  $out.WriteLine("=== upload/PUBACK session closing $(Get-Date -Format o) ===")
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
