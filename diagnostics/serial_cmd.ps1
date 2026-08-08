param(
  [string]$Port = 'COM13',
  [string]$Cmd = '',
  [int]$ReadMs = 6000,
  [int]$PreReadMs = 300
)
$ErrorActionPreference = 'Stop'
$sp = New-Object System.IO.Ports.SerialPort($Port, 115200)
$sp.DtrEnable = $true
$sp.RtsEnable = $true
$sp.ReadTimeout = 250
$sp.NewLine = "`n"
$sp.Open()
Start-Sleep -Milliseconds $PreReadMs
# drain anything already buffered
try { $sp.ReadExisting() | Out-Null } catch {}
if ($Cmd -ne '') {
  $sp.WriteLine($Cmd)
}
$sb = New-Object System.Text.StringBuilder
$deadline = (Get-Date).AddMilliseconds($ReadMs)
while ((Get-Date) -lt $deadline) {
  try {
    $chunk = $sp.ReadExisting()
    if ($chunk) { [void]$sb.Append($chunk) }
  } catch {}
  Start-Sleep -Milliseconds 100
}
$sp.Close()
Write-Output $sb.ToString()
