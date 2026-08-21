# =============================================================================
# Spectre overnight analytics run
# =============================================================================
# Reboots the device, sweeps every console diagnostic, then soaks for hours
# while sampling health telemetry so we can spot leaks, stalls, and drift.
#
# Standalone: takes no Claude session to run. Everything lands in
#   diagnostics/overnight-<timestamp>/
#
#   powershell -ExecutionPolicy Bypass -File tools\overnight_test.ps1 -SoakHours 4
#
# The app CDC (MI_01) is single-access — close PuTTY/monitors before running.
# =============================================================================

param(
    [double]$SoakHours   = 4.0,
    [int]$SampleEveryMin = 5,
    [switch]$SkipReboot
)

$ErrorActionPreference = 'Continue'
$root    = Split-Path -Parent $PSScriptRoot
$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$outDir  = Join-Path $root "diagnostics\overnight-$stamp"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$bootLog = Join-Path $outDir '00-boot.log'
$cmdLog  = Join-Path $outDir '01-commands.log'
$soakLog = Join-Path $outDir '02-soak.log'
$csvOut  = Join-Path $outDir '03-samples.csv'
$sumOut  = Join-Path $outDir '04-summary.txt'
$runLog  = Join-Path $outDir 'run.log'

function Log {
    param([string]$Msg)
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $Msg
    Write-Output $line
    Add-Content -Path $runLog -Value $line
}

# ── Port discovery ────────────────────────────────────────────────────────────
# MI_01 = app TinyUSB CDC (the real console). MI_00 = USB-JTAG; opening it with
# DTR/RTS asserted resets the S3 into ROM download mode, so never touch it here.
function Get-AppPort {
    $dev = Get-CimInstance Win32_PnPEntity | Where-Object {
        $_.Name -match 'USB Serial Device \(COM\d+\)' -and
        $_.DeviceID -match 'VID_303A&PID_1001&MI_01'
    } | Select-Object -First 1
    if ($dev -and $dev.Name -match 'COM(\d+)') { return "COM$($Matches[1])" }
    return $null
}
function Get-DownloadPort {
    $dev = Get-CimInstance Win32_PnPEntity | Where-Object {
        $_.Name -match 'USB Serial Device \(COM\d+\)' -and
        $_.DeviceID -match 'VID_303A&PID_1001&MI_00'
    } | Select-Object -First 1
    if ($dev -and $dev.Name -match 'COM(\d+)') { return "COM$($Matches[1])" }
    return $null
}

# ── Serial helpers ────────────────────────────────────────────────────────────
function Read-Serial {
    param([int]$Seconds, [string]$Port, [string]$Path)
    if (-not $Port) { return '' }
    $sp = New-Object System.IO.Ports.SerialPort($Port, 115200)
    $sp.DtrEnable = $true; $sp.RtsEnable = $true; $sp.ReadTimeout = 2000
    $sb = New-Object System.Text.StringBuilder
    try {
        $sp.Open()
        $deadline = (Get-Date).AddSeconds($Seconds)
        while ((Get-Date) -lt $deadline) {
            try {
                $l = $sp.ReadLine()
                [void]$sb.AppendLine($l)
                if ($Path) { Add-Content -Path $Path -Value $l }
            } catch { }
        }
    } catch { Log "read error: $($_.Exception.Message)" }
    finally { if ($sp.IsOpen) { $sp.Close() } }
    return $sb.ToString()
}

function Send-Commands {
    param([string[]]$Commands, [int]$SettleSeconds = 6, [string]$Port, [string]$Path)
    if (-not $Port) { return '' }
    $sp = New-Object System.IO.Ports.SerialPort($Port, 115200)
    $sp.DtrEnable = $true; $sp.RtsEnable = $true; $sp.ReadTimeout = 1200
    $sb = New-Object System.Text.StringBuilder
    try {
        $sp.Open(); Start-Sleep -Milliseconds 400; $sp.DiscardInBuffer()
        foreach ($c in $Commands) {
            $hdr = "`n===== CMD: $c ====="
            [void]$sb.AppendLine($hdr)
            if ($Path) { Add-Content -Path $Path -Value $hdr }
            $sp.WriteLine($c)
            $deadline = (Get-Date).AddSeconds($SettleSeconds)
            while ((Get-Date) -lt $deadline) {
                try {
                    $l = $sp.ReadLine()
                    [void]$sb.AppendLine($l)
                    if ($Path) { Add-Content -Path $Path -Value $l }
                } catch { }
            }
        }
    } catch { Log "cmd error: $($_.Exception.Message)" }
    finally { if ($sp.IsOpen) { $sp.Close() } }
    return $sb.ToString()
}

# ── Phase 0: clean power-on reset ─────────────────────────────────────────────
Log "=== Spectre overnight run: soak=${SoakHours}h sample=${SampleEveryMin}min ==="
Log "output: $outDir"

$appPort = Get-AppPort
if (-not $appPort) { Log "FATAL: app CDC (MI_01) not found - is Spectre connected?"; exit 1 }
Log "app port: $appPort"

if (-not $SkipReboot) {
    Log "Phase 0: power-on reset via 1200bps touch + esptool hard-reset"
    try {
        $p = New-Object System.IO.Ports.SerialPort($appPort, 1200)
        $p.Open(); Start-Sleep -Milliseconds 200; $p.Close()
    } catch { Log "touch threw (expected): $($_.Exception.Message)" }
    Start-Sleep -Seconds 4

    $dlPort = Get-DownloadPort
    if ($dlPort) {
        Log "download port: $dlPort - issuing hard reset back into app"
        $env:PYTHONIOENCODING = 'utf-8'; $env:PYTHONUTF8 = '1'
        $py = "C:\Users\JimSchneider\.platformio\penv\Scripts\python.exe"
        & $py C:\pio\packages\tool-esptoolpy\esptool.py --chip esp32s3 --port $dlPort `
            --before default-reset --after watchdog-reset read-mac 2>&1 |
            Select-Object -Last 3 | ForEach-Object { Log "  esptool: $_" }
    } else {
        Log "WARN: no download port appeared; device may not have reset"
    }
    Start-Sleep -Seconds 5
    $appPort = Get-AppPort
    if (-not $appPort) { Log "FATAL: app port did not return after reset"; exit 1 }
    Log "app port back: $appPort"
}

Log "Phase 0b: capturing 120s of boot/early-run serial"
Read-Serial -Seconds 120 -Port $appPort -Path $bootLog | Out-Null

# ── Phase 1: diagnostic command sweep ─────────────────────────────────────────
Log "Phase 1: console diagnostic sweep"
$sweep = @(
    'debug status',
    'heap status',
    'time',
    'wio status',
    'companion status',
    'entity status',
    'spool count',
    'spool enrich',
    'spool diag',
    'spool audit',
    'spool quarantine list',
    'mesh status'
)
Send-Commands -Commands $sweep -SettleSeconds 8 -Port $appPort -Path $cmdLog | Out-Null

# ── Phase 2: soak with periodic sampling ──────────────────────────────────────
Log "Phase 2: soak ${SoakHours}h, sampling every ${SampleEveryMin}min"
"timestamp,uptime_ms,heap_free_kb,heap_min_kb,frag_pct,internal_free_kb,internal_largest_kb,psram_free_kb,pending_upload,pending_enrich,core0_pct,core1_pct,owner" |
    Set-Content -Path $csvOut

$soakEnd  = (Get-Date).AddHours($SoakHours)
$sampleNo = 0
while ((Get-Date) -lt $soakEnd) {
    $sampleNo++
    # Listen long enough to catch at least one 30s HEAP/HEALTH broadcast.
    $chunk = Read-Serial -Seconds 75 -Port $appPort -Path $soakLog

    foreach ($line in ($chunk -split "`r?`n")) {
        # [123456][I][HEAP] heap free=6247KB min=6229KB largest=6144KB frag=1%
        #   internalFree=61KB internalLargest=31KB psramFree=6186KB core=0/0%
        #   owner=WIFI_CAPTURE nets=6 pendingUpload=168 pendingEnrich=168
        if ($line -match '\[(\d+)\]\[I\]\[HEAP\] heap free=(\d+)KB min=(\d+)KB largest=\d+KB frag=(\d+)%.*internalFree=(\d+)KB internalLargest=(\d+)KB psramFree=(\d+)KB core=(\d+)/(\d+)%.*owner=(\w+).*pendingUpload=(\d+) pendingEnrich=(\d+)') {
            $row = '{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12}' -f `
                (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Matches[1], $Matches[2], $Matches[3],
                $Matches[4], $Matches[5], $Matches[6], $Matches[7], $Matches[11], $Matches[12],
                $Matches[8], $Matches[9], $Matches[10]
            Add-Content -Path $csvOut -Value $row
        }
    }

    # Surface anything alarming as it happens.
    $bad = $chunk -split "`r?`n" | Where-Object {
        $_ -match 'PANIC|Guru Meditation|Backtrace|assert failed|StoreProhibited|LoadProhibited|watchdog|WDT|E\]\[' }
    if ($bad) {
        Log "!!! sample $sampleNo flagged $($bad.Count) error line(s)"
        $bad | Select-Object -First 8 | ForEach-Object { Log "    $_" }
    }

    $remain = [math]::Round(($soakEnd - (Get-Date)).TotalMinutes, 0)
    Log "sample $sampleNo done; ${remain}min remaining"

    $sleepSec = ($SampleEveryMin * 60) - 75
    if ($sleepSec -gt 0 -and (Get-Date).AddSeconds($sleepSec) -lt $soakEnd) {
        Start-Sleep -Seconds $sleepSec
    }
}

# ── Phase 3: post-soak sweep (compare against Phase 1) ────────────────────────
Log "Phase 3: post-soak diagnostic sweep"
Add-Content -Path $cmdLog -Value "`n`n########## POST-SOAK ##########`n"
Send-Commands -Commands $sweep -SettleSeconds 8 -Port $appPort -Path $cmdLog | Out-Null

# ── Phase 4: analytics ────────────────────────────────────────────────────────
Log "Phase 4: analyzing"
$rows = @()
if (Test-Path $csvOut) { $rows = Import-Csv $csvOut }

$sum = New-Object System.Text.StringBuilder
function S { param([string]$t) [void]$sum.AppendLine($t) }

S "SPECTRE OVERNIGHT ANALYTICS"
S "==========================="
S "run started : $stamp"
S "finished    : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
S "soak hours  : $SoakHours"
S "samples     : $($rows.Count)"
S ""

if ($rows.Count -ge 2) {
    $first = $rows[0]; $last = $rows[-1]
    $dHeap = [int]$last.heap_free_kb     - [int]$first.heap_free_kb
    $dInt  = [int]$last.internal_free_kb - [int]$first.internal_free_kb
    $dPs   = [int]$last.psram_free_kb    - [int]$first.psram_free_kb
    $hours = ([int]$last.uptime_ms - [int]$first.uptime_ms) / 3600000.0

    S "MEMORY TREND (leak detection)"
    S "-----------------------------"
    S ("window            : {0:N2} h" -f $hours)
    S ("heap free         : {0} -> {1} KB   (delta {2:+#;-#;0} KB)" -f $first.heap_free_kb, $last.heap_free_kb, $dHeap)
    S ("internal free     : {0} -> {1} KB   (delta {2:+#;-#;0} KB)" -f $first.internal_free_kb, $last.internal_free_kb, $dInt)
    S ("psram free        : {0} -> {1} KB   (delta {2:+#;-#;0} KB)" -f $first.psram_free_kb, $last.psram_free_kb, $dPs)
    if ($hours -gt 0.5) {
        S ("heap drift/hour   : {0:N1} KB/h" -f ($dHeap / $hours))
        S ("internal drift/h  : {0:N1} KB/h" -f ($dInt / $hours))
    }
    $verdict = if ($hours -gt 1 -and ($dHeap / $hours) -lt -50) { "SUSPECT LEAK - investigate" }
               elseif ($hours -gt 1 -and ($dHeap / $hours) -lt -10) { "mild downward drift - watch" }
               else { "stable" }
    S ("verdict           : {0}" -f $verdict)
    S ""

    $intMin = ($rows | ForEach-Object { [int]$_.internal_free_kb } | Measure-Object -Minimum).Minimum
    $fragMax = ($rows | ForEach-Object { [int]$_.frag_pct } | Measure-Object -Maximum).Maximum
    S "PRESSURE EXTREMES"
    S "-----------------"
    S ("lowest internal free : {0} KB   (headroom warning below ~25 KB)" -f $intMin)
    S ("worst fragmentation  : {0} %" -f $fragMax)
    S ""

    S "BACKLOG TREND"
    S "-------------"
    S ("pendingUpload : {0} -> {1}" -f $first.pending_upload, $last.pending_upload)
    S ("pendingEnrich : {0} -> {1}" -f $first.pending_enrich, $last.pending_enrich)
    if ([int]$last.pending_upload -gt [int]$first.pending_upload) {
        S "note: upload backlog GREW during soak - capture is outrunning drain"
    }
    S ""

    S "RADIO OWNERSHIP DISTRIBUTION"
    S "----------------------------"
    $rows | Group-Object owner | Sort-Object Count -Descending | ForEach-Object {
        S ("  {0,-18} {1,4} samples ({2:N0}%)" -f $_.Name, $_.Count, (100.0 * $_.Count / $rows.Count))
    }
    S ""
} else {
    S "INSUFFICIENT SAMPLES - device may not have been broadcasting health lines."
    S ""
}

# Error census across every captured log.
S "ERROR / WARNING CENSUS"
S "----------------------"
$allLogs = @($bootLog, $cmdLog, $soakLog) | Where-Object { Test-Path $_ }
$errPatterns = @{
    'panic/crash'     = 'PANIC|Guru Meditation|Backtrace|StoreProhibited|LoadProhibited'
    'assert'          = 'assert failed'
    'watchdog'        = 'watchdog|WDT|task stall'
    'error-level log' = '\]\[E\]\['
    'warn-level log'  = '\]\[W\]\['
    'storage drop'    = 'drop\[full=[1-9]|p3Drop=[1-9]|p2Drop=[1-9]|p1Fail=[1-9]'
    'radio churn'     = 'deathloop|same-owner|radio kick'
    'enrich stall'    = 'enrich.*stall|drain.*stall'
}
foreach ($k in $errPatterns.Keys | Sort-Object) {
    $n = 0
    foreach ($f in $allLogs) { $n += (Select-String -Path $f -Pattern $errPatterns[$k] -AllMatches -ErrorAction SilentlyContinue).Count }
    S ("  {0,-18} {1}" -f $k, $n)
}
S ""

# Top distinct warning/error lines - the actionable list.
S "TOP DISTINCT WARN/ERROR LINES (what to work on)"
S "-----------------------------------------------"
$hits = @()
foreach ($f in $allLogs) {
    $hits += (Select-String -Path $f -Pattern '\]\[(W|E)\]\[' -ErrorAction SilentlyContinue |
              ForEach-Object { $_.Line -replace '^\[\d+\]', '' -replace '\d+', 'N' })
}
if ($hits.Count -gt 0) {
    $hits | Group-Object | Sort-Object Count -Descending | Select-Object -First 20 | ForEach-Object {
        S ("  {0,5}x {1}" -f $_.Count, $_.Name.Trim())
    }
} else { S "  (none)" }
S ""
S "ARTIFACTS"
S "---------"
S "  boot      : $bootLog"
S "  commands  : $cmdLog"
S "  soak      : $soakLog"
S "  samples   : $csvOut"

$sum.ToString() | Set-Content -Path $sumOut
Log "=== complete -> $sumOut ==="
Write-Output ""
Write-Output $sum.ToString()
