
#pragma once

// Top level
#define F_SESSION       "session"
#define F_DEVICE        "device"
#define F_VERSION       "version"
#define F_MODE          "mode"
#define F_TIMESTAMP     "ts"
#define F_TIMESTAMP_ISO "ts_iso"
#define F_PACKETS       "packets"
#define F_SCANS         "scans"
#define F_PROBES        "probes"
#define F_START         "start"
#define F_START_ISO     "start_iso"
#define F_END           "end"
#define F_END_ISO       "end_iso"
#define F_TIME_SOURCE   "time_source"
#define F_ENRICHED_TS   "enriched_ts"
#define F_ENRICHED_TS_ISO "enriched_ts_iso"
#define F_UPLOADED_TS   "uploaded_ts"
#define F_UPLOADED_TS_ISO "uploaded_ts_iso"

// GPS
#define F_GPS           "gps"
#define F_LAT           "lat"
#define F_LON           "lon"
#define F_ACCURACY      "accuracy"
#define F_VALID         "valid"

// LoRa
#define F_ADDRESS       "addr"
#define F_RSSI          "rssi"
#define F_SNR           "snr"
#define F_FREQUENCY     "freq"
#define F_PAYLOAD       "payload"
#define F_PAYLOAD_HEX   "payload_hex"
#define F_SF            "sf"
#define F_BW            "bw"
#define F_CR            "cr"
#define F_PREAMBLE      "preamble"

// WiFi
#define F_SSID          "ssid"
#define F_BSSID         "bssid"
#define F_CHANNEL       "channel"
#define F_ENCRYPTION    "encryption"
#define F_PASSWORD      "password"

// MQTT
#define F_BROKER        "broker"
#define F_PORT          "port"
#define F_USER          "user"
#define F_TOPIC_BASE    "topicBase"

// Device config
#define F_NAME          "name"
#define F_OWNER         "owner"
#define F_NETWORKS      "networks"
#define F_LORA          "lora"
#define F_MQTT          "mqtt"
#define F_WIFI          "wifi"

// MQTT topics
#define TOPIC_LORA_PACKET   "spectre/lora/packet"
#define TOPIC_WIFI_SCAN     "spectre/wifi/scan"
#define TOPIC_WIFI_PROBE    "spectre/wifi/probe"
#define TOPIC_SYSTEM_STATUS "spectre/system/status"
#define TOPIC_OFFLOAD       "spectre/system/offload"

// File paths
#define PATH_LOGS           "/logs"
#define PATH_SESSIONS       "/logs/sessions.json"
#define PATH_EVENTS         "/events"
#define PATH_EVENT_COUNTER  "/events/counter.txt"
#define PATH_EVENT_META     "/events/meta.json"
#define PATH_EXPORTS        "/exports"
#define PATH_EXPORT_INDEX   "/exports/index.jsonl"
#define PATH_HC22000        "/exports/captures.hc22000"
#define PATH_PMKID_DIR      "/pmkids"
#define PATH_BADUSB_DIR              "/config/vault/badusb"
#define PATH_BADUSB_INDEX            "/config/vault/badusb/index.json"


