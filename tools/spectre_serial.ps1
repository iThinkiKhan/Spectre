# Spectre serial console helper.
#
# Sends console commands to the running firmware over the app CDC port and
# captures the reply. Used for smoke tests and the overnight analytics run.
#
#   . .\tools\spectre_serial.ps1
#   Invoke-SpectreCommand -Command 'heap status' -SettleSeconds 3
#
# NOTE: only ever open the MI_01 interface (the app CDC). Opening the MI_00
# USB-JTAG port with DTR/RTS asserted resets the S3 into ROM download mode.
#
# NOTE: open with BOTH DtrEnable and RtsEnable true. Opening the app CDC with
# RTS deasserted drops the S3 straight into ROM download mode — the app CDC
# (MI_01) vanishes and the ROM's MI_00 + MI_02 pair appears in its place, so
# every subsequent command fails with "port not found" until esptool resets it.

function Get-SpectrePort {
    $dev = Get-CimInstance Win32_PnPEntity | Where-Object {
        $_.Name -match 'USB Serial Device \(COM\d+\)' -and
        $_.DeviceID -match 'VID_303A&PID_1001&MI_01'
    } | Select-Object -First 1
    if (-not $dev) { return $null }
    if ($dev.Name -match 'COM(\d+)') { return "COM$($Matches[1])" }
    return $null
}

function Invoke-SpectreCommand {
    param(
        [Parameter(Mandatory = $true)][string[]]$Command,
        [int]$SettleSeconds = 4,
        [string]$Port
    )
    if (-not $Port) { $Port = Get-SpectrePort }
    if (-not $Port) { return "ERROR: Spectre app CDC port (MI_01) not found" }

    $sp = New-Object System.IO.Ports.SerialPort($Port, 115200)
    $sp.DtrEnable = $true; $sp.RtsEnable = $true
    $sp.ReadTimeout = 1200
    $sb = New-Object System.Text.StringBuilder
    try {
        $sp.Open()
        Start-Sleep -Milliseconds 400
        $sp.DiscardInBuffer()
        foreach ($c in $Command) {
            $sp.WriteLine($c)
            $deadline = (Get-Date).AddSeconds($SettleSeconds)
            while ((Get-Date) -lt $deadline) {
                try { [void]$sb.AppendLine($sp.ReadLine()) } catch { }
            }
        }
    } catch {
        return "ERROR: $($_.Exception.Message)"
    } finally {
        if ($sp.IsOpen) { $sp.Close() }
    }
    return $sb.ToString()
}

function Read-SpectreSerial {
    param([int]$Seconds = 30, [string]$Port)
    if (-not $Port) { $Port = Get-SpectrePort }
    if (-not $Port) { return "ERROR: Spectre app CDC port (MI_01) not found" }

    $sp = New-Object System.IO.Ports.SerialPort($Port, 115200)
    $sp.DtrEnable = $true; $sp.RtsEnable = $true
    $sp.ReadTimeout = 2000
    $sb = New-Object System.Text.StringBuilder
    try {
        $sp.Open()
        $deadline = (Get-Date).AddSeconds($Seconds)
        while ((Get-Date) -lt $deadline) {
            try { [void]$sb.AppendLine($sp.ReadLine()) } catch { }
        }
    } catch {
        return "ERROR: $($_.Exception.Message)"
    } finally {
        if ($sp.IsOpen) { $sp.Close() }
    }
    return $sb.ToString()
}
