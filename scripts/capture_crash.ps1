<#
.SYNOPSIS
    Capture Spectre crash evidence from a device on USB into a single transcript.

.DESCRIPTION
    Grabs the two things that survive a field crash:

      1. The boot log, including the [BOOT] crash log: ring printed by
         crashLogPrint() (src/core/CrashBreadcrumb.cpp). Entries tagged
         [CRASH?] were live phases when the device reset.
      2. The USB console dumps (spool state, debug ring, FieldVault records).

    The transcript is flushed to disk incrementally, so a mid-capture
    disconnect never loses what was already read. That matters because
    'fieldvault dump' CLEARS records as it prints -- it runs last, after
    everything else is already on disk.

    The breadcrumb ring is only CRASH_LOG_DEPTH (5) entries deep and is
    overwritten by new checkpoints. Run this BEFORE rebooting or reflashing.

.EXAMPLE
    .\scripts\capture_crash.ps1
    .\scripts\capture_crash.ps1 -Port COM7 -OutFile run_aug18.log
#>

[CmdletBinding()]
param(
    [string] $Port,
    [string] $OutFile   = "spectre_crash_$(Get-Date -Format 'yyyyMMdd_HHmmss').log",
    [int]    $Baud      = 115200,
    [int]    $BootSeconds = 25,
    [switch] $SkipFieldVault,
    # ESP32-S3 native USB CDC: platformio.ini pins monitor_rts/dtr to 0.
    # If the board goes silent, retry with -Dtr to assert the line.
    [switch] $Dtr,
    [switch] $Rts
)

$ErrorActionPreference = 'Stop'

function Write-Transcript([string] $Text) {
    Add-Content -Path $script:OutPath -Value $Text -Encoding UTF8
}

# ── Port selection ───────────────────────────────────────────────────────────
$available = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
if (-not $Port) {
    if ($available.Count -eq 0) {
        Write-Error "No serial ports found. Is the board plugged in and powered?"
        return
    }
    if ($available.Count -gt 1) {
        Write-Host "Multiple ports found: $($available -join ', ')" -ForegroundColor Yellow
        Write-Host "Re-run with -Port <COMx> if the one chosen below is wrong." -ForegroundColor Yellow
    }
    $Port = $available[-1]
}
Write-Host "Using port $Port at $Baud baud." -ForegroundColor Cyan

$script:OutPath = if ([System.IO.Path]::IsPathRooted($OutFile)) {
    $OutFile
} else {
    Join-Path (Get-Location) $OutFile
}

Set-Content -Path $script:OutPath -Value @"
Spectre crash capture
  captured : $(Get-Date -Format 'u')
  port     : $Port @ $Baud
  host     : $env:COMPUTERNAME
  git      : $(git rev-parse --short HEAD 2>$null) $(git rev-parse --abbrev-ref HEAD 2>$null)
"@ -Encoding UTF8

# ── Open port ────────────────────────────────────────────────────────────────
$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.DtrEnable  = [bool]$Dtr
$sp.RtsEnable  = [bool]$Rts
$sp.ReadTimeout = 500
$sp.NewLine     = "`n"

try {
    $sp.Open()
} catch {
    Write-Error "Could not open $Port. Close any PlatformIO/PuTTY monitor holding it, then retry. ($_)"
    return
}

# Drain whatever is already buffered so the boot capture starts clean.
Start-Sleep -Milliseconds 200
try { $sp.DiscardInBuffer() } catch { }

function Read-For([int] $Seconds, [string] $Label) {
    Write-Transcript "`n===== $Label =====`n"
    $deadline = (Get-Date).AddSeconds($Seconds)
    $buf = New-Object System.Text.StringBuilder
    while ((Get-Date) -lt $deadline) {
        try {
            $chunk = $sp.ReadExisting()
        } catch {
            $chunk = ''
        }
        if ($chunk) {
            [void]$buf.Append($chunk)
            Write-Host -NoNewline $chunk
            # Flush completed lines to disk as they arrive.
            $text = $buf.ToString()
            $cut  = $text.LastIndexOf("`n")
            if ($cut -ge 0) {
                Write-Transcript $text.Substring(0, $cut)
                [void]$buf.Remove(0, $cut + 1)
            }
        } else {
            Start-Sleep -Milliseconds 50
        }
    }
    if ($buf.Length -gt 0) { Write-Transcript $buf.ToString() }
}

try {
    # ── Boot log ─────────────────────────────────────────────────────────────
    Write-Host ""
    Write-Host "  >> TAP THE RESET BUTTON ON THE BOARD NOW <<" -ForegroundColor Green
    Write-Host "     (capturing $BootSeconds s of boot output)" -ForegroundColor Green
    Write-Host ""
    Read-For $BootSeconds "BOOT LOG (reset -> crash breadcrumb ring)"

    # ── Console dumps ────────────────────────────────────────────────────────
    # Ordered least- to most-destructive; fieldvault dump clears as it prints.
    $commands = @(
        @{ Cmd = 'debug status';           Wait = 3  },
        @{ Cmd = 'spool diag';             Wait = 8  },
        @{ Cmd = 'spool audit';            Wait = 45 },
        @{ Cmd = 'spool quarantine list';  Wait = 8  },
        @{ Cmd = 'spool quarantine meta';  Wait = 8  },
        @{ Cmd = 'companion status';       Wait = 5  },
        @{ Cmd = 'debug dump';             Wait = 15 }
    )
    if (-not $SkipFieldVault) {
        $commands += @{ Cmd = 'fieldvault dump'; Wait = 12 }
    }

    foreach ($entry in $commands) {
        Write-Host "`n--- $($entry.Cmd) ---" -ForegroundColor Cyan
        if ($entry.Cmd -eq 'fieldvault dump') {
            Write-Host "    (this clears records as it prints; everything above is already saved)" -ForegroundColor Yellow
        }
        try { $sp.DiscardInBuffer() } catch { }
        $sp.Write("$($entry.Cmd)`r`n")
        Read-For $entry.Wait $entry.Cmd
    }
}
finally {
    if ($sp -and $sp.IsOpen) { $sp.Close() }
    $sp.Dispose()
}

# ── Summary ──────────────────────────────────────────────────────────────────
Write-Host "`n`nSaved transcript: $script:OutPath" -ForegroundColor Green

$crashLines = Select-String -Path $script:OutPath -Pattern '\[CRASH\?\]' -ErrorAction SilentlyContinue
if ($crashLines) {
    Write-Host "`nUnresolved crash checkpoints found:" -ForegroundColor Red
    $crashLines | ForEach-Object { Write-Host "  $($_.Line.Trim())" -ForegroundColor Red }
} else {
    Write-Host "`nNo [CRASH?] entries in the ring." -ForegroundColor Yellow
    Write-Host "If the run crashed, the ring (depth 5) may already be overwritten." -ForegroundColor Yellow
}

$panics = Select-String -Path $script:OutPath -Pattern 'Guru Meditation|abort\(\)|assert failed|Backtrace:|rst:0x' -ErrorAction SilentlyContinue
if ($panics) {
    Write-Host "`nPanic / reset-reason markers:" -ForegroundColor Red
    $panics | Select-Object -First 20 | ForEach-Object { Write-Host "  $($_.Line.Trim())" -ForegroundColor Red }
}
