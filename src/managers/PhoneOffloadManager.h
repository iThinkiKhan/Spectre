#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

#include "../core/StorageExclusiveWindow.h"
#include "protocol/CompanionProtocol.h"

// Pulls pending MQTT-ready records from StorageManager a record at a time,
// then exposes each record as bounded chunks on the authenticated phone
// command channel. The manager never owns a radio; callers must already hold
// RADIO_BLE_GPS. Persistent upload watermarks advance only after ACK.
class PhoneOffloadManager {
public:
    static PhoneOffloadManager& getInstance();

    bool begin(CmdOffloadBeginResponseV1& out);
    bool next(const CmdOffloadNextRequestV1& req,
              uint8_t* out,
              size_t cap,
              size_t& outLen);
    bool ack(const CmdOffloadAckRequestV1& req,
             CmdOffloadAckResponseV1& out);
    bool end(uint8_t transferId, const char* reason = nullptr);
    void abandonPreparation(const char* reason = nullptr);
    void expireIfStale();
    // Runs queued index preparation from TaskHardware, the sole persistent
    // storage owner. Kept separate from begin() so the BLE command response
    // can be returned before the bounded LittleFS scan starts.
    void servicePreparation();
    bool startWifiBulk(const uint8_t* payload, size_t len);
    bool resumeWifiBulkEarly();
    void discardRetainedWifiBulkResume();
    void tickWifiBulk();
    bool wifiBulkActive() const { return _wifiBulkState != WIFI_BULK_IDLE; }

    bool active() const { return _active; }
    bool preparationPending() const;
    uint8_t transferId() const { return _transferId; }

private:
    enum PrepState : uint8_t {
        PREP_IDLE = 0,
        PREP_RUNNING,
        PREP_READY,
        PREP_FAILED
    };
    enum WifiBulkState : uint8_t {
        WIFI_BULK_IDLE = 0,
        WIFI_BULK_DELAY,
        WIFI_BULK_LEASE,
        WIFI_BULK_BOOT_PREP,
        WIFI_BULK_CONNECT_WIFI,
        WIFI_BULK_CONNECT_TCP,
        WIFI_BULK_TRANSFER
    };
    struct WifiStagedRecord {
        uint32_t eventId = 0;
        uint8_t lane = 0xff;
        uint16_t bytes = 0;
        char sessionId[64] = {};
    };

    PhoneOffloadManager() = default;
    PhoneOffloadManager(const PhoneOffloadManager&) = delete;
    PhoneOffloadManager& operator=(const PhoneOffloadManager&) = delete;

    bool loadNextRecord(bool callerOwnsStorageWindow = false);
    bool startPreparation();
    void runPreparation();
    bool flushCheckpointIfDue(bool force, bool callerOwnsStorageWindow = false);
    bool buildRecordBody(ArduinoJson::JsonObjectConst record,
                         const String& sessionId);
    void clearRecord();
    bool reset(const char* reason);
    bool sendWifiBatch();
    void finishWifiBulk(bool success, const char* reason);
    bool ensureRecordBody();
    bool ensureWifiStaging();
    void releaseWifiStaging();

    bool _active = false;
    uint8_t _transferId = 0;
    uint32_t _lastActivityMs = 0;
    uint32_t _lastAckEventId = 0;
    uint32_t _beginPendingTotal = 0;
    uint32_t _beginIndexedTotal = 0;
    uint32_t _lastCheckpointMs = 0;
    uint32_t _transferStartMs = 0;
    uint32_t _transferAckedBytes = 0;
    uint32_t _transferAckedRecords = 0;
    uint16_t _acksSinceCheckpoint = 0;
    uint8_t _beginFlags = 0;
    volatile uint8_t _prepState = PREP_IDLE;
    volatile bool _prepAbandonRequested = false;
    bool _storageBatchOpen = false;

    std::vector<String> _sessions;
    size_t _sessionIndex = 0;
    uint32_t _sinceId = 0;
    bool _sinceInitialized = false;

    bool _recordReady = false;
    uint32_t _eventId = 0;
    uint8_t _lane = 0xFF;
    uint8_t _sessionLen = 0;
    uint8_t _topicLen = 0;
    uint16_t _bodyLen = 0;
    char _sessionId[64] = {};
    // Cold transfer payload. Keep control state internal, but do not reserve
    // this 1792-byte body in scarce SRAM while the device is capturing.
    uint8_t* _body = nullptr;
    WifiBulkState _wifiBulkState = WIFI_BULK_IDLE;
    uint32_t _wifiBulkDeadlineMs = 0;
    uint32_t _wifiBulkNextActionMs = 0;
    uint16_t _wifiBulkPort = 0;
    char _wifiBulkSsid[33] = {};
    char _wifiBulkPassword[64] = {};
    uint8_t _wifiBulkToken[32] = {};
    std::vector<uint8_t> _wifiBatch;
    // Cold Wi-Fi-only ACK metadata. Allocate it explicitly in PSRAM instead
    // of permanently consuming ~4.6 KB of internal SRAM while capturing.
    WifiStagedRecord* _wifiStaged = nullptr;
    uint16_t _wifiStagedCount = 0;
};

#define PHONE_OFFLOAD PhoneOffloadManager::getInstance()
