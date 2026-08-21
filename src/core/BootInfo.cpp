
#include "BootInfo.h"

#include <Preferences.h>
#include <esp_idf_version.h>
#include <esp_system.h>

namespace BootInfo {
namespace {

constexpr const char* NS = "bootinfo";

Preferences  _prefs;
bool         _ready = false;
uint32_t     _bootCount = 0;
uint8_t      _resetReason = 0;
LastSession  _last;

const char* _rrName(uint8_t r) {
    switch (static_cast<esp_reset_reason_t>(r)) {
        case ESP_RST_POWERON:   return "POWER";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_DEEPSLEEP: return "WAKE";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
        // Names must fit the 100px header slot on the Boot Summary screen.
        case ESP_RST_USB:        return "USB";
        case ESP_RST_JTAG:       return "JTAG";
        case ESP_RST_EFUSE:      return "EFUSE";
        case ESP_RST_PWR_GLITCH: return "PWRGLITCH";
        case ESP_RST_CPU_LOCKUP: return "LOCKUP";
#endif
        default:                return "UNKNOWN";
    }
}

}  // namespace

void begin() {
    _resetReason = static_cast<uint8_t>(esp_reset_reason());
    if (!_prefs.begin(NS, false)) {
        _ready = false;
        _bootCount = 0;
        return;
    }
    _ready = true;

    _bootCount = _prefs.getUInt("boots", 0) + 1;
    _prefs.putUInt("boots", _bootCount);

    _last.valid             = _prefs.getUChar("ls_valid", 0) != 0;
    _last.records           = _prefs.getUInt("ls_rec", 0);
    _last.pendUploadMission = _prefs.getUInt("ls_pum", 0);
    _last.pendUploadNoise   = _prefs.getUInt("ls_pun", 0);
    _last.pendEnrichMission = _prefs.getUInt("ls_pem", 0);
    _last.pendEnrichNoise   = _prefs.getUInt("ls_pen", 0);
    _last.durationSec       = _prefs.getUInt("ls_dur", 0);
}

uint32_t bootCount() { return _bootCount; }
uint8_t resetReason() { return _resetReason; }
const char* resetReasonName() { return _rrName(_resetReason); }

bool resetWasCleanish() {
    switch (static_cast<esp_reset_reason_t>(_resetReason)) {
        case ESP_RST_POWERON:
        case ESP_RST_SW:
        case ESP_RST_DEEPSLEEP:
        case ESP_RST_EXT:
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
        // Host-initiated resets, not faults. The 1200bps-touch flash and any
        // USB replug land here; before these cases existed they fell to the
        // default and painted the Boot Summary header red on every flash.
        case ESP_RST_USB:
        case ESP_RST_JTAG:
#endif
            return true;
        default:
            return false;
    }
}

const LastSession& lastSession() { return _last; }

void snapshotCurrentSession(uint32_t records,
                            uint32_t pendUploadMission,
                            uint32_t pendUploadNoise,
                            uint32_t pendEnrichMission,
                            uint32_t pendEnrichNoise,
                            uint32_t durationSec) {
    if (!_ready) {
        return;
    }
    // NVS compares before writing, so unchanged values don't burn flash.
    _prefs.putUInt("ls_rec", records);
    _prefs.putUInt("ls_pum", pendUploadMission);
    _prefs.putUInt("ls_pun", pendUploadNoise);
    _prefs.putUInt("ls_pem", pendEnrichMission);
    _prefs.putUInt("ls_pen", pendEnrichNoise);
    _prefs.putUInt("ls_dur", durationSec);
    _prefs.putUChar("ls_valid", 1);
}

}  // namespace BootInfo
