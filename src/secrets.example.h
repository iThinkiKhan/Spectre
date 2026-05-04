

#pragma once

// Copy to src/secrets.h for local provisioning. The real src/secrets.h should
// stay ignored by git.

#ifndef SPECTRE_PROVISIONING_VERSION
#define SPECTRE_PROVISIONING_VERSION 0
#endif

#ifndef SPECTRE_PROVISIONING_REPLACE_WIFI
#define SPECTRE_PROVISIONING_REPLACE_WIFI 1
#endif

#ifndef SPECTRE_DEVICE_NAME
#define SPECTRE_DEVICE_NAME "Spectre"
#endif

#ifndef SPECTRE_DEVICE_OWNER
#define SPECTRE_DEVICE_OWNER "operator"
#endif

#ifndef SPECTRE_DEVICE_VERSION
#define SPECTRE_DEVICE_VERSION "0.1.0"
#endif

#ifndef SPECTRE_MQTT_BROKER
#define SPECTRE_MQTT_BROKER ""
#endif

#ifndef SPECTRE_MQTT_PORT
#define SPECTRE_MQTT_PORT 1883
#endif

#ifndef SPECTRE_MQTT_USER
#define SPECTRE_MQTT_USER ""
#endif

#ifndef SPECTRE_MQTT_PASSWORD
#define SPECTRE_MQTT_PASSWORD ""
#endif

#ifndef SPECTRE_MQTT_SENSOR_ID
#define SPECTRE_MQTT_SENSOR_ID "spectre_field"
#endif

#ifndef SPECTRE_MQTT_TOPIC_BASE
#define SPECTRE_MQTT_TOPIC_BASE "aether/sensor"
#endif

#ifndef SPECTRE_WIFI_1_SSID
#define SPECTRE_WIFI_1_SSID ""
#endif

#ifndef SPECTRE_WIFI_1_PASSWORD
#define SPECTRE_WIFI_1_PASSWORD ""
#endif

#ifndef SPECTRE_WIFI_2_SSID
#define SPECTRE_WIFI_2_SSID ""
#endif

#ifndef SPECTRE_WIFI_2_PASSWORD
#define SPECTRE_WIFI_2_PASSWORD ""
#endif

#ifndef SPECTRE_WIFI_3_SSID
#define SPECTRE_WIFI_3_SSID ""
#endif

#ifndef SPECTRE_WIFI_3_PASSWORD
#define SPECTRE_WIFI_3_PASSWORD ""
#endif

#ifndef SPECTRE_LORA_FREQUENCY
#define SPECTRE_LORA_FREQUENCY 915000000UL
#endif

#ifndef SPECTRE_LORA_NETWORK_ID
#define SPECTRE_LORA_NETWORK_ID 6
#endif

#ifndef SPECTRE_LORA_ADDRESS
#define SPECTRE_LORA_ADDRESS 1
#endif

#ifndef SPECTRE_LORA_SF
#define SPECTRE_LORA_SF 9
#endif

#ifndef SPECTRE_LORA_BW
#define SPECTRE_LORA_BW 7
#endif

#ifndef SPECTRE_LORA_CR
#define SPECTRE_LORA_CR 1
#endif

#ifndef SPECTRE_LORA_PREAMBLE
#define SPECTRE_LORA_PREAMBLE 12
#endif

#ifndef SPECTRE_TIMEZONE
#define SPECTRE_TIMEZONE "CST6CDT,M3.2.0,M11.1.0"
#endif

#ifndef SPECTRE_NTP_SERVER_1
#define SPECTRE_NTP_SERVER_1 "time.cloudflare.com"
#endif

#ifndef SPECTRE_NTP_SERVER_2
#define SPECTRE_NTP_SERVER_2 "pool.ntp.org"
#endif

#ifndef SPECTRE_ACCENT_HEX
#define SPECTRE_ACCENT_HEX "#00F0FF"
#endif

#ifndef SPECTRE_USB_SERIAL_DEBUG_ENABLED
#define SPECTRE_USB_SERIAL_DEBUG_ENABLED true
#endif

#ifndef SPECTRE_USB_SERIAL_DEBUG_LEVEL
#define SPECTRE_USB_SERIAL_DEBUG_LEVEL DEBUG_LEVEL_INFO
#endif

#ifndef SPECTRE_USB_SERIAL_DEBUG_AREAS
#define SPECTRE_USB_SERIAL_DEBUG_AREAS DEBUG_AREA_OPERATORS
#endif

#ifndef SPECTRE_BLE_TARGET_DEVICE_NAME
#define SPECTRE_BLE_TARGET_DEVICE_NAME "SpectrePhone"
#endif

// P-256 app-layer BLE security.
//
// Device private key: 32-byte P-256 scalar as 64 hex chars. Keep only in the
// ignored src/secrets.h and protect device flash in production.
// Public keys: uncompressed P-256 points as 130 hex chars, starting with 04.
#ifndef SPECTRE_DEVICE_PRIVATE_KEY_HEX
#define SPECTRE_DEVICE_PRIVATE_KEY_HEX ""
#endif

#ifndef SPECTRE_DEVICE_PUBLIC_KEY_HEX
#define SPECTRE_DEVICE_PUBLIC_KEY_HEX ""
#endif

#ifndef SPECTRE_PHONE_PUBLIC_KEY_HEX
#define SPECTRE_PHONE_PUBLIC_KEY_HEX ""
#endif

