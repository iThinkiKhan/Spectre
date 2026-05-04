

#include "SettingsManager.h"

#include <ArduinoJson.h>
#include <cstring>

#include "../core/DebugLog.h"

namespace {
constexpr const char* PREF_NAMESPACE      = "spectre";
constexpr const char* KEY_SCHEMA_VERSION  = "schema_ver";
constexpr const char* KEY_PROVISIONING_VERSION = "prov_ver";
constexpr const char* KEY_DEVICE_NAME     = "dev_name";
constexpr const char* KEY_DEVICE_OWNER    = "dev_owner";
constexpr const char* KEY_DEVICE_VERSION  = "dev_ver";
constexpr const char* KEY_LORA_FREQ       = "lora_f";
constexpr const char* KEY_LORA_NET_ID     = "lora_nid";
constexpr const char* KEY_LORA_ADDR       = "lora_addr";
constexpr const char* KEY_LORA_SF         = "lora_sf";
constexpr const char* KEY_LORA_BW         = "lora_bw";
constexpr const char* KEY_LORA_CR         = "lora_cr";
constexpr const char* KEY_LORA_PREAMBLE   = "lora_pre";
constexpr const char* KEY_MQTT_BROKER     = "mq_host";
constexpr const char* KEY_MQTT_PORT       = "mq_port";
constexpr const char* KEY_MQTT_USER       = "mq_user";
constexpr const char* KEY_MQTT_PASSWORD   = "mq_pass";
constexpr const char* KEY_MQTT_TOPIC_BASE = "mq_topic";
constexpr const char* KEY_WIFI_COUNT      = "wifi_cnt";
constexpr const char* KEY_TIMEZONE        = "tz";
constexpr const char* KEY_ACCENT_HEX      = "accent";
constexpr const char* KEY_DISPLAY_TIMEOUT = "disp_to";
constexpr const char* KEY_BATTERY_CAPACITY = "bat_mah";
constexpr const char* KEY_NTP_1           = "ntp_1";
constexpr const char* KEY_NTP_2           = "ntp_2";
constexpr const char* KEY_USB_SERIAL_EN   = "usb_dbg_en";
constexpr const char* KEY_USB_SERIAL_LVL  = "usb_dbg_lv";
constexpr const char* KEY_USB_SERIAL_AREAS = "usb_dbg_ar";

void _wifiSsidKey(uint8_t index, char* out, size_t len) {
    snprintf(out, len, "wf_s_%u", static_cast<unsigned>(index));
}

void _wifiPassKey(uint8_t index, char* out, size_t len) {
    snprintf(out, len, "wf_p_%u", static_cast<unsigned>(index));
}

template <size_t N>
void _copyStringField(JsonVariantConst src, char (&dst)[N]) {
    const char* value = src | "";
    strlcpy(dst, value, N);
}

template <size_t N>
bool _stringUnset(const char (&value)[N]) {
    return value[0] == '\0';
}
}  // namespace

bool SettingsManager::begin() {
    if (_ready) return true;

    if (!_prefs.begin(PREF_NAMESPACE, false)) {
        DebugLog::configureUsbSerial(true,
                                     DEBUG_LEVEL_INFO,
                                     DEBUG_AREA_OPERATORS);
        DLOG_ERROR("SETTINGS", "Failed to open NVS namespace");
        return false;
    }

    _setDefaults(_settings);
    const uint16_t storedVersion = _prefs.getUShort(KEY_SCHEMA_VERSION, 0);
    const uint16_t storedProvisioningVersion =
        _prefs.getUShort(KEY_PROVISIONING_VERSION, 0);

    if (storedVersion == 0) {
        _settings.schemaVersion = CURRENT_SCHEMA_VERSION;
        _sanitize(_settings);
        _ready = _persist();
        if (_ready) {
            DebugLog::configureUsbSerial(_settings.usbSerialDebugEnabled,
                                         _settings.usbSerialDebugLevel,
                                         _settings.usbSerialDebugAreas);
        }
        if (_ready) {
            DLOG_INFO("SETTINGS", "Created settings from defaults");
        }
        return _ready;
    }

    _loadFromPreferences();
    _settings.schemaVersion = storedVersion;

    if (storedVersion != CURRENT_SCHEMA_VERSION) {
        if (!_migrate(storedVersion)) {
            return false;
        }
    }

    _sanitize(_settings);
    if (storedProvisioningVersion < SPECTRE_PROVISIONING_VERSION) {
        _applyProvisioningDefaults(_settings, SPECTRE_PROVISIONING_REPLACE_WIFI != 0);
        _sanitize(_settings);
        if (!_persist()) {
            return false;
        }
        DLOG_INFO("SETTINGS", "Applied provisioning version %u",
                  static_cast<unsigned>(SPECTRE_PROVISIONING_VERSION));
    }
    DebugLog::configureUsbSerial(_settings.usbSerialDebugEnabled,
                                 _settings.usbSerialDebugLevel,
                                 _settings.usbSerialDebugAreas);
    _ready = true;
    DLOG_INFO("SETTINGS",
              "Loaded schema v%u (usb=%d level=%c mask=0x%08lx)",
              static_cast<unsigned>(_settings.schemaVersion),
              _settings.usbSerialDebugEnabled ? 1 : 0,
              _settings.usbSerialDebugLevel,
              static_cast<unsigned long>(_settings.usbSerialDebugAreas));
    return true;
}

bool SettingsManager::apply(const RuntimeSettings& settings) {
    _settings = settings;
    _settings.schemaVersion = CURRENT_SCHEMA_VERSION;
    _sanitize(_settings);
    const bool ok = _persist();
    if (ok) {
        DebugLog::configureUsbSerial(_settings.usbSerialDebugEnabled,
                                     _settings.usbSerialDebugLevel,
                                     _settings.usbSerialDebugAreas);
    }
    return ok;
}

bool SettingsManager::setTimezone(const char* timezone) {
    RuntimeSettings next = _settings;
    strlcpy(next.timezone, timezone ? timezone : "", sizeof(next.timezone));
    return apply(next);
}

bool SettingsManager::setAccentHex(const char* accentHex) {
    RuntimeSettings next = _settings;
    strlcpy(next.accentHex, accentHex ? accentHex : "", sizeof(next.accentHex));
    return apply(next);
}

bool SettingsManager::setDisplayTimeoutMs(uint32_t timeoutMs) {
    RuntimeSettings next = _settings;
    next.displayTimeoutMs = timeoutMs;
    return apply(next);
}

bool SettingsManager::setBatteryCapacityMah(uint16_t capacityMah) {
    RuntimeSettings next = _settings;
    next.batteryCapacityMah = capacityMah;
    return apply(next);
}

bool SettingsManager::setNtpServers(const char* primary, const char* secondary) {
    RuntimeSettings next = _settings;
    strlcpy(next.ntpServer1, primary ? primary : "", sizeof(next.ntpServer1));
    strlcpy(next.ntpServer2, secondary ? secondary : "", sizeof(next.ntpServer2));
    return apply(next);
}

bool SettingsManager::setUsbSerialDebugEnabled(bool enabled) {
    RuntimeSettings next = _settings;
    next.usbSerialDebugEnabled = enabled;
    return apply(next);
}

bool SettingsManager::setUsbSerialDebugLevel(char level) {
    RuntimeSettings next = _settings;
    next.usbSerialDebugLevel = level;
    return apply(next);
}

bool SettingsManager::setUsbSerialDebugAreas(uint32_t areaMask) {
    RuntimeSettings next = _settings;
    next.usbSerialDebugAreas = areaMask;
    return apply(next);
}

bool SettingsManager::upsertWiFiNetwork(const char* ssid, const char* password) {
    if (!ssid || !ssid[0]) return false;

    RuntimeSettings next = _settings;
    for (uint8_t i = 0; i < next.wifiNetworkCount; i++) {
        if (strcmp(next.wifiNetworks[i].ssid, ssid) == 0) {
            strlcpy(next.wifiNetworks[i].password,
                    password ? password : "",
                    sizeof(next.wifiNetworks[i].password));
            return apply(next);
        }
    }

    if (next.wifiNetworkCount >= SETTINGS_WIFI_NETWORK_CAPACITY) {
        return false;
    }

    strlcpy(next.wifiNetworks[next.wifiNetworkCount].ssid,
            ssid,
            sizeof(next.wifiNetworks[next.wifiNetworkCount].ssid));
    strlcpy(next.wifiNetworks[next.wifiNetworkCount].password,
            password ? password : "",
            sizeof(next.wifiNetworks[next.wifiNetworkCount].password));
    next.wifiNetworkCount++;
    return apply(next);
}

bool SettingsManager::removeWiFiNetwork(const char* ssid) {
    if (!ssid || !ssid[0]) return false;

    RuntimeSettings next = _settings;
    for (uint8_t i = 0; i < next.wifiNetworkCount; i++) {
        if (strcmp(next.wifiNetworks[i].ssid, ssid) != 0) continue;

        for (uint8_t j = i; j + 1 < next.wifiNetworkCount; j++) {
            next.wifiNetworks[j] = next.wifiNetworks[j + 1];
        }
        next.wifiNetworks[next.wifiNetworkCount - 1] = WiFiCredential();
        next.wifiNetworkCount--;
        return apply(next);
    }

    return false;
}

void SettingsManager::_setDefaults(RuntimeSettings& settings) const {
    settings = RuntimeSettings();
    settings.schemaVersion = CURRENT_SCHEMA_VERSION;
    _applyProvisioningDefaults(settings, true);
}

void SettingsManager::_appendProvisionedWiFi(RuntimeSettings& settings,
                                             const char* ssid,
                                             const char* password) const {
    if (!ssid || !ssid[0]) return;
    if (settings.wifiNetworkCount >= SETTINGS_WIFI_NETWORK_CAPACITY) return;

    strlcpy(settings.wifiNetworks[settings.wifiNetworkCount].ssid,
            ssid,
            sizeof(settings.wifiNetworks[settings.wifiNetworkCount].ssid));
    strlcpy(settings.wifiNetworks[settings.wifiNetworkCount].password,
            password ? password : "",
            sizeof(settings.wifiNetworks[settings.wifiNetworkCount].password));
    settings.wifiNetworkCount++;
}

void SettingsManager::_applyProvisioningDefaults(RuntimeSettings& settings,
                                                 bool replaceWifi) const {
    strlcpy(settings.deviceName, SPECTRE_DEVICE_NAME, sizeof(settings.deviceName));
    strlcpy(settings.deviceOwner, SPECTRE_DEVICE_OWNER, sizeof(settings.deviceOwner));
    strlcpy(settings.deviceVersion, SPECTRE_DEVICE_VERSION, sizeof(settings.deviceVersion));

    settings.loraFrequency = SPECTRE_LORA_FREQUENCY;
    settings.loraNetworkId = SPECTRE_LORA_NETWORK_ID;
    settings.loraAddress = SPECTRE_LORA_ADDRESS;
    settings.loraSF = SPECTRE_LORA_SF;
    settings.loraBW = SPECTRE_LORA_BW;
    settings.loraCR = SPECTRE_LORA_CR;
    settings.loraPreamble = SPECTRE_LORA_PREAMBLE;

    strlcpy(settings.mqttBroker, SPECTRE_MQTT_BROKER, sizeof(settings.mqttBroker));
    settings.mqttPort = SPECTRE_MQTT_PORT;
    strlcpy(settings.mqttUser, SPECTRE_MQTT_USER, sizeof(settings.mqttUser));
    strlcpy(settings.mqttPassword, SPECTRE_MQTT_PASSWORD, sizeof(settings.mqttPassword));
    strlcpy(settings.mqttTopicBase, SPECTRE_MQTT_TOPIC_BASE, sizeof(settings.mqttTopicBase));

    strlcpy(settings.timezone, SPECTRE_TIMEZONE, sizeof(settings.timezone));
    strlcpy(settings.accentHex, SPECTRE_ACCENT_HEX, sizeof(settings.accentHex));
    settings.displayTimeoutMs = BACKLIGHT_TIMEOUT_MS;
    strlcpy(settings.ntpServer1, SPECTRE_NTP_SERVER_1, sizeof(settings.ntpServer1));
    strlcpy(settings.ntpServer2, SPECTRE_NTP_SERVER_2, sizeof(settings.ntpServer2));
    settings.usbSerialDebugEnabled = SPECTRE_USB_SERIAL_DEBUG_ENABLED;
    settings.usbSerialDebugLevel = SPECTRE_USB_SERIAL_DEBUG_LEVEL;
    settings.usbSerialDebugAreas = SPECTRE_USB_SERIAL_DEBUG_AREAS;

    if (replaceWifi) {
        settings.wifiNetworkCount = 0;
        for (uint8_t i = 0; i < SETTINGS_WIFI_NETWORK_CAPACITY; i++) {
            settings.wifiNetworks[i] = WiFiCredential();
        }
    }
    _appendProvisionedWiFi(settings, SPECTRE_WIFI_1_SSID, SPECTRE_WIFI_1_PASSWORD);
    _appendProvisionedWiFi(settings, SPECTRE_WIFI_2_SSID, SPECTRE_WIFI_2_PASSWORD);
    _appendProvisionedWiFi(settings, SPECTRE_WIFI_3_SSID, SPECTRE_WIFI_3_PASSWORD);
}

void SettingsManager::_sanitize(RuntimeSettings& settings) const {
    if (!settings.deviceName[0]) strlcpy(settings.deviceName, SPECTRE_DEVICE_NAME, sizeof(settings.deviceName));
    if (!settings.deviceOwner[0]) strlcpy(settings.deviceOwner, SPECTRE_DEVICE_OWNER, sizeof(settings.deviceOwner));
    if (!settings.deviceVersion[0]) strlcpy(settings.deviceVersion, SPECTRE_DEVICE_VERSION, sizeof(settings.deviceVersion));
    if (settings.loraFrequency == 0) settings.loraFrequency = SPECTRE_LORA_FREQUENCY;
    if (settings.loraNetworkId == 0) settings.loraNetworkId = SPECTRE_LORA_NETWORK_ID;
    if (settings.loraAddress == 0) settings.loraAddress = SPECTRE_LORA_ADDRESS;
    if (settings.loraSF < 6 || settings.loraSF > 12) settings.loraSF = SPECTRE_LORA_SF;
    if (settings.loraBW > 9) settings.loraBW = SPECTRE_LORA_BW;
    if (settings.loraCR == 0 || settings.loraCR > 4) settings.loraCR = SPECTRE_LORA_CR;
    if (settings.loraPreamble == 0) settings.loraPreamble = SPECTRE_LORA_PREAMBLE;
    if (settings.mqttPort == 0) settings.mqttPort = SPECTRE_MQTT_PORT;
    if (!settings.mqttTopicBase[0]) strlcpy(settings.mqttTopicBase, SPECTRE_MQTT_TOPIC_BASE, sizeof(settings.mqttTopicBase));

    if (settings.wifiNetworkCount > SETTINGS_WIFI_NETWORK_CAPACITY) {
        settings.wifiNetworkCount = SETTINGS_WIFI_NETWORK_CAPACITY;
    }

    uint8_t compacted = 0;
    for (uint8_t i = 0; i < settings.wifiNetworkCount; i++) {
        if (!settings.wifiNetworks[i].ssid[0]) continue;
        if (compacted != i) {
            settings.wifiNetworks[compacted] = settings.wifiNetworks[i];
            settings.wifiNetworks[i] = WiFiCredential();
        }
        compacted++;
    }
    settings.wifiNetworkCount = compacted;

    if (!settings.timezone[0]) strlcpy(settings.timezone, SPECTRE_TIMEZONE, sizeof(settings.timezone));
    if (strlen(settings.accentHex) != 7 || settings.accentHex[0] != '#') {
        strlcpy(settings.accentHex, SPECTRE_ACCENT_HEX, sizeof(settings.accentHex));
    }
    if (settings.displayTimeoutMs < 1000UL) settings.displayTimeoutMs = BACKLIGHT_TIMEOUT_MS;
    if (settings.batteryCapacityMah < POWER_BATTERY_CAPACITY_MIN_MAH ||
        settings.batteryCapacityMah > POWER_BATTERY_CAPACITY_MAX_MAH) {
        settings.batteryCapacityMah = POWER_BATTERY_CAPACITY_DEFAULT_MAH;
    }
    if (!settings.ntpServer1[0]) strlcpy(settings.ntpServer1, SPECTRE_NTP_SERVER_1, sizeof(settings.ntpServer1));
    if (!settings.ntpServer2[0]) strlcpy(settings.ntpServer2, SPECTRE_NTP_SERVER_2, sizeof(settings.ntpServer2));
    settings.usbSerialDebugLevel = sanitizeDebugLevel(settings.usbSerialDebugLevel);
    if (settings.usbSerialDebugAreas == 0) {
        settings.usbSerialDebugAreas = SPECTRE_USB_SERIAL_DEBUG_AREAS;
    }
}

void SettingsManager::_loadFromPreferences() {
    _setDefaults(_settings);
    _prefs.getString(KEY_DEVICE_NAME, _settings.deviceName, sizeof(_settings.deviceName));
    _prefs.getString(KEY_DEVICE_OWNER, _settings.deviceOwner, sizeof(_settings.deviceOwner));
    _prefs.getString(KEY_DEVICE_VERSION, _settings.deviceVersion, sizeof(_settings.deviceVersion));
    _settings.loraFrequency = _prefs.getULong(KEY_LORA_FREQ, _settings.loraFrequency);
    _settings.loraNetworkId = _prefs.getUShort(KEY_LORA_NET_ID, _settings.loraNetworkId);
    _settings.loraAddress = _prefs.getUShort(KEY_LORA_ADDR, _settings.loraAddress);
    _settings.loraSF = _prefs.getUChar(KEY_LORA_SF, _settings.loraSF);
    _settings.loraBW = _prefs.getUChar(KEY_LORA_BW, _settings.loraBW);
    _settings.loraCR = _prefs.getUChar(KEY_LORA_CR, _settings.loraCR);
    _settings.loraPreamble = _prefs.getUChar(KEY_LORA_PREAMBLE, _settings.loraPreamble);
    _prefs.getString(KEY_MQTT_BROKER, _settings.mqttBroker, sizeof(_settings.mqttBroker));
    _settings.mqttPort = _prefs.getUShort(KEY_MQTT_PORT, _settings.mqttPort);
    _prefs.getString(KEY_MQTT_USER, _settings.mqttUser, sizeof(_settings.mqttUser));
    _prefs.getString(KEY_MQTT_PASSWORD, _settings.mqttPassword, sizeof(_settings.mqttPassword));
    _prefs.getString(KEY_MQTT_TOPIC_BASE, _settings.mqttTopicBase, sizeof(_settings.mqttTopicBase));
    _settings.wifiNetworkCount = _prefs.getUChar(KEY_WIFI_COUNT, 0);
    if (_settings.wifiNetworkCount > SETTINGS_WIFI_NETWORK_CAPACITY) {
        _settings.wifiNetworkCount = SETTINGS_WIFI_NETWORK_CAPACITY;
    }
    for (uint8_t i = 0; i < _settings.wifiNetworkCount; i++) {
        char key[12] = {};
        _wifiSsidKey(i, key, sizeof(key));
        _prefs.getString(key, _settings.wifiNetworks[i].ssid, sizeof(_settings.wifiNetworks[i].ssid));
        _wifiPassKey(i, key, sizeof(key));
        _prefs.getString(key, _settings.wifiNetworks[i].password, sizeof(_settings.wifiNetworks[i].password));
    }
    _prefs.getString(KEY_TIMEZONE, _settings.timezone, sizeof(_settings.timezone));
    _prefs.getString(KEY_ACCENT_HEX, _settings.accentHex, sizeof(_settings.accentHex));
    _settings.displayTimeoutMs = _prefs.getULong(KEY_DISPLAY_TIMEOUT, _settings.displayTimeoutMs);
    _settings.batteryCapacityMah =
        _prefs.getUShort(KEY_BATTERY_CAPACITY, _settings.batteryCapacityMah);
    _prefs.getString(KEY_NTP_1, _settings.ntpServer1, sizeof(_settings.ntpServer1));
    _prefs.getString(KEY_NTP_2, _settings.ntpServer2, sizeof(_settings.ntpServer2));
    _settings.usbSerialDebugEnabled =
        _prefs.getBool(KEY_USB_SERIAL_EN, _settings.usbSerialDebugEnabled);
    _settings.usbSerialDebugLevel =
        static_cast<char>(_prefs.getUChar(KEY_USB_SERIAL_LVL,
                                          static_cast<uint8_t>(_settings.usbSerialDebugLevel)));
    _settings.usbSerialDebugAreas =
        _prefs.getULong(KEY_USB_SERIAL_AREAS, _settings.usbSerialDebugAreas);
}

bool SettingsManager::_persist() {
    _settings.schemaVersion = CURRENT_SCHEMA_VERSION;
    _sanitize(_settings);

    bool ok = true;
    auto putStringOk = [&](const char* key, const char* value) -> bool {
        const char* safeValue = value ? value : "";
        const size_t len = strlen(safeValue);
        const size_t written = _prefs.putString(key, safeValue);
        if (written == len) return true;
        if (len == 0) {
            return _prefs.getString(key, "__spectre_unset__") == "";
        }
        return false;
    };

    ok &= _prefs.putUShort(KEY_SCHEMA_VERSION, _settings.schemaVersion) > 0;
    ok &= _prefs.putUShort(KEY_PROVISIONING_VERSION, SPECTRE_PROVISIONING_VERSION) > 0;
    ok &= putStringOk(KEY_DEVICE_NAME, _settings.deviceName);
    ok &= putStringOk(KEY_DEVICE_OWNER, _settings.deviceOwner);
    ok &= putStringOk(KEY_DEVICE_VERSION, _settings.deviceVersion);
    ok &= _prefs.putULong(KEY_LORA_FREQ, _settings.loraFrequency) > 0;
    ok &= _prefs.putUShort(KEY_LORA_NET_ID, _settings.loraNetworkId) > 0;
    ok &= _prefs.putUShort(KEY_LORA_ADDR, _settings.loraAddress) > 0;
    ok &= _prefs.putUChar(KEY_LORA_SF, _settings.loraSF) > 0;
    ok &= _prefs.putUChar(KEY_LORA_BW, _settings.loraBW) > 0;
    ok &= _prefs.putUChar(KEY_LORA_CR, _settings.loraCR) > 0;
    ok &= _prefs.putUChar(KEY_LORA_PREAMBLE, _settings.loraPreamble) > 0;
    ok &= putStringOk(KEY_MQTT_BROKER, _settings.mqttBroker);
    ok &= _prefs.putUShort(KEY_MQTT_PORT, _settings.mqttPort) > 0;
    ok &= putStringOk(KEY_MQTT_USER, _settings.mqttUser);
    ok &= putStringOk(KEY_MQTT_PASSWORD, _settings.mqttPassword);
    ok &= putStringOk(KEY_MQTT_TOPIC_BASE, _settings.mqttTopicBase);
    ok &= _prefs.putUChar(KEY_WIFI_COUNT, _settings.wifiNetworkCount) > 0;

    for (uint8_t i = 0; i < SETTINGS_WIFI_NETWORK_CAPACITY; i++) {
        char key[12] = {};
        _wifiSsidKey(i, key, sizeof(key));
        if (i < _settings.wifiNetworkCount) ok &= putStringOk(key, _settings.wifiNetworks[i].ssid);
        else if (_prefs.isKey(key)) _prefs.remove(key);
        _wifiPassKey(i, key, sizeof(key));
        if (i < _settings.wifiNetworkCount) ok &= putStringOk(key, _settings.wifiNetworks[i].password);
        else if (_prefs.isKey(key)) _prefs.remove(key);
    }

    ok &= putStringOk(KEY_TIMEZONE, _settings.timezone);
    ok &= putStringOk(KEY_ACCENT_HEX, _settings.accentHex);
    ok &= _prefs.putULong(KEY_DISPLAY_TIMEOUT, _settings.displayTimeoutMs) > 0;
    ok &= _prefs.putUShort(KEY_BATTERY_CAPACITY, _settings.batteryCapacityMah) > 0;
    ok &= putStringOk(KEY_NTP_1, _settings.ntpServer1);
    ok &= putStringOk(KEY_NTP_2, _settings.ntpServer2);
    ok &= _prefs.putBool(KEY_USB_SERIAL_EN, _settings.usbSerialDebugEnabled);
    ok &= _prefs.putUChar(KEY_USB_SERIAL_LVL,
                          static_cast<uint8_t>(_settings.usbSerialDebugLevel)) > 0;
    ok &= _prefs.putULong(KEY_USB_SERIAL_AREAS, _settings.usbSerialDebugAreas) > 0;

    if (!ok) {
        DLOG_ERROR("SETTINGS", "Persist failed");
        return false;
    }
    DLOG_INFO("SETTINGS", "Saved schema v%u",
              static_cast<unsigned>(_settings.schemaVersion));
    return true;
}

bool SettingsManager::_migrate(uint16_t storedVersion) {
    RuntimeSettings migrated = _settings;
    switch (storedVersion) {
        case 1:
        case 2: break;
        case 3:
            // The old default was too aggressive; preserve explicit overrides.
            if (migrated.displayTimeoutMs == 15000UL) {
                migrated.displayTimeoutMs = 30000UL;
            }
            break;
        default: _setDefaults(migrated); break;
    }
    migrated.schemaVersion = CURRENT_SCHEMA_VERSION;
    _sanitize(migrated);
    _settings = migrated;
    DLOG_WARN("SETTINGS", "Migrating schema v%u -> v%u",
              static_cast<unsigned>(storedVersion),
              static_cast<unsigned>(CURRENT_SCHEMA_VERSION));
    return _persist();
}


