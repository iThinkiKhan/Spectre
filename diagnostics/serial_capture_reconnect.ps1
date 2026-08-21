param(
  [string]$Port = 'COM13',
  [Parameter(Mandatory = $true)][string]$LogFile,
  [int]$Seconds = 120,
  [string]$Command = '',
  [int]$SendAfterMs = 0,
  [string]$Command2 = '',
  [int]$Send2AfterMs = 0,
  [string]$Command3 = '',
  [int]$Send3AfterMs = 0,
  [switch]$Dtr
)

$ErrorActionPreference = 'Stop'
$out = [System.IO.StreamWriter]::new($LogFile, $false)
$out.AutoFlush = $true
$deadline = (Get-Date).AddSeconds($Seconds)
$commandSent = [string]::IsNullOrWhiteSpace($Command)
$sendAt = (Get-Date).AddMilliseconds($SendAfterMs)
$command2Sent = [string]::IsNullOrWhiteSpace($Command2)
$send2At = (Get-Date).AddMilliseconds($Send2AfterMs)
$command3Sent = [string]::IsNullOrWhiteSpace($Command3)
$send3At = (Get-Date).AddMilliseconds($Send3AfterMs)
$attempt = 0

$out.WriteLine("=== reconnecting capture opened $(Get-Date -Format o) port=$Port ===")
try {
  while ((Get-Date) -lt $deadline) {
    $sp = $null
    try {
      $attempt++
      $sp = [System.IO.Ports.SerialPort]::new($Port, 115200)
      $sp.DtrEnable = $Dtr.IsPresent
      $sp.RtsEnable = $false
      $sp.ReadTimeout = 200
      $sp.Open()
      $out.WriteLine("=== serial connected $(Get-Date -Format o) attempt=$attempt ===")

      while ((Get-Date) -lt $deadline -and $sp.IsOpen) {
        if (-not $commandSent -and (Get-Date) -ge $sendAt) {
          $out.WriteLine("=== send: $Command ===")
          $sp.WriteLine($Command)
          $commandSent = $true
        }
        if (-not $command2Sent -and (Get-Date) -ge $send2At) {
          $out.WriteLine("=== send: $Command2 ===")
          $sp.WriteLine($Command2)
          $command2Sent = $true
        }
        if (-not $command3Sent -and (Get-Date) -ge $send3At) {
          $out.WriteLine("=== send: $Command3 ===")
          $sp.WriteLine($Command3)
          $command3Sent = $true
        }
        $chunk = $sp.ReadExisting()
        if ($chunk) { $out.Write($chunk) }
        Start-Sleep -Milliseconds 50
      }
    } catch {
      $out.WriteLine("=== serial disconnected $(Get-Date -Format o): $($_.Exception.Message) ===")
    } finally {
      if ($null -ne $sp) {
        try { if ($sp.IsOpen) { $sp.Close() } } catch {}
        $sp.Dispose()
      }
    }

    if ((Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
  }
} finally {
  $out.WriteLine("=== reconnecting capture closed $(Get-Date -Format o) ===")
  $out.Close()
}

Write-Output "done -> $LogFile"
