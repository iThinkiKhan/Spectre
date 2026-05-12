


param(
    [string]$ProjectRoot = "."
)

$ErrorActionPreference = "Stop"
Set-Location $ProjectRoot

$checks = @(
    @{ Name = "UI tag cmd enum";               Path = "src/core/SpectreState.h";      Pattern = "UI_CMD_REQUEST_TAG" },
    @{ Name = "UI save-location cmd enum";     Path = "src/core/SpectreState.h";      Pattern = "UI_CMD_REQUEST_SAVE_LOCATION" },
    @{ Name = "Tag prompt dispatch";           Path = "src/main.cpp";                 Pattern = "Tag this session:" },
    @{ Name = "Save prompt dispatch";          Path = "src/main.cpp";                 Pattern = "Save location:" },
    @{ Name = "Known locations load";          Path = "src/main.cpp";                 Pattern = "loadKnownLocations(" },
    @{ Name = "WIFI_SCAN to MODE_WIFI_RECON";  Path = "src/main.cpp";                 Pattern = "case RADIO_WIFI_SCAN:" },
    @{ Name = "MASCOT_SCANNING alias";         Path = "src/ui/MascotState.h";         Pattern = "MASCOT_SCANNING = MASCOT_WIFI_RECON" },
    @{ Name = "MQTT upload lease ready";       Path = "src/managers/MQTTManager.cpp"; Pattern = "uploadLeaseReady(bool force)" },
    @{ Name = "MQTT requestDump";              Path = "src/managers/MQTTManager.cpp"; Pattern = "bool MQTTManager::requestDump(bool force)" },
    @{ Name = "MQTT dump complete release";    Path = "src/managers/MQTTManager.cpp"; Pattern = "dump_complete" },
    @{ Name = "MQTT dump failed release";      Path = "src/managers/MQTTManager.cpp"; Pattern = "dump_failed" },
    @{ Name = "MQTT session_tag";              Path = "src/managers/MQTTManager.cpp"; Pattern = 'doc["session_tag"] = tag;' },
    @{ Name = "Debrief TAG label";             Path = "src/managers/DisplayManager.cpp"; Pattern = '_makeLabel(_sysContent, "TAG"' },
    @{ Name = "OpenDroneID include";           Path = "src/managers/WiFiManager.h";   Pattern = "#include <opendroneid.h>" },
    @{ Name = "RemoteID parser";               Path = "src/managers/WiFiManager.cpp"; Pattern = "void WiFiManager::_parseRemoteID(" },
    @{ Name = "Single-msg type nibble";        Path = "src/managers/WiFiManager.cpp"; Pattern = "msgType = (payload[0] >> 4) & 0x0F;" },
    @{ Name = "BasicID/Location usefulness";   Path = "src/managers/WiFiManager.cpp"; Pattern = "if (!hasId && !hasLoc) return;" },
    @{ Name = "Schema PATH_HC22000";           Path = "src/data/Schema.h";            Pattern = "PATH_HC22000" },
    @{ Name = "Upstream OpenDroneID header";   Path = "lib/opendroneid-core-c/include/opendroneid.h"; Pattern = "ODID_PROTOCOL_VERSION" },
    @{ Name = "Upstream OpenDroneID source";   Path = "lib/opendroneid-core-c/src/opendroneid.c"; Pattern = "odid_initBasicIDData" }
)

$missing = @()

foreach ($c in $checks) {
    if (-not (Test-Path $c.Path)) {
        Write-Host ("[MISSING FILE] {0}: {1}" -f $c.Name, $c.Path) -ForegroundColor Red
        $missing += $c
        continue
    }

    $hit = Select-String -Path $c.Path -Pattern $c.Pattern -SimpleMatch
    if ($hit) {
        $first = $hit | Select-Object -First 1
        Write-Host ("[OK] {0} -> {1}:{2}" -f $c.Name, $first.Path, $first.LineNumber) -ForegroundColor Green
    } else {
        Write-Host ("[MISSING] {0} -> {1}" -f $c.Name, $c.Pattern) -ForegroundColor Yellow
        $missing += $c
    }
}

if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host ("Restore audit failed: {0} checkpoint(s) missing." -f $missing.Count) -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Restore audit passed: all checkpoints present." -ForegroundColor Green
exit 0




