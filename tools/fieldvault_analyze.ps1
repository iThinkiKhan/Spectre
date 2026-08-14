<#
.SYNOPSIS
  Non-destructively dumps and analyzes Spectre FieldVault records.

.EXAMPLE
  .\tools\fieldvault_analyze.ps1
  .\tools\fieldvault_analyze.ps1 -Port COM13 -IncludeSpoolAudit
  .\tools\fieldvault_analyze.ps1 -InputPath .\diagnostics\fieldvault-raw.log

The firmware's `fieldvault dump` command retains all records. This tool writes
the raw serial transcript, a machine-readable summary, and a compact text
report beneath diagnostics/fieldvault-<timestamp>.
#>

[CmdletBinding()]
param(
    [string]$Port,
    [string]$InputPath,
    [string]$OutputDirectory,
    [switch]$IncludeSpoolAudit,
    [int]$SettleSeconds = 12
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot "diagnostics\fieldvault-$stamp"
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
[System.IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null

if ($InputPath) {
    $resolvedInput = (Resolve-Path -LiteralPath $InputPath).Path
    $raw = [System.IO.File]::ReadAllText($resolvedInput)
} else {
    . (Join-Path $PSScriptRoot 'spectre_serial.ps1')
    if (-not $Port) {
        $Port = Get-SpectrePort
    }
    if (-not $Port) {
        throw 'Spectre app CDC port (MI_01) not found.'
    }

    $commands = @('companion status', 'fieldvault dump')
    if ($IncludeSpoolAudit) {
        $commands += 'spool audit'
    }
    $raw = Invoke-SpectreCommand -Command $commands -SettleSeconds $SettleSeconds -Port $Port
}

$rawPath = Join-Path $OutputDirectory 'fieldvault-raw.log'
[System.IO.File]::WriteAllText($rawPath, $raw, [System.Text.UTF8Encoding]::new($false))

$records = [System.Collections.Generic.List[object]]::new()
$parseErrors = [System.Collections.Generic.List[string]]::new()
foreach ($line in ($raw -split "`r?`n")) {
    $candidate = $line.Trim()
    if (-not ($candidate.StartsWith('{') -and $candidate.EndsWith('}'))) {
        continue
    }
    try {
        $records.Add(($candidate | ConvertFrom-Json))
    } catch {
        $parseErrors.Add($candidate)
    }
}

$ordered = @($records | Where-Object { $null -ne $_.seq } | Sort-Object { [uint64]$_.seq })
$typeCounts = [ordered]@{}
foreach ($group in ($records | Group-Object type | Sort-Object Name)) {
    $typeCounts[$group.Name] = $group.Count
}

$sequenceGaps = [System.Collections.Generic.List[object]]::new()
$duplicateSequences = [System.Collections.Generic.List[uint64]]::new()
for ($i = 1; $i -lt $ordered.Count; $i++) {
    $previous = [uint64]$ordered[$i - 1].seq
    $current = [uint64]$ordered[$i].seq
    if ($current -eq $previous) {
        $duplicateSequences.Add($current)
    } elseif ($current -gt ($previous + 1)) {
        $sequenceGaps.Add([pscustomobject]@{
            after = $previous
            before = $current
            missing = $current - $previous - 1
        })
    }
}

$boots = @($records | Where-Object { $_.type -eq 'boot' })
$bootReadySamples = @($records | Where-Object {
    $_.type -eq 'power_sample' -and $_.reason -eq 'boot'
})
$slowBoots = @($bootReadySamples | Where-Object { [uint64]$_.ts_ms -ge 8000 })
$crashes = @($records | Where-Object {
    $_.type -eq 'crash' -or $_.type -eq 'reset_crash'
})
$power = @($records | Where-Object { $_.type -eq 'power_sample' })
$usbPower = @($power | Where-Object { $_.source -eq 'usb' -and $null -ne $_.mv })
$runSamples = @($records | Where-Object { $_.type -eq 'run_sample' })
$latestRun = $runSamples | Sort-Object { [uint64]$_.seq } | Select-Object -Last 1
$liveHealth = $null
$liveHealthMatches = [regex]::Matches(
    $raw,
    '\[HEALTH\]\s+radio=(\S+)\s+wifi=(\d+)\s+ble=(\d+)\s+gps=(\d+)\s+nets=(\d+)\s+pendingUpload=(\d+)\s+pendingEnrich=(\d+)'
)
if ($liveHealthMatches.Count -gt 0) {
    $match = $liveHealthMatches[$liveHealthMatches.Count - 1]
    $liveHealth = [ordered]@{
        radio = $match.Groups[1].Value
        wifi = [int]$match.Groups[2].Value
        ble = [int]$match.Groups[3].Value
        gps = [int]$match.Groups[4].Value
        networks = [int]$match.Groups[5].Value
        pendingUpload = [uint64]$match.Groups[6].Value
        pendingEnrichment = [uint64]$match.Groups[7].Value
    }
}
$auditProblems = @($records | Where-Object {
    $_.type -match '^fs_audit_' -and
    ($_.type -match 'invalid|unknown|action' -or $_.result -match 'fail|invalid|abort')
})

# A full power loss clears RTC breadcrumbs. Flag boots whose preceding retained
# run sample was in native BLE ownership; this is evidence of a rail/link event,
# not proof of a firmware crash.
$bleAdjacentBoots = [System.Collections.Generic.List[object]]::new()
foreach ($bootLike in ($ordered | Where-Object {
    ($_.type -eq 'boot') -or ($_.type -eq 'power_sample' -and $_.reason -eq 'boot')
})) {
    $seq = [uint64]$bootLike.seq
    $previousRun = $ordered |
        Where-Object { [uint64]$_.seq -lt $seq -and $_.type -eq 'run_sample' } |
        Select-Object -Last 1
    if ($previousRun -and [int]$previousRun.owner -eq 7) {
        $bleAdjacentBoots.Add([pscustomobject]@{
            bootSeq = $seq
            priorRunSeq = [uint64]$previousRun.seq
            priorPending = [uint64]$previousRun.pending
            priorInternalHeapKb = [uint64]$previousRun.int_kb
        })
    }
}

$minUsbMv = $null
$maxUsbMv = $null
if ($usbPower.Count -gt 0) {
    $minUsbMv = [int](($usbPower | Measure-Object mv -Minimum).Minimum)
    $maxUsbMv = [int](($usbPower | Measure-Object mv -Maximum).Maximum)
}

$summary = [ordered]@{
    generatedAt = (Get-Date).ToUniversalTime().ToString('o')
    source = if ($InputPath) { $resolvedInput } else { "serial:$Port" }
    nonDestructive = $true
    recordCount = $records.Count
    parseErrorCount = $parseErrors.Count
    sequence = [ordered]@{
        first = if ($ordered.Count) { [uint64]$ordered[0].seq } else { $null }
        last = if ($ordered.Count) { [uint64]$ordered[-1].seq } else { $null }
        gaps = @($sequenceGaps)
        duplicates = @($duplicateSequences)
    }
    typeCounts = $typeCounts
    boot = [ordered]@{
        recordCount = $boots.Count
        readySampleCount = $bootReadySamples.Count
        slowCount = $slowBoots.Count
        slowThresholdMs = 8000
        slow = @($slowBoots | ForEach-Object {
            [pscustomobject]@{ seq = $_.seq; readyMs = $_.ts_ms; millivolts = $_.mv; source = $_.source }
        })
        bleAdjacent = @($bleAdjacentBoots)
    }
    crash = [ordered]@{
        count = $crashes.Count
        records = @($crashes | ForEach-Object {
            [pscustomobject]@{ seq = $_.seq; phase = $_.phase; reason = $_.reason; reset = $_.reset }
        })
    }
    power = [ordered]@{
        sampleCount = $power.Count
        usbSampleCount = $usbPower.Count
        minUsbMv = $minUsbMv
        maxUsbMv = $maxUsbMv
    }
    latestRun = if ($latestRun) {
        [ordered]@{
            seq = $latestRun.seq
            uptimeSeconds = $latestRun.uptime_s
            owner = $latestRun.owner
            pendingUpload = $latestRun.pending
            pendingEnrichment = $latestRun.enrich
            internalHeapKb = $latestRun.int_kb
        }
    } else { $null }
    liveHealth = $liveHealth
    auditProblemCount = $auditProblems.Count
}

$jsonPath = Join-Path $OutputDirectory 'summary.json'
[System.IO.File]::WriteAllText(
    $jsonPath,
    ($summary | ConvertTo-Json -Depth 8),
    [System.Text.UTF8Encoding]::new($false)
)

$reportLines = [System.Collections.Generic.List[string]]::new()
$reportLines.Add('Spectre FieldVault analysis')
$reportLines.Add("Records: $($records.Count); parse errors: $($parseErrors.Count)")
$reportLines.Add("Sequence: $($summary.sequence.first)..$($summary.sequence.last); gaps: $($sequenceGaps.Count); duplicates: $($duplicateSequences.Count)")
$reportLines.Add("Boot records: $($boots.Count); ready samples: $($bootReadySamples.Count); slow ready (>=8 s): $($slowBoots.Count); BLE-adjacent boot evidence: $($bleAdjacentBoots.Count)")
$reportLines.Add("Crashes: $($crashes.Count); audit problem records: $($auditProblems.Count)")
$reportLines.Add("USB power samples: $($usbPower.Count); range: $minUsbMv..$maxUsbMv mV")
if ($latestRun) {
    $reportLines.Add("Latest run: seq=$($latestRun.seq) uptime=$($latestRun.uptime_s)s owner=$($latestRun.owner) pending=$($latestRun.pending) enrich=$($latestRun.enrich) intHeap=$($latestRun.int_kb)KB")
}
if ($liveHealth) {
    $reportLines.Add("Live health after commands: radio=$($liveHealth.radio) pending=$($liveHealth.pendingUpload) enrich=$($liveHealth.pendingEnrichment) nets=$($liveHealth.networks)")
}
if ($slowBoots.Count) {
    $reportLines.Add('Slow boots: ' + (($slowBoots | ForEach-Object { "seq=$($_.seq):$($_.ts_ms)ms/$($_.mv)mV" }) -join ', '))
}
if ($crashes.Count) {
    $reportLines.Add('Crash records: ' + (($crashes | ForEach-Object { "seq=$($_.seq):$($_.phase)/$($_.reason)" }) -join ', '))
}
$reportPath = Join-Path $OutputDirectory 'summary.txt'
[System.IO.File]::WriteAllLines($reportPath, $reportLines, [System.Text.UTF8Encoding]::new($false))

$reportLines
Write-Output "Raw: $rawPath"
Write-Output "JSON: $jsonPath"
