param(
  [string]$Port = 'COM13',
  [Parameter(Mandatory = $true)][string]$LogFile,
  [int]$Seconds = 60,
  [string]$Command = '',
  [string]$CommandBase64 = '',
  [int]$SendAfterMs = 0,
  [bool]$DtrEnable = $true
)

$ErrorActionPreference = 'Stop'
$sp = [System.IO.Ports.SerialPort]::new($Port, 115200)
$sp.DtrEnable = $DtrEnable
$sp.RtsEnable = $false
$sp.ReadTimeout = 200
$out = [System.IO.StreamWriter]::new($LogFile, $false)
$out.AutoFlush = $true

if ($CommandBase64) {
  $Command = [System.Text.Encoding]::UTF8.GetString(
    [System.Convert]::FromBase64String($CommandBase64)
  )
}

$sp.Open()
try {
  $out.WriteLine("=== capture opened $(Get-Date -Format o) ===")
  $sendAt = (Get-Date).AddMilliseconds($SendAfterMs)
  $commandSent = [string]::IsNullOrWhiteSpace($Command)
  $deadline = (Get-Date).AddSeconds($Seconds)
  while ((Get-Date) -lt $deadline) {
    if (-not $commandSent -and (Get-Date) -ge $sendAt) {
      $out.WriteLine("=== send: $Command ===")
      $sp.WriteLine($Command)
      $commandSent = $true
    }
    $chunk = $sp.ReadExisting()
    if ($chunk) { $out.Write($chunk) }
    Start-Sleep -Milliseconds 80
  }
  $out.WriteLine("=== capture closed $(Get-Date -Format o) ===")
} finally {
  $out.Close()
  # Keep DTR asserted through Close(). Deasserting it while the native USB
  # CDC port closes can strand this S3 revision on its ROM COM port.
  $sp.Close()
  $sp.Dispose()
}

Write-Output "done -> $LogFile"
