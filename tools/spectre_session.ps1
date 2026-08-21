# Persistent Spectre console session.
#
# Opening the app CDC (MI_01) from a one-shot SerialPort resets the S3, so every
# separate Invoke-SpectreCommand call costs a full reboot (~105s with a large
# entity rebuild). This helper pays that cost ONCE: it opens the port, waits for
# the firmware to finish booting, then runs a whole command list on the same
# handle and tees everything to a log.
#
#   .\tools\spectre_session.ps1 -Commands 'spool count','heap status' `
#       -LogPath diagnostics\foo.log -BootTimeoutSec 240 -PerCommandSec 60
#
# NOTE: only ever open MI_01. See tools/spectre_serial.ps1 for why.

param(
    [Parameter(Mandatory = $true)][string[]]$Commands,
    [string]$LogPath,
    [int]$BootTimeoutSec = 240,
    [int]$PerCommandSec = 60,
    # Regex that marks "firmware is up and listening".
    [string]$ReadyPattern = 'Hardware ready|BOOTSEQ\] start|Initial storage summary',
    # Stop waiting on a command early once this matches (per command).
    [string]$DonePattern = ''
)

function Get-SpectrePort {
    $dev = Get-CimInstance Win32_PnPEntity | Where-Object {
        $_.Name -match 'USB Serial Device \(COM\d+\)' -and
        $_.DeviceID -match 'VID_303A&PID_1001&MI_01'
    } | Select-Object -First 1
    if (-not $dev) { return $null }
    if ($dev.Name -match 'COM(\d+)') { return "COM$($Matches[1])" }
    return $null
}

$port = Get-SpectrePort
if (-not $port) { Write-Output 'ERROR: app CDC (MI_01) not found'; exit 1 }
Write-Output "[session] port=$port"

$sp = New-Object System.IO.Ports.SerialPort($port, 115200)
$sp.DtrEnable = $true
$sp.RtsEnable = $true
$sp.ReadTimeout = 500
$lines = New-Object System.Collections.Generic.List[string]

function Add-Line([string]$t) {
    $script:lines.Add($t) | Out-Null
    Write-Output $t
}

try {
    $sp.Open()
} catch {
    Write-Output "ERROR: open failed: $($_.Exception.Message)"
    exit 1
}

try {
    # --- wait for boot to settle -------------------------------------------
    Add-Line "[session] waiting up to ${BootTimeoutSec}s for firmware ready"
    $deadline = (Get-Date).AddSeconds($BootTimeoutSec)
    $ready = $false
    $quietUntil = $null
    # Opening the CDC usually resets the S3, but not always -- when it does not,
    # the boot banner never comes and waiting for it burns the whole timeout.
    # Steady runtime chatter is equally good evidence the app is up and reading
    # its console, so accept that after a short grace period.
    $runtimeLines = 0
    $graceUntil = (Get-Date).AddSeconds(12)
    while ((Get-Date) -lt $deadline) {
        try {
            $l = $sp.ReadLine()
            Add-Line $l
            if ($l -match $ReadyPattern) {
                $ready = $true
                # let the remaining boot chatter drain before commanding
                $quietUntil = (Get-Date).AddSeconds(8)
            } elseif ($l -match '^\[\d+\]\[[IWED]\]') {
                $runtimeLines++
                if (-not $ready -and $runtimeLines -ge 5 -and (Get-Date) -gt $graceUntil) {
                    Add-Line '[session] no boot banner; app already running'
                    $ready = $true
                    $quietUntil = (Get-Date).AddSeconds(3)
                }
            }
        } catch {
            if ($ready -and $quietUntil -and (Get-Date) -gt $quietUntil) { break }
        }
        if ($ready -and $quietUntil -and (Get-Date) -gt $quietUntil) { break }
    }
    Add-Line "[session] ready=$ready — sending $($Commands.Count) command(s)"

    # --- run the command list on the same handle ---------------------------
    foreach ($c in $Commands) {
        Add-Line "[session] >>> $c"
        $sp.DiscardInBuffer()
        $sp.WriteLine($c)
        $cmdDeadline = (Get-Date).AddSeconds($PerCommandSec)
        while ((Get-Date) -lt $cmdDeadline) {
            try {
                $l = $sp.ReadLine()
                Add-Line $l
                if ($DonePattern -and $l -match $DonePattern) { break }
            } catch { }
        }
    }
} finally {
    if ($sp.IsOpen) { $sp.Close() }
    if ($LogPath) {
        $dir = Split-Path -Parent $LogPath
        if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
        $lines | Set-Content -Path $LogPath -Encoding utf8
        Write-Output "[session] log -> $LogPath ($($lines.Count) lines)"
    }
}
