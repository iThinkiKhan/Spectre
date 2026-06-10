


#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "../config.h"
#include "../SecretsConfig.h"
#include "../core/DebugConfig.h"

static constexpr uint8_t SETTINGS_WIFI_NETWORK_CAPACITY = 8;

struct WiFiCredential {
    char ssid[33] = "";
    char password[65] = "";
};

struct RuntimeSettings {
    uint16_t schemaVersion = 4;
    char deviceName[32] = SPECTRE_DEVICE_NAME;
    char deviceOwner[32] = SPECTRE_DEVICE_OWNER;
    char deviceVersion[16] = SPECTRE_DEVICE_VERSION;
    uint32_t loraFrequency = SPECTRE_LORA_FREQUENCY;
    uint16_t loraNetworkId = SPECTRE_LORA_NETWORK_ID;
    uint16_t loraAddress = SPECTRE_LORA_ADDRESS;
    uint8_t loraSF = SPECTRE_LORA_SF;
    uint8_t loraBW = SPECTRE_LORA_BW;
    uint8_t loraCR = SPECTRE_LORA_CR;
    uint8_t loraPreamble = SPECTRE_LORA_PREAMBLE;
    char mqttBroker[64] = SPECTRE_MQTT_BROKER;
    uint16_t mqttPort = SPECTRE_MQTT_PORT;
    char mqttUser[32] = SPECTRE_MQTT_USER;
    char mqttPassword[64] = SPECTRE_MQTT_PASSWORD;
    char mqttTopicBase[32] = SPECTRE_MQTT_TOPIC_BASE;
    uint8_t wifiNetworkCount = 0;
    WiFiCredential wifiNetworks[SETTINGS_WIFI_NETWORK_CAPACITY] = {};
    char timezone[40] = SPECTRE_TIMEZONE;
    char accentHex[8] = SPECTRE_ACCENT_HEX;
    uint32_t displayTimeoutMs = BACKLIGHT_TIMEOUT_MS;
    uint16_t batteryCapacityMah = POWER_BATTERY_CAPACITY_DEFAULT_MAH;
    char ntpServer1[64] = SPECTRE_NTP_SERVER_1;
    char ntpServer2[64] = SPECTRE_NTP_SERVER_2;
    bool usbSerialDebugEnabled = SPECTRE_USB_SERIAL_DEBUG_ENABLED;
    char usbSerialDebugLevel = SPECTRE_USB_SERIAL_DEBUG_LEVEL;
    uint32_t usbSerialDebugAreas = SPECTRE_USB_SERIAL_DEBUG_AREAS;
};

class SettingsManager {
public:
    static SettingsManager& getInstance() {
        static SettingsManager instance;
        return instance;
    }

    bool begin();
    bool isReady() const { return _ready; }

    const RuntimeSettings& get() const { return _settings; }
    RuntimeSettings snapshot() const { return _settings; }

    bool apply(const RuntimeSettings& settings);
    bool setTimezone(const char* timezone);
    bool setAccentHex(const char* accentHex);
    bool setDisplayTimeoutMs(uint32_t timeoutMs);
    bool setBatteryCapacityMah(uint16_t capacityMah);
    bool setNtpServers(const char* primary, const char* secondary);
    bool setUsbSerialDebugEnabled(bool enabled);
    bool setUsbSerialDebugLevel(char level);
    bool setUsbSerialDebugAreas(uint32_t areaMask);
    bool upsertWiFiNetwork(const char* ssid, const char* password);
    bool removeWiFiNetwork(const char* ssid);

private:
    SettingsManager() = default;

    static constexpr uint16_t CURRENT_SCHEMA_VERSION = 4;

    void _setDefaults(RuntimeSettings& settings) const;
    void _applyProvisioningDefaults(RuntimeSettings& settings, bool replaceWifi) const;
    void _appendProvisionedWiFi(RuntimeSettings& settings, const char* ssid, const char* password) const;
    void _sanitize(RuntimeSettings& settings) const;
    void _loadFromPreferences();
    bool _persist();
    bool _migrate(uint16_t storedVersion);

    Preferences _prefs;
    RuntimeSettings _settings;
    bool _ready = false;
};

#define SETTINGS SettingsManager::getInstance()




