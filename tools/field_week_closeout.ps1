param(
    [string]$Port = 'COM13',
    [int]$EnrichTimeoutMinutes = 45,
    [int]$UploadTimeoutMinutes = 45,
    [int]$FieldUploadTimeoutMinutes = 30
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$logPath = Join-Path $root "diagnostics\field-week-closeout-$stamp.log"
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
    $captured = [System.Text.StringBuilder]::new()
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
        if ($chunk) {
            $out.Write($chunk)
            [void]$captured.Append($chunk)
        }
        Start-Sleep -Milliseconds 50
    }
    return $captured.ToString()
}

function Send-FieldCommand {
    param([string]$Command, [double]$SettleSeconds = 4)
    Write-Output "closeout command: $Command"
    $out.WriteLine("`r`n===== CMD: $Command =====")
    if (-not $script:sp -or -not $script:sp.IsOpen) { Open-FieldSerial }
    try {
        $script:sp.WriteLine($Command)
    } catch {
        Open-FieldSerial
        $script:sp.WriteLine($Command)
    }
    return Pump-Serial $SettleSeconds
}

function Get-PendingEnrichment {
    param([string]$Text)
    $matches = [regex]::Matches($Text, 'pendingEnrichTotal=(\d+)')
    if ($matches.Count -eq 0) { return $null }
    return [int64]$matches[$matches.Count - 1].Groups[1].Value
}

function Get-CompanionWork {
    param([string]$Text)
    $matches = [regex]::Matches($Text, '\[COMP\] phone=\S+ work=(\S+)')
    if ($matches.Count -eq 0) { return $null }
    return $matches[$matches.Count - 1].Groups[1].Value
}

$enrichmentComplete = $false
$uploadComplete = $false
$fieldUploadComplete = $false

try {
    Open-FieldSerial
    $out.WriteLine("=== field week closeout opened $(Get-Date -Format o) port=$Port ===")
    [void](Pump-Serial 5)

    Write-Output 'closeout stage: baseline diagnostics and FieldVault capture'
    foreach ($command in @(
        'debug status',
        'heap status',
        'time',
        'wio status',
        'companion status',
        'entity status',
        'mesh status',
        'crash log',
        'ble rxdiag'
    )) {
        [void](Send-FieldCommand $command 4)
    }
    [void](Send-FieldCommand 'spool count' 20)
    # A large pending backlog can immediately schedule the automatic BLE probe.
    # Release it before the remaining storage-owner diagnostics so those
    # commands are not silently deferred behind the radio lease.
    [void](Send-FieldCommand 'companion cancel' 8)
    [void](Send-FieldCommand 'spool enrich' 20)
    [void](Send-FieldCommand 'spool diag' 8)
    [void](Send-FieldCommand 'spool quarantine list' 5)
    [void](Send-FieldCommand 'spool quarantine meta' 5)
    [void](Send-FieldCommand 'fieldvault dump' 30)

    Write-Output 'closeout stage: phone enrichment'
    [void](Send-FieldCommand 'companion enrich' 5)
    $enrichDeadline = (Get-Date).AddMinutes($EnrichTimeoutMinutes)
    $enrichRetries = 0
    while ((Get-Date) -lt $enrichDeadline) {
        [void](Pump-Serial 15)
        $status = Send-FieldCommand 'companion status' 5
        $pending = Get-PendingEnrichment $status
        $work = Get-CompanionWork $status
        Write-Output "enrichment progress: work=$work pending=$pending"
        if ($null -ne $pending -and $pending -eq 0 -and $work -eq 'IDLE') {
            $enrichmentComplete = $true
            break
        }
        if ($null -ne $pending -and $pending -gt 0 -and $work -eq 'IDLE' -and $enrichRetries -lt 3) {
            $enrichRetries++
            Write-Output "enrichment retry: $enrichRetries/3"
            [void](Send-FieldCommand 'companion enrich' 5)
        }
    }

    Write-Output "closeout stage: mission upload (enrichmentComplete=$enrichmentComplete)"
    [void](Send-FieldCommand 'upload resume' 3)
    $uploadDeadline = (Get-Date).AddMinutes($UploadTimeoutMinutes)
    while ((Get-Date) -lt $uploadDeadline) {
        $status = Send-FieldCommand 'upload start' 5
        if ($status -match '\[UPLOAD\] no pending records') {
            $uploadComplete = $true
            break
        }
        [void](Pump-Serial 20)
        Write-Output 'mission upload progress: awaiting next state check'
    }

    Write-Output "closeout stage: FieldVault live-cursor upload (missionUploadComplete=$uploadComplete)"
    $fieldDeadline = (Get-Date).AddMinutes($FieldUploadTimeoutMinutes)
    while ((Get-Date) -lt $fieldDeadline) {
        $status = Send-FieldCommand 'fieldvault upload' 5
        if ($status -match '\[FIELD\] no pending records') {
            $fieldUploadComplete = $true
            break
        }
        [void](Pump-Serial 25)
        Write-Output 'FieldVault upload progress: awaiting next 8-record batch'
    }

    Write-Output 'closeout stage: post-transfer diagnostic audit'
    foreach ($command in @(
        'heap status',
        'time',
        'wio status',
        'companion status',
        'entity status',
        'mesh status'
    )) {
        [void](Send-FieldCommand $command 4)
    }
    [void](Send-FieldCommand 'spool audit' 25)
    [void](Send-FieldCommand 'spool count' 20)
    [void](Send-FieldCommand 'spool enrich' 20)
    [void](Send-FieldCommand 'spool diag' 8)
    [void](Send-FieldCommand 'spool quarantine list' 5)
    [void](Send-FieldCommand 'crash log' 5)
    [void](Send-FieldCommand 'ble rxdiag' 5)
    [void](Send-FieldCommand 'fieldvault dump' 30)

    if ($fieldUploadComplete) {
        Write-Output 'closeout stage: clearing addressed FieldVault'
        [void](Send-FieldCommand 'fieldvault clear' 8)
        [void](Send-FieldCommand 'fieldvault dump' 8)
    } else {
        Write-Warning 'FieldVault still has a pending live cursor; leaving it intact.'
    }

    $out.WriteLine("=== SUMMARY enrichmentComplete=$enrichmentComplete uploadComplete=$uploadComplete fieldUploadComplete=$fieldUploadComplete ===")
    $out.WriteLine("=== field week closeout closed $(Get-Date -Format o) ===")
} finally {
    $out.Close()
    if ($sp) {
        if ($sp.IsOpen) { $sp.Close() }
        $sp.Dispose()
    }
}

Write-Output "done -> $logPath"
Write-Output "result: enrichmentComplete=$enrichmentComplete uploadComplete=$uploadComplete fieldUploadComplete=$fieldUploadComplete"
