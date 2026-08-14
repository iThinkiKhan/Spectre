#include "EntityManager.h"

#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

#include "../core/DebugLog.h"
#include "../core/Session.h"
#include "../core/SpectreState.h"
#include "StorageManager.h"

EntityManager ENTITY_MGR;

namespace {
constexpr uint64_t FNV_OFFSET = 0xCBF29CE484222325ULL;
constexpr uint64_t FNV_PRIME = 0x100000001B3ULL;

uint64_t hashByte(uint64_t value, uint8_t byte) {
    value ^= byte;
    return value * FNV_PRIME;
}

int8_t eventRssi(JsonObjectConst event) {
    return static_cast<int8_t>(constrain(event["rssi"] | -127, -127, 0));
}

uint8_t eventChannel(JsonObjectConst event) {
    return static_cast<uint8_t>(constrain(event["channel"] | 0, 0, 255));
}
}

bool EntityManager::begin() {
    if (isReady()) return true;

    _slots = static_cast<EntitySlot*>(heap_caps_calloc(
        SLOT_COUNT, sizeof(EntitySlot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    _activeSlots = static_cast<uint16_t*>(heap_caps_malloc(
        MAX_ENTITIES * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    _eventLinks = static_cast<EventLink*>(heap_caps_calloc(
        EVENT_LINK_SLOT_COUNT, sizeof(EventLink),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    _mutex = xSemaphoreCreateMutex();

    if (!_slots || !_activeSlots || !_eventLinks || !_mutex) {
        DLOG_ERROR("ENTITY", "Allocation failed psram=%uKB",
                   static_cast<unsigned>(psramBytes() / 1024U));
        if (_slots) heap_caps_free(_slots);
        if (_activeSlots) heap_caps_free(_activeSlots);
        if (_eventLinks) heap_caps_free(_eventLinks);
        if (_mutex) vSemaphoreDelete(_mutex);
        _slots = nullptr;
        _activeSlots = nullptr;
        _eventLinks = nullptr;
        _mutex = nullptr;
        return false;
    }

    startSession(SESS.getId().c_str());
    _selfTestPassed = _runBootSelfTest();
    const bool rebuilt = rebuildFromStorage();
    DLOG_INFO("ENTITY",
              "Ready entities=%u observations=%lu links=%u psram=%uKB selftest=%s rebuild=%s",
              static_cast<unsigned>(_entityCount),
              static_cast<unsigned long>(_observationCount),
              static_cast<unsigned>(MAX_EVENT_LINKS),
              static_cast<unsigned>(psramBytes() / 1024U),
              _selfTestPassed ? "pass" : "FAIL",
              rebuilt ? "ok" : "partial");
    return true;
}

bool EntityManager::_runBootSelfTest() {
    constexpr uint32_t testEventId = 0xFFFFFFFEUL;
    JsonDocument event;
    event["type"] = "device";
    event["session_id"] = SESS.getId();
    event["mac"] = "02:00:00:00:00:01";
    event["rssi"] = -42;
    event["channel"] = 6;
    event["is_random_mac"] = true;

    observeStoredEvent(testEventId, event.as<JsonObjectConst>());
    applyEnrichment(testEventId, 41.881832f, -87.623177f,
                    181.0f, 3.0f, false);
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const bool accepted = _entityCount == 1U &&
                          _observationCount == 1U &&
                          _locationUpdates == 1U &&
                          _slots[_activeSlots[0]].locationObservations == 1U;
    xSemaphoreGive(_mutex);

    observeStoredEvent(testEventId, event.as<JsonObjectConst>());
    applyEnrichment(testEventId, 41.881832f, -87.623177f,
                    181.0f, 3.0f, false);
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const bool idempotent = _entityCount == 1U &&
                            _observationCount == 1U &&
                            _locationUpdates == 1U &&
                            _slots[_activeSlots[0]].locationObservations == 1U;
    _clearLocked();
    strlcpy(_sessionId, SESS.getId().c_str(), sizeof(_sessionId));
    xSemaphoreGive(_mutex);

    DLOG_INFO("ENTITY", "Boot selftest accepted=%d idempotent=%d result=%s",
              accepted ? 1 : 0,
              idempotent ? 1 : 0,
              (accepted && idempotent) ? "pass" : "FAIL");
    return accepted && idempotent;
}

EntitySummary EntityManager::snapshot() const {
    if (!_mutex) return {};
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const EntitySummary copy = _summary;
    xSemaphoreGive(_mutex);
    return copy;
}

void EntityManager::_clearLocked() {
    memset(_slots, 0, SLOT_COUNT * sizeof(EntitySlot));
    memset(_activeSlots, 0, MAX_ENTITIES * sizeof(uint16_t));
    memset(_eventLinks, 0, EVENT_LINK_SLOT_COUNT * sizeof(EventLink));
    _entityCount = 0;
    _eventLinkCount = 0;
    _observationCount = 0;
    _locationUpdates = 0;
    _dropped = 0;
    _eventLinksDropped = 0;
    _session = {};
    _summary = {};
    _lastSummaryMs = 0;
}

void EntityManager::reset() {
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _clearLocked();
    _refreshSummaryLocked(millis());
    xSemaphoreGive(_mutex);
}

void EntityManager::startSession(const char* sessionId) {
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    strlcpy(_sessionId, sessionId ? sessionId : "", sizeof(_sessionId));
    _session = {};
    _refreshSummaryLocked(millis());
    xSemaphoreGive(_mutex);
}

size_t EntityManager::psramBytes() const {
    return SLOT_COUNT * sizeof(EntitySlot) +
           MAX_ENTITIES * sizeof(uint16_t) +
           EVENT_LINK_SLOT_COUNT * sizeof(EventLink);
}

uint64_t EntityManager::_hash(EntityKind kind, const char* stableId) {
    uint64_t value = hashByte(FNV_OFFSET, static_cast<uint8_t>(kind));
    const char* p = stableId ? stableId : "";
    while (*p) {
        char c = *p++;
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
        value = hashByte(value, static_cast<uint8_t>(c));
    }
    value = hashByte(value, 0);
    return value == 0 ? 1 : value;
}

bool EntityManager::_isRandomMacString(const char* mac) {
    if (!mac || strlen(mac) < 2) return false;
    char first[3] = {mac[0], mac[1], '\0'};
    return (strtoul(first, nullptr, 16) & 0x02U) != 0;
}

bool EntityManager::_validLocation(float lat, float lon) {
    return isfinite(lat) && isfinite(lon) &&
           lat >= -90.0f && lat <= 90.0f &&
           lon >= -180.0f && lon <= 180.0f &&
           !(lat == 0.0f && lon == 0.0f);
}

EntityManager::EntitySlot* EntityManager::_findOrInsert(
    uint64_t key, bool& inserted, uint16_t& slotIndex) {
    inserted = false;
    const uint16_t start = static_cast<uint16_t>(key & (SLOT_COUNT - 1U));
    for (uint16_t probe = 0; probe < SLOT_COUNT; ++probe) {
        slotIndex = static_cast<uint16_t>((start + probe) & (SLOT_COUNT - 1U));
        EntitySlot& slot = _slots[slotIndex];
        if (slot.key == key) return &slot;
        if (slot.key == 0) {
            if (_entityCount >= MAX_ENTITIES) return nullptr;
            slot.key = key;
            _activeSlots[_entityCount++] = slotIndex;
            inserted = true;
            return &slot;
        }
    }
    return nullptr;
}

EntityManager::EventLink* EntityManager::_findEventLink(uint32_t eventId,
                                                        bool insert) {
    if (eventId == 0) return nullptr;
    const uint32_t start = (eventId * 2654435761UL) &
                           (EVENT_LINK_SLOT_COUNT - 1U);
    for (uint32_t probe = 0; probe < EVENT_LINK_SLOT_COUNT; ++probe) {
        EventLink& link = _eventLinks[(start + probe) &
                                      (EVENT_LINK_SLOT_COUNT - 1U)];
        if (link.eventId == eventId) return &link;
        if (link.eventId == 0) {
            if (!insert) return nullptr;
            if (_eventLinkCount >= MAX_EVENT_LINKS) return nullptr;
            link.eventId = eventId;
            _eventLinkCount++;
            return &link;
        }
    }
    return nullptr;
}

uint16_t EntityManager::_observe(EntityKind kind, const char* stableId,
                                 const char* identity, const char* name,
                                 int8_t rssi, uint8_t channel, bool randomId,
                                 bool recovered) {
    if (!stableId || !stableId[0]) return UINT16_MAX;

    bool inserted = false;
    uint16_t slotIndex = UINT16_MAX;
    EntitySlot* slot = _findOrInsert(_hash(kind, stableId), inserted, slotIndex);
    if (!slot) {
        _dropped++;
        return UINT16_MAX;
    }

    const uint32_t now = millis();
    if (inserted) {
        slot->firstSeenMs = now;
        slot->kind = static_cast<uint8_t>(kind);
        slot->strongestRssi = rssi;
    }
    slot->lastSeenMs = now;
    slot->lastRssi = rssi;
    slot->channel = channel;
    if (!recovered) slot->flags |= FLAG_LIVE_SEEN;
    if (randomId) slot->flags |= FLAG_RANDOM_ID;
    if (rssi != 0 && (slot->strongestRssi == -127 || rssi > slot->strongestRssi)) {
        slot->strongestRssi = rssi;
    }
    if (slot->observations != UINT32_MAX) slot->observations++;
    if (identity && identity[0]) {
        strlcpy(slot->identity, identity, sizeof(slot->identity));
    }
    if (name && name[0]) {
        strlcpy(slot->name, name, sizeof(slot->name));
    }
    return slotIndex;
}

void EntityManager::_applyObserverLocation(uint16_t slotIndex,
                                           float lat, float lon) {
    if (slotIndex == UINT16_MAX || !_validLocation(lat, lon)) return;
    EntitySlot& slot = _slots[slotIndex];
    slot.observerLatitudeE7 = static_cast<int32_t>(lat * 10000000.0f);
    slot.observerLongitudeE7 = static_cast<int32_t>(lon * 10000000.0f);
    slot.flags |= FLAG_OBSERVER_LOCATED;
    if (slot.locationObservations != UINT32_MAX) slot.locationObservations++;
}

void EntityManager::_applyReportedLocation(uint16_t slotIndex,
                                           float lat, float lon) {
    if (slotIndex == UINT16_MAX || !_validLocation(lat, lon)) return;
    EntitySlot& slot = _slots[slotIndex];
    slot.reportedLatitudeE7 = static_cast<int32_t>(lat * 10000000.0f);
    slot.reportedLongitudeE7 = static_cast<int32_t>(lon * 10000000.0f);
    slot.flags |= FLAG_REPORTED_LOCATED;
}

void EntityManager::_syncSessionLocked(const char* sessionId) {
    const char* active = sessionId ? sessionId : "";
    if (_sessionId[0] == '\0') {
        strlcpy(_sessionId, active, sizeof(_sessionId));
    }
}

void EntityManager::observeStoredEvent(uint32_t eventId, JsonObjectConst event,
                                       bool recovered) {
    if (!isReady() || eventId == 0 || event.isNull()) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    EventLink* link = _findEventLink(eventId, false);
    if (link) {
        xSemaphoreGive(_mutex);
        return;
    }

    const char* type = event["type"] | "";
    const char* sessionId = event["session_id"] | "";
    _syncSessionLocked(sessionId);
    const bool inSession = _sessionId[0] && strcmp(_sessionId, sessionId) == 0;
    const int8_t rssi = eventRssi(event);
    const uint8_t channel = eventChannel(event);
    uint16_t primary = UINT16_MAX;
    uint16_t secondary = UINT16_MAX;

    if (strcmp(type, "network") == 0) {
        const char* bssid = event["bssid"] | "";
        primary = _observe(ENTITY_WIFI_AP, bssid, bssid,
                           event["ssid"] | "", rssi, channel, false, recovered);
        if (inSession) _session.networks++;
    } else if (strcmp(type, "probe") == 0 || strcmp(type, "device") == 0) {
        const char* mac = event["mac"] | "";
        const char* trackId = event["track_id"] | "";
        const char* stable = trackId[0] ? trackId : mac;
        primary = _observe(ENTITY_WIFI_CLIENT, stable, mac, nullptr, rssi, channel,
                           (event["is_random_mac"] | false) ||
                               _isRandomMacString(mac),
                           recovered);
        if (inSession) {
            if (strcmp(type, "probe") == 0) _session.probes++;
            else _session.devices++;
        }
    } else if (strcmp(type, "drone") == 0) {
        const char* droneId = event["drone_id"] | "";
        const char* mac = event["mac"] | "";
        const char* stable = droneId[0] ? droneId : mac;
        primary = _observe(ENTITY_DRONE, stable, mac[0] ? mac : droneId,
                           droneId, rssi, channel, false, recovered);
        _applyReportedLocation(primary,
                               event["latitude"] | 0.0f,
                               event["longitude"] | 0.0f);
        if (inSession) _session.drones++;
    } else if (strcmp(type, "subghz") == 0) {
        const uint32_t source = event["source_addr"] | 0U;
        if (source != 0) {
            char stable[32] = {};
            char identity[24] = {};
            snprintf(stable, sizeof(stable), "SRC:%lu",
                     static_cast<unsigned long>(source));
            snprintf(identity, sizeof(identity), "RF:%lu",
                     static_cast<unsigned long>(source));
            const uint32_t hz = event["frequency_hz"] | 0U;
            const uint32_t mhz = hz / 1000000UL;
            const uint8_t band = static_cast<uint8_t>(mhz > 255U ? 255U : mhz);
            primary = _observe(ENTITY_SUBGHZ, stable, identity, nullptr,
                               rssi, band, false, recovered);
        }
    } else if (strcmp(type, "pmkid") == 0 ||
               (strcmp(type, "event") == 0 &&
                strcmp(event["event_type"] | "", "handshake") == 0)) {
        const char* ap = event["bssid"] | event["ap"] | "";
        const char* client = event["client_mac"] | event["client"] |
                             event["sta"] | "";
        primary = _observe(ENTITY_WIFI_AP, ap, ap, event["ssid"] | "",
                           rssi, channel, false, recovered);
        secondary = _observe(ENTITY_WIFI_CLIENT, client, client, nullptr,
                             rssi, channel, _isRandomMacString(client), recovered);
        if (inSession && strcmp(type, "pmkid") == 0) _session.pmkids++;
    } else if (strcmp(type, "ble") == 0 || strcmp(type, "ble_device") == 0) {
        const char* address = event["address"] | event["mac"] | "";
        primary = _observe(ENTITY_BLE, address, address, event["name"] | "",
                           rssi, channel, false, recovered);
    }

    if (primary != UINT16_MAX || secondary != UINT16_MAX) {
        if (_observationCount != UINT32_MAX) _observationCount++;
        link = _findEventLink(eventId, true);
        if (link) {
            link->primarySlot = primary;
            link->secondarySlot = secondary;
        } else {
            _eventLinksDropped++;
        }
    }

    xSemaphoreGive(_mutex);
}

void EntityManager::applyEnrichment(uint32_t eventId, float lat, float lon,
                                    float, float, bool noData) {
    if (!isReady() || eventId == 0) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    EventLink* link = _findEventLink(eventId, false);
    if (!link || (link->flags & LINK_ENRICH_APPLIED)) {
        xSemaphoreGive(_mutex);
        return;
    }

    link->flags |= LINK_ENRICH_APPLIED;
    if (!noData && _validLocation(lat, lon)) {
        _applyObserverLocation(link->primarySlot, lat, lon);
        _applyObserverLocation(link->secondarySlot, lat, lon);
        if (_locationUpdates != UINT32_MAX) _locationUpdates++;
    }
    xSemaphoreGive(_mutex);
}

bool EntityManager::rebuildFromStorage() {
    if (!isReady() || !STORAGE.isReady()) return false;

    xSemaphoreTake(_mutex, portMAX_DELAY);
    _clearLocked();
    strlcpy(_sessionId, SESS.getId().c_str(), sizeof(_sessionId));
    xSemaphoreGive(_mutex);

    const uint32_t started = millis();
    const bool ok = STORAGE.forEachStoredRecord(
        [&](const DecodedSpoolRecord& rec) -> bool {
            if (rec.recordType == SPOOL_REC_EVENT) {
                observeStoredEvent(rec.eventId,
                                   rec.doc.as<JsonObjectConst>(), true);
            } else if (rec.recordType == SPOOL_REC_ENRICH_DELTA) {
                applyEnrichment(rec.doc["event_id"] | 0U,
                                rec.doc["lat"] | 0.0f,
                                rec.doc["lon"] | 0.0f,
                                rec.doc["alt"] | 0.0f,
                                rec.doc["acc"] | 0.0f,
                                rec.doc["enrich_no_data"] | false);
            }
            return true;
        });

    xSemaphoreTake(_mutex, portMAX_DELAY);
    _refreshSummaryLocked(millis());
    const uint16_t entities = _entityCount;
    const uint32_t observations = _observationCount;
    xSemaphoreGive(_mutex);
    DLOG_INFO("ENTITY", "Rebuild %s entities=%u observations=%lu ms=%lu",
              ok ? "complete" : "partial",
              static_cast<unsigned>(entities),
              static_cast<unsigned long>(observations),
              static_cast<unsigned long>(millis() - started));
    return ok;
}

void EntityManager::_refreshSummaryLocked(uint32_t nowMs) {
    EntitySummary next{};
    next.total = _entityCount;
    next.observations = _observationCount;
    next.locationUpdates = _locationUpdates;
    next.dropped = _dropped;
    next.eventLinksDropped = _eventLinksDropped;
    next.selfTestPassed = _selfTestPassed;
    next.sessionNetworks = _session.networks;
    next.sessionDevices = _session.devices;
    next.sessionProbes = _session.probes;
    next.sessionPmkids = _session.pmkids;
    next.sessionDrones = _session.drones;

    uint32_t bestAge = UINT32_MAX;
    for (uint16_t i = 0; i < _entityCount; ++i) {
        const EntitySlot& slot = _slots[_activeSlots[i]];
        if (slot.kind < ENTITY_KIND_COUNT && next.byKind[slot.kind] != UINT16_MAX) {
            next.byKind[slot.kind]++;
        }
        const bool located = (slot.flags &
            (FLAG_OBSERVER_LOCATED | FLAG_REPORTED_LOCATED)) != 0;
        if (located) next.located++;
        if (!(slot.flags & FLAG_LIVE_SEEN)) continue;

        const uint32_t age = nowMs - slot.lastSeenMs;
        if (age > NEARBY_WINDOW_MS) continue;
        next.nearby++;
        if (slot.lastRssi == 0 || slot.lastRssi == -127) continue;
        if (next.closestRssi == -127 || slot.lastRssi > next.closestRssi ||
            (slot.lastRssi == next.closestRssi && age < bestAge)) {
            next.closestRssi = slot.lastRssi;
            next.closestChannel = slot.channel;
            next.closestKind = static_cast<EntityKind>(slot.kind);
            next.closestLocated = located;
            strlcpy(next.closestIdentity, slot.identity,
                    sizeof(next.closestIdentity));
            strlcpy(next.closestName, slot.name, sizeof(next.closestName));
            bestAge = age;
        }
    }
    _summary = next;

    STATE_WRITE_BEGIN();
    g_state.entityTotal = next.total;
    g_state.entityNearby = next.nearby;
    g_state.entityObservations = next.observations;
    g_state.entityLocated = next.located;
    g_state.entityLocationUpdates = next.locationUpdates;
    g_state.entityDropped = next.dropped + next.eventLinksDropped;
    g_state.entityAccessPoints = next.byKind[ENTITY_WIFI_AP];
    g_state.entityClients = next.byKind[ENTITY_WIFI_CLIENT];
    g_state.entityDrones = next.byKind[ENTITY_DRONE];
    g_state.entitySubGhz = next.byKind[ENTITY_SUBGHZ];
    g_state.entityBle = next.byKind[ENTITY_BLE];
    g_state.sessionNetworks = static_cast<int>(next.sessionNetworks);
    g_state.sessionDevices = static_cast<int>(next.sessionDevices);
    g_state.sessionProbes = static_cast<int>(next.sessionProbes);
    g_state.sessionPMKIDs = static_cast<int>(next.sessionPmkids);
    g_state.sessionDrones = static_cast<int>(next.sessionDrones);
    g_state.entityClosestRssi = next.closestRssi;
    g_state.entityClosestChannel = next.closestChannel;
    g_state.entityClosestKind = static_cast<uint8_t>(next.closestKind);
    g_state.entityClosestLocated = next.closestLocated;
    strlcpy(g_state.entityClosestIdentity, next.closestIdentity,
            sizeof(g_state.entityClosestIdentity));
    strlcpy(g_state.entityClosestName, next.closestName,
            sizeof(g_state.entityClosestName));
    STATE_WRITE_END();
}

void EntityManager::tick(uint32_t nowMs) {
    if (!isReady() || nowMs - _lastSummaryMs < 500UL) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _lastSummaryMs = nowMs;
    _refreshSummaryLocked(nowMs);
    xSemaphoreGive(_mutex);
}
