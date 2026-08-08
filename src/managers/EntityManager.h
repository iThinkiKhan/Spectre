#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/semphr.h>

enum EntityKind : uint8_t {
    ENTITY_WIFI_AP = 0,
    ENTITY_WIFI_CLIENT,
    ENTITY_DRONE,
    ENTITY_SUBGHZ,
    ENTITY_BLE,
    ENTITY_KIND_COUNT
};

struct EntitySummary {
    uint32_t total = 0;
    uint32_t nearby = 0;
    uint32_t observations = 0;
    uint32_t located = 0;
    uint32_t locationUpdates = 0;
    uint32_t dropped = 0;
    uint32_t eventLinksDropped = 0;
    bool selfTestPassed = false;
    uint16_t byKind[ENTITY_KIND_COUNT] = {};

    uint32_t sessionNetworks = 0;
    uint32_t sessionDevices = 0;
    uint32_t sessionProbes = 0;
    uint32_t sessionPmkids = 0;
    uint32_t sessionDrones = 0;

    int8_t closestRssi = -127;
    uint8_t closestChannel = 0;
    EntityKind closestKind = ENTITY_WIFI_CLIENT;
    bool closestLocated = false;
    char closestIdentity[24] = {};
    char closestName[24] = {};
};

class EntityManager {
public:
    static constexpr uint16_t MAX_ENTITIES = 8192;
    static constexpr uint32_t MAX_EVENT_LINKS = 98304;

    bool begin();
    bool rebuildFromStorage();
    void reset();
    void startSession(const char* sessionId);
    void tick(uint32_t nowMs = millis());
    bool isReady() const {
        return _slots && _activeSlots && _eventLinks && _mutex;
    }
    EntitySummary snapshot() const;
    size_t psramBytes() const;

    // This is the canonical ingestion boundary. Call only after an event has
    // been durably accepted by StorageManager.
    void observeStoredEvent(uint32_t eventId, JsonObjectConst event,
                            bool recovered = false);
    void applyEnrichment(uint32_t eventId, float lat, float lon,
                         float alt, float accuracy, bool noData = false);

private:
    struct EntitySlot {
        uint64_t key = 0;
        uint32_t firstSeenMs = 0;
        uint32_t lastSeenMs = 0;
        uint32_t observations = 0;
        uint32_t locationObservations = 0;
        int32_t observerLatitudeE7 = 0;
        int32_t observerLongitudeE7 = 0;
        int32_t reportedLatitudeE7 = 0;
        int32_t reportedLongitudeE7 = 0;
        int8_t lastRssi = -127;
        int8_t strongestRssi = -127;
        uint8_t channel = 0;
        uint8_t kind = ENTITY_WIFI_CLIENT;
        uint8_t flags = 0;
        char identity[24] = {};
        char name[24] = {};
    };

    struct EventLink {
        uint32_t eventId = 0;
        uint16_t primarySlot = UINT16_MAX;
        uint16_t secondarySlot = UINT16_MAX;
        uint8_t flags = 0;
        uint8_t reserved = 0;
    };

    struct SessionCounters {
        uint32_t networks = 0;
        uint32_t devices = 0;
        uint32_t probes = 0;
        uint32_t pmkids = 0;
        uint32_t drones = 0;
    };

    static constexpr uint16_t SLOT_COUNT = 16384;
    static constexpr uint32_t EVENT_LINK_SLOT_COUNT = 131072;
    static constexpr uint32_t NEARBY_WINDOW_MS = 30000UL;
    static constexpr uint8_t FLAG_RANDOM_ID = 0x01;
    static constexpr uint8_t FLAG_OBSERVER_LOCATED = 0x02;
    static constexpr uint8_t FLAG_REPORTED_LOCATED = 0x04;
    static constexpr uint8_t FLAG_LIVE_SEEN = 0x08;
    static constexpr uint8_t LINK_ENRICH_APPLIED = 0x01;

    EntitySlot* _slots = nullptr;
    uint16_t* _activeSlots = nullptr;
    EventLink* _eventLinks = nullptr;
    SemaphoreHandle_t _mutex = nullptr;
    uint16_t _entityCount = 0;
    uint32_t _eventLinkCount = 0;
    uint32_t _observationCount = 0;
    uint32_t _locationUpdates = 0;
    uint32_t _dropped = 0;
    uint32_t _eventLinksDropped = 0;
    uint32_t _lastSummaryMs = 0;
    bool _selfTestPassed = false;
    char _sessionId[40] = {};
    SessionCounters _session{};
    EntitySummary _summary{};

    uint16_t _observe(EntityKind kind, const char* stableId,
                      const char* identity, const char* name,
                      int8_t rssi, uint8_t channel, bool randomId,
                      bool recovered);
    EntitySlot* _findOrInsert(uint64_t key, bool& inserted,
                              uint16_t& slotIndex);
    EventLink* _findEventLink(uint32_t eventId, bool insert);
    void _applyObserverLocation(uint16_t slotIndex, float lat, float lon);
    void _applyReportedLocation(uint16_t slotIndex, float lat, float lon);
    void _clearLocked();
    void _refreshSummaryLocked(uint32_t nowMs);
    void _syncSessionLocked(const char* sessionId);
    bool _runBootSelfTest();
    static uint64_t _hash(EntityKind kind, const char* stableId);
    static bool _validLocation(float lat, float lon);
    static bool _isRandomMacString(const char* mac);
};

extern EntityManager ENTITY_MGR;
