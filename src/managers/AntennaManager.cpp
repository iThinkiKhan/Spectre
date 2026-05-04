

#include "AntennaManager.h"

#include <WiFi.h>
#include "../config.h"
#include "../core/DebugLog.h"

void AntennaManager::begin(bool externalDefault) {
    _external = externalDefault;

#if WIFI_ANTENNA_SWITCH_MODE == ANTENNA_SWITCH_GPIO
    if (WIFI_ANTENNA_CTRL_PIN < 0) {
        DLOG_WARN("RADIO", "GPIO switch mode selected but WIFI_ANTENNA_CTRL_PIN is unset");
        return;
    }

    pinMode(WIFI_ANTENNA_CTRL_PIN, OUTPUT);
    _available = true;
#elif WIFI_ANTENNA_SWITCH_MODE == ANTENNA_SWITCH_DUAL_GPIO
    if (WIFI_ANTENNA_GPIO_ANT0 < 0 || WIFI_ANTENNA_GPIO_ANT1 < 0) {
        DLOG_WARN("RADIO", "Dual antenna mode selected but ANT0/ANT1 GPIOs are unset");
        return;
    }

    if (WiFi.getMode() == WIFI_MODE_NULL) {
        WiFi.mode(WIFI_STA);
        delay(50);
    }
    _available = true;
#else
    DLOG_INFO("RADIO", "Antenna switching disabled in config.h");
    return;
#endif

    if (_available && !_apply(_external)) {
        _available = false;
        DLOG_WARN("RADIO", "Failed to apply initial antenna route");
    } else if (_available) {
        DLOG_INFO("RADIO", "Active antenna path: %s", label());
    }
}

bool AntennaManager::setExternal(bool external) {
    if (!_available) {
        return false;
    }
    if (_apply(external)) {
        _external = external;
        DLOG_INFO("RADIO", "Switched antenna to %s", label());
        return true;
    }
    DLOG_WARN("RADIO", "Failed to switch antenna to %s",
              external ? "EXTERNAL" : "INTERNAL");
    return false;
}

bool AntennaManager::toggle() {
    return setExternal(!_external);
}

const char* AntennaManager::label() const {
    return _external ? "EXTERNAL" : "INTERNAL";
}

bool AntennaManager::_apply(bool external) {
#if WIFI_ANTENNA_SWITCH_MODE == ANTENNA_SWITCH_GPIO
    digitalWrite(WIFI_ANTENNA_CTRL_PIN,
                 external ? WIFI_ANTENNA_EXTERNAL_LEVEL
                          : WIFI_ANTENNA_INTERNAL_LEVEL);
    return true;
#elif WIFI_ANTENNA_SWITCH_MODE == ANTENNA_SWITCH_DUAL_GPIO
    wifi_rx_ant_t rxMode = WIFI_ANTENNA_INTERNAL_PATH == 0 ?
                           WIFI_RX_ANT0 : WIFI_RX_ANT1;
    wifi_tx_ant_t txMode = WIFI_ANTENNA_INTERNAL_PATH == 0 ?
                           WIFI_TX_ANT0 : WIFI_TX_ANT1;

    if (external) {
        rxMode = WIFI_ANTENNA_EXTERNAL_PATH == 0 ?
                 WIFI_RX_ANT0 : WIFI_RX_ANT1;
        txMode = WIFI_ANTENNA_EXTERNAL_PATH == 0 ?
                 WIFI_TX_ANT0 : WIFI_TX_ANT1;
    }

    if (WiFi.getMode() == WIFI_MODE_NULL && !WiFi.mode(WIFI_STA)) {
        return false;
    }

    return WiFi.setDualAntennaConfig(
        (uint8_t)WIFI_ANTENNA_GPIO_ANT0,
        (uint8_t)WIFI_ANTENNA_GPIO_ANT1,
        rxMode,
        txMode);
#else
    (void)external;
    return false;
#endif
}



