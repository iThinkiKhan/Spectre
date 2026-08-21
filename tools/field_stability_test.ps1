param(
    [double]$SoakMinutes = 6.0,
    [string]$Port = 'COM13',
    [int]$ProbeCycles = 1
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$logPath = Join-Path $root "diagnostics\field-stability-$stamp.log"
$out = [System.IO.StreamWriter]::new($logPath, $false)
$out.AutoFlush = $true

$sp = $null

function Open-FieldSerial {
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline) {
        try {
            if ($script:sp) {
                if ($script:sp.IsOpen) { $script:sp.Close() }
                $script:sp.Dispose()
            }
            $script:sp = [System.IO.Ports.SerialPort]::new($Port, 115200)
            $script:sp.DtrEnable = $true
            $script:sp.RtsEnable = $false
            $script:sp.ReadTimeout = 200
            $script:sp.Open()
            $out.WriteLine("=== serial attached $(Get-Date -Format o) port=$Port ===")
            return
        } catch {
            Start-Sleep -Milliseconds 500
        }
    }
    throw "Unable to attach Spectre serial port $Port within 30 seconds"
}

function Pump-Serial {
    param([double]$Seconds)
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        try {
            if (-not $script:sp -or -not $script:sp.IsOpen) {
                Open-FieldSerial
            }
            $chunk = $script:sp.ReadExisting()
        } catch {
            $out.WriteLine("=== serial detached $(Get-Date -Format o): $($_.Exception.Message) ===")
            Open-FieldSerial
            $chunk = ''
        }
        if ($chunk) { $out.Write($chunk) }
        Start-Sleep -Milliseconds 50
    }
}

function Send-TestCommand {
    param([string]$Command, [double]$SettleSeconds = 4)
    Write-Output "field command: $Command"
    $out.WriteLine("`r`n===== CMD: $Command =====")
    if (-not $script:sp -or -not $script:sp.IsOpen) { Open-FieldSerial }
    try {
        $script:sp.WriteLine($Command)
    } catch {
        Open-FieldSerial
        $script:sp.WriteLine($Command)
    }
    Pump-Serial $SettleSeconds
}

try {
    Open-FieldSerial
    $out.WriteLine("=== field stability opened $(Get-Date -Format o) port=$Port soak=${SoakMinutes}m ===")
    Pump-Serial 5

    foreach ($command in @(
        'debug status',
        'heap status',
        'time',
        'wio status',
        'companion status',
        'entity status',
        'mesh status'
    )) {
        Send-TestCommand $command 4
    }

    # Large-spool diagnostics are intentionally serialized. Each command can
    # hold the storage exclusive window for several seconds, so fixed 4-second
    # command spacing would queue later diagnostics before cleanup completes.
    Send-TestCommand 'spool count' 15
    Write-Output 'field stage: post-count maintenance'
    Pump-Serial 20
    Send-TestCommand 'spool diag' 6
    Send-TestCommand 'spool enrich' 20
    Send-TestCommand 'spool diag' 6
    Send-TestCommand 'spool quarantine list' 5

    Send-TestCommand 'spool audit' 15
    Write-Output 'field stage: post-audit maintenance'
    Pump-Serial 20

    # Exercise a Wi-Fi ownership window and return to field capture.
    Send-TestCommand 'time sync' 25
    Send-TestCommand 'time' 5

    # Exercise BLE acquisition/probe and the cleanup handoff back to Wi-Fi.
    # Multiple cycles stress scan stop/restart and ownership transitions.
    for ($probeCycle = 1; $probeCycle -le $ProbeCycles; $probeCycle++) {
        Write-Output "field stage: companion cycle $probeCycle/$ProbeCycles"
        Send-TestCommand 'companion probe' 25
        Send-TestCommand 'companion status' 5
        Send-TestCommand 'companion offload prep' 5
        Send-TestCommand 'companion cancel' 8
    }

    $out.WriteLine("`r`n===== STABILITY SOAK ${SoakMinutes}m =====")
    $soakEnd = (Get-Date).AddMinutes($SoakMinutes)
    $nextProgress = Get-Date
    while ((Get-Date) -lt $soakEnd) {
        Pump-Serial 10
        if ((Get-Date) -ge $nextProgress) {
            $remaining = [Math]::Max(0, [Math]::Round(($soakEnd - (Get-Date)).TotalMinutes, 1))
            Write-Output "soak remaining ${remaining}m"
            $nextProgress = (Get-Date).AddSeconds(30)
        }
    }

    foreach ($command in @(
        'debug status',
        'heap status',
        'time',
        'wio status',
        'companion status',
        'entity status',
        'spool count',
        'spool enrich',
        'spool diag',
        'mesh status'
    )) {
        Send-TestCommand $command 4
    }

    $out.WriteLine("=== field stability closed $(Get-Date -Format o) ===")
} finally {
    $out.Close()
    if ($sp) {
        if ($sp.IsOpen) { $sp.Close() }
        $sp.Dispose()
    }
}

Write-Output "done -> $logPath"
