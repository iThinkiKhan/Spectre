#include "RAMSpool.h"

#include "../config.h"
#include "../core/DebugLog.h"
#include "StorageManager.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>

namespace RAMSpool {

static constexpr uint32_t WORKER_SLOW_MS = 250U;
static constexpr uint32_t PRESSURE_WATCH_FREE_SLOTS = POOL_SIZE / 8U;
static constexpr uint32_t PRESSURE_FULL_FREE_SLOTS = POOL_SIZE / 32U;

static EventSlot*   s_pool          = nullptr;
static QueueHandle_t s_freeQ        = nullptr;
static QueueHandle_t s_laneQ[4]     = {nullptr, nullptr, nullptr, nullptr};
static SemaphoreHandle_t s_enqueueMux = nullptr;
static TaskHandle_t s_workerTask    = nullptr;

static volatile uint32_t s_seq                = 0;
static volatile uint32_t s_enqueued           = 0;
static volatile uint32_t s_droppedPoolFull    = 0;
static volatile uint32_t s_droppedTooLarge    = 0;
static volatile uint32_t s_droppedNoiseSquelch = 0;
static volatile uint32_t s_consumed           = 0;
static volatile uint32_t s_inflightHigh       = 0;

static volatile uint32_t s_producerProbeOk    = 0;
static volatile uint32_t s_producerProbeDrop  = 0;
static volatile uint32_t s_producerDeviceOk   = 0;
static volatile uint32_t s_producerDeviceDrop = 0;
static volatile uint32_t s_workerProbeOk      = 0;
static volatile uint32_t s_workerProbeSuppressed = 0;
static volatile uint32_t s_workerProbeDropped = 0;
static volatile uint32_t s_workerProbeFail    = 0;
static volatile uint32_t s_workerProbeSlow    = 0;
static volatile uint32_t s_workerDeviceOk     = 0;
static volatile uint32_t s_workerDeviceSuppressed = 0;
static volatile uint32_t s_workerDeviceDropped = 0;
static volatile uint32_t s_workerDeviceFail   = 0;
static volatile uint32_t s_workerDeviceSlow   = 0;
static volatile uint32_t s_workerSlow         = 0;
static volatile uint32_t s_workerSlowMax      = 0;
static volatile uint8_t  s_pressureState       = 0;
static volatile uint32_t s_p3EvictedOldest     = 0;
static volatile uint32_t s_p3PressureDrop      = 0;
static volatile uint32_t s_p2PressureDrop      = 0;
static volatile uint32_t s_p1EnqueueFail       = 0;
static volatile uint32_t s_priorityEnqueued[4] = {0, 0, 0, 0};
static volatile uint16_t s_lanePeakDepth[4]    = {0, 0, 0, 0};
static uint32_t s_workerMetaAcceptedSinceFlush = 0;
static uint32_t s_workerMetaLastFlushMs        = 0;
static uint32_t s_workerMetaLastIdleProbeMs    = 0;

static constexpr uint32_t WORKER_IDLE_FLUSH_RETRY_MS = 1000U;

static const char* const kScoreIgnoredKeys[] = {
    "id",
    "ts",
    F_TIMESTAMP_ISO,
    "type",
    "status",
    F_ENRICH_STATE,
    "prio",
    "lane",
    "lane_name",
    F_SESSION,
    "session_id",
    "uploaded_ts",
    "enriched_ts"
};

static const char* const kScoreUsefulKeys[] = {
    "mac",
    "bssid",
    "ap",
    "sta",
    "client",
    "ssid",
    "rssi",
    "channel",
    "pmkid_hex",
    "hashcat_line",
    "drone_id",
    "protocol",
    "frequency_hz",
    "payload_hex",
    "event_type",
    "detail",
    "tag"
};

static inline uint8_t _laneIndexForPriority(EventPrio prio) {
    switch (prio) {
        case PRIO_P1: return 1U;
        case PRIO_P2: return 2U;
        case PRIO_P3: return 3U;
        case PRIO_P0:
        default:      return 1U;
    }
}

static inline uint8_t _pressureStateForFreeSlots(uint16_t freeSlots) {
    if (freeSlots <= PRESSURE_FULL_FREE_SLOTS) {
        return 3U;
    }
    if (freeSlots <= PRESSURE_WATCH_FREE_SLOTS) {
        return 2U;
    }
    return 0U;
}

static inline const char* _slotKindText(SlotKind kind) {
    switch (kind) {
        case SLOT_PROBE:     return "probe";
        case SLOT_DEVICE:    return "device";
        case SLOT_NOISE_P3:  return "noise_p3";
        case SLOT_SHADOW:
        default:             return "shadow";
    }
}

static inline bool _isMigratedKind(SlotKind kind) {
    return kind == SLOT_PROBE ||
           kind == SLOT_DEVICE ||
           kind == SLOT_NOISE_P3;
}

static uint8_t _captureValueScore(JsonObjectConst payload) {
    auto matchesKey = [](const char* key,
                         const char* const* keys,
                         size_t keyCount) -> bool {
        for (size_t i = 0; i < keyCount; ++i) {
            if (strcmp(key, keys[i]) == 0) {
                return true;
            }
        }
        return false;
    };

    auto hasUsefulValue = [](JsonVariantConst value) -> bool {
        if (value.isNull()) {
            return false;
        }
        if (value.is<const char*>()) {
            const char* text = value.as<const char*>();
            return text && text[0];
        }
        return true;
    };

    uint8_t score = 0;
    for (JsonPairConst kv : payload) {
        const char* key = kv.key().c_str();
        if (!key || !key[0]) {
            continue;
        }

        if (matchesKey(key, kScoreIgnoredKeys,
                       sizeof(kScoreIgnoredKeys) / sizeof(kScoreIgnoredKeys[0]))) {
            continue;
        }

        if (!matchesKey(key, kScoreUsefulKeys,
                        sizeof(kScoreUsefulKeys) / sizeof(kScoreUsefulKeys[0]))) {
            continue;
        }

        if (!hasUsefulValue(kv.value())) {
            continue;
        }

        ++score;
    }

    return score;
}

CaptureClassification classify(const char* type, const char* eventType) {
    const char* t = type ? type : "event";
    const char* et = eventType ? eventType : "";

    if (strcmp(t, "pmkid") == 0) {
        return {PRIO_P1, LANE_MISSION, true};
    }
    if (strcmp(t, "drone") == 0) {
        return {PRIO_P1, LANE_MISSION, true};
    }
    if (strcmp(t, "subghz") == 0) {
        return {PRIO_P1, LANE_NOISE, true};
    }
    if (strcmp(t, "probe") == 0) {
        return {PRIO_P2, LANE_NOISE, true};
    }
    if (strcmp(t, "device") == 0) {
        return {PRIO_P2, LANE_NOISE, true};
    }
    if (strcmp(t, "event") == 0 && strcmp(et, "handshake") == 0) {
        return {PRIO_P1, LANE_MISSION, true};
    }

    return {PRIO_P3, LANE_NOISE, false};
}

CaptureClassification classify(const char* type, JsonObjectConst payload) {
    CaptureClassification cls = classify(type, payload["event_type"] | "");
    cls.valueScore = _captureValueScore(payload);

    if (cls.priority > PRIO_P1 && cls.valueScore < 2U) {
        cls.priority = PRIO_P3;
        cls.lane = LANE_NOISE;
        cls.enrichEligible = false;
    } else if (cls.priority != PRIO_P3) {
        cls.enrichEligible = true;
    }

    if (cls.priority == PRIO_P3) {
        cls.enrichEligible = false;
    }

    return cls;
}

static inline bool _laneHasPending(QueueHandle_t q) {
    return q && uxQueueMessagesWaiting(q) > 0;
}

static bool _popNextPendingSlot(uint16_t& idx) {
    static constexpr uint8_t kServiceOrder[] = {1U, 2U, 3U, 0U};
    for (uint8_t lane : kServiceOrder) {
        if (_laneHasPending(s_laneQ[lane]) &&
            xQueueReceive(s_laneQ[lane], &idx, 0) == pdTRUE) {
            return true;
        }
    }
    return false;
}

static void _noteWorkerSlow(uint32_t ms) {
    if (ms < WORKER_SLOW_MS) {
        return;
    }

    __atomic_add_fetch(&s_workerSlow, 1, __ATOMIC_RELAXED);
    uint32_t prevMax = __atomic_load_n(&s_workerSlowMax, __ATOMIC_RELAXED);
    if (ms > prevMax) {
        __atomic_store_n(&s_workerSlowMax, ms, __ATOMIC_RELAXED);
    }
}

static void _updatePressureState() {
    const uint16_t freeSlots = s_freeQ
        ? static_cast<uint16_t>(uxQueueMessagesWaiting(s_freeQ))
        : 0U;
    __atomic_store_n(&s_pressureState,
                     _pressureStateForFreeSlots(freeSlots),
                     __ATOMIC_RELAXED);
}

static void _noteWorkerProbeSlow(uint32_t ms) {
    __atomic_add_fetch(&s_workerProbeSlow, 1, __ATOMIC_RELAXED);
    _noteWorkerSlow(ms);
}

static void _noteWorkerDeviceSlow(uint32_t ms) {
    __atomic_add_fetch(&s_workerDeviceSlow, 1, __ATOMIC_RELAXED);
    _noteWorkerSlow(ms);
}

void noteWorkerProbeOk(uint32_t ms) {
    __atomic_add_fetch(&s_workerProbeOk, 1, __ATOMIC_RELAXED);
    _noteWorkerProbeSlow(ms);
}

void noteWorkerProbeSuppressed(uint32_t ms) {
    __atomic_add_fetch(&s_workerProbeSuppressed, 1, __ATOMIC_RELAXED);
    _noteWorkerProbeSlow(ms);
}

void noteWorkerProbeDropped(uint32_t ms) {
    __atomic_add_fetch(&s_workerProbeDropped, 1, __ATOMIC_RELAXED);
    _noteWorkerProbeSlow(ms);
}

void noteWorkerProbeFail(uint32_t ms, uint32_t status) {
    __atomic_add_fetch(&s_workerProbeFail, 1, __ATOMIC_RELAXED);
    _noteWorkerProbeSlow(ms);
    DLOG_WARN("STORAGE",
              "worker probe write fail status=%u ms=%lu",
              static_cast<unsigned>(status),
              static_cast<unsigned long>(ms));
}

void noteWorkerDeviceOk(uint32_t ms) {
    __atomic_add_fetch(&s_workerDeviceOk, 1, __ATOMIC_RELAXED);
    _noteWorkerDeviceSlow(ms);
}

void noteWorkerDeviceSuppressed(uint32_t ms) {
    __atomic_add_fetch(&s_workerDeviceSuppressed, 1, __ATOMIC_RELAXED);
    _noteWorkerDeviceSlow(ms);
}

void noteWorkerDeviceDropped(uint32_t ms) {
    __atomic_add_fetch(&s_workerDeviceDropped, 1, __ATOMIC_RELAXED);
    _noteWorkerDeviceSlow(ms);
}

void noteWorkerDeviceFail(uint32_t ms, uint32_t status) {
    __atomic_add_fetch(&s_workerDeviceFail, 1, __ATOMIC_RELAXED);
    _noteWorkerDeviceSlow(ms);
    DLOG_WARN("STORAGE",
              "worker device write fail status=%u ms=%lu",
              static_cast<unsigned>(status),
              static_cast<unsigned long>(ms));
}

static void _workerTask(void*) {
    DLOG_INFO("STORAGE", "RAMSpool worker started on core %d",
              (int)xPortGetCoreID());
    for (;;) {
        uint16_t idx = 0;
        if (!_popNextPendingSlot(idx)) {
            if (STORAGE.hasWorkerUiRefreshWork()) {
                STORAGE.refreshStorageUiState(false, false);
            }
            if (STORAGE.hasWorkerMetadataFlushWork()) {
                s_workerMetaLastIdleProbeMs = millis();
                if (STORAGE.flushWorkerMetadataBatch("ramspool_idle", true)) {
                    s_workerMetaAcceptedSinceFlush = 0;
                    s_workerMetaLastFlushMs = millis();
                }
            }
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        const EventSlot& slot = s_pool[idx];
        const SlotKind kind = static_cast<SlotKind>(slot.slotKind);
        if (_isMigratedKind(kind)) {
            const uint32_t startMs = millis();
            const AppendEventResult r = STORAGE.appendQueuedRecord(slot);
            const uint32_t elapsed = millis() - startMs;

            switch (kind) {
                case SLOT_PROBE:
                    if (r.ok()) {
                        noteWorkerProbeOk(elapsed);
                    } else if (r.suppressed()) {
                        noteWorkerProbeSuppressed(elapsed);
                    } else if (r.dropped()) {
                        noteWorkerProbeDropped(elapsed);
                        DLOG_WARN("STORAGE",
                                  "worker probe dropped status=%u ms=%lu",
                                  static_cast<unsigned>(r.status),
                                  static_cast<unsigned long>(elapsed));
                    } else {
                        noteWorkerProbeFail(elapsed, static_cast<uint32_t>(r.status));
                    }
                    break;

                case SLOT_DEVICE:
                case SLOT_NOISE_P3:
                    if (r.ok()) {
                        noteWorkerDeviceOk(elapsed);
                    } else if (r.suppressed()) {
                        noteWorkerDeviceSuppressed(elapsed);
                    } else if (r.dropped()) {
                        noteWorkerDeviceDropped(elapsed);
                        DLOG_WARN("STORAGE",
                                  "worker %s dropped status=%u ms=%lu",
                                  _slotKindText(kind),
                                  static_cast<unsigned>(r.status),
                                  static_cast<unsigned long>(elapsed));
                    } else {
                        noteWorkerDeviceFail(elapsed, static_cast<uint32_t>(r.status));
                    }
                    break;

                case SLOT_SHADOW:
                default:
                    break;
            }

            if (r.ok() && elapsed >= WORKER_SLOW_MS) {
                DLOG_WARN("STORAGE",
                          "queued write slow type=%s ms=%lu ok=1",
                          _slotKindText(kind),
                          static_cast<unsigned long>(elapsed));
            }
            if (r.ok()) {
                const uint32_t now = millis();
                s_workerMetaAcceptedSinceFlush++;
                if (s_workerMetaAcceptedSinceFlush == 1 && s_workerMetaLastFlushMs == 0) {
                    s_workerMetaLastFlushMs = now;
                }
            }

            const uint32_t now = millis();
            const bool dueByCount =
                s_workerMetaAcceptedSinceFlush >= STORAGE_EVENT_COUNTER_SAVE_EVERY_N;
            const bool dueByTime =
                s_workerMetaLastFlushMs != 0 &&
                (now - s_workerMetaLastFlushMs) >= STORAGE_HOT_META_SAVE_INTERVAL_MS;
            if (dueByCount || dueByTime) {
                if (STORAGE.flushWorkerMetadataBatch("ramspool_batch", false) ||
                    !STORAGE.hasWorkerMetadataFlushWork()) {
                    s_workerMetaAcceptedSinceFlush = 0;
                    s_workerMetaLastFlushMs = millis();
                }
            }
        }

        __atomic_add_fetch(&s_consumed, 1, __ATOMIC_RELAXED);
        xQueueSend(s_freeQ, &idx, 0);
        _updatePressureState();
        taskYIELD();
    }
}

bool begin() {
    if (isReady()) return true;
    if (s_pool && !isReady()) {
        return false;
    }

    const size_t totalBytes = POOL_SIZE * sizeof(EventSlot);
    s_pool = static_cast<EventSlot*>(
        heap_caps_calloc(POOL_SIZE, sizeof(EventSlot), MALLOC_CAP_SPIRAM));
    if (!s_pool) {
        DLOG_ERROR("STORAGE",
                   "RAMSpool PSRAM alloc failed need=%uKB",
                   (unsigned)(totalBytes / 1024U));
        return false;
    }

    s_freeQ     = xQueueCreate(POOL_SIZE, sizeof(uint16_t));
    s_laneQ[0]  = xQueueCreate(POOL_SIZE, sizeof(uint16_t));
    s_laneQ[1]  = xQueueCreate(POOL_SIZE, sizeof(uint16_t));
    s_laneQ[2]  = xQueueCreate(POOL_SIZE, sizeof(uint16_t));
    s_laneQ[3]  = xQueueCreate(POOL_SIZE, sizeof(uint16_t));
    s_enqueueMux = xSemaphoreCreateMutex();
    if (!s_freeQ || !s_laneQ[0] || !s_laneQ[1] || !s_laneQ[2] || !s_laneQ[3] ||
        !s_enqueueMux) {
        DLOG_ERROR("STORAGE", "RAMSpool queue create failed");
        if (s_freeQ) {
            vQueueDelete(s_freeQ);
            s_freeQ = nullptr;
        }
        for (QueueHandle_t& q : s_laneQ) {
            if (q) {
                vQueueDelete(q);
                q = nullptr;
            }
        }
        if (s_enqueueMux) {
            vSemaphoreDelete(s_enqueueMux);
            s_enqueueMux = nullptr;
        }
        if (s_pool) {
            heap_caps_free(s_pool);
            s_pool = nullptr;
        }
        return false;
    }

    for (uint16_t i = 0; i < POOL_SIZE; i++) {
        xQueueSend(s_freeQ, &i, 0);
    }

    s_workerMetaAcceptedSinceFlush = 0;
    s_workerMetaLastFlushMs = 0;
    for (uint8_t i = 0; i < 4; ++i) {
        __atomic_store_n(&s_priorityEnqueued[i], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&s_lanePeakDepth[i], 0, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&s_pressureState, 0, __ATOMIC_RELAXED);

    BaseType_t ok = xTaskCreatePinnedToCore(
        _workerTask, "TaskStorage", 4096, nullptr, 2, &s_workerTask, 1);
    if (ok != pdPASS) {
        DLOG_ERROR("STORAGE", "RAMSpool worker spawn failed");
        vQueueDelete(s_freeQ);
        s_freeQ = nullptr;
        for (QueueHandle_t& q : s_laneQ) {
            if (q) {
                vQueueDelete(q);
                q = nullptr;
            }
        }
        vSemaphoreDelete(s_enqueueMux);
        s_enqueueMux = nullptr;
        heap_caps_free(s_pool);
        s_pool = nullptr;
        return false;
    }

    DLOG_INFO("STORAGE",
              "RAMSpool ready slots=%u slotBytes=%u total=%uKB core=1",
              (unsigned)POOL_SIZE,
              (unsigned)sizeof(EventSlot),
              (unsigned)(totalBytes / 1024U));
    return true;
}

bool isReady() {
    return s_pool != nullptr &&
           s_freeQ != nullptr &&
           s_laneQ[0] != nullptr &&
           s_laneQ[1] != nullptr &&
           s_laneQ[2] != nullptr &&
           s_laneQ[3] != nullptr &&
           s_enqueueMux != nullptr;
}

bool enqueue(const char* type,
             JsonObjectConst payload,
             SlotKind slotKind,
             const CaptureClassification& classification) {
    if (!type || !type[0]) return false;

    if (!isReady()) {
        switch (slotKind) {
            case SLOT_PROBE:
                __atomic_add_fetch(&s_producerProbeDrop, 1, __ATOMIC_RELAXED);
                break;
            case SLOT_DEVICE:
            case SLOT_NOISE_P3:
                __atomic_add_fetch(&s_producerDeviceDrop, 1, __ATOMIC_RELAXED);
                break;
            case SLOT_SHADOW:
            default:
                break;
        }
        return false;
    }

    const size_t needed = measureJson(payload);
    if (needed >= MAX_PAYLOAD) {
        __atomic_add_fetch(&s_droppedTooLarge, 1, __ATOMIC_RELAXED);
        switch (slotKind) {
            case SLOT_PROBE:
                __atomic_add_fetch(&s_producerProbeDrop, 1, __ATOMIC_RELAXED);
                break;
            case SLOT_DEVICE:
            case SLOT_NOISE_P3:
                __atomic_add_fetch(&s_producerDeviceDrop, 1, __ATOMIC_RELAXED);
                break;
            case SLOT_SHADOW:
            default:
                break;
        }
        return false;
    }

    const uint8_t laneIndex = _laneIndexForPriority(classification.priority);
    const uint8_t p3Index = _laneIndexForPriority(PRIO_P3);
    uint16_t idx = 0;
    bool ok = false;
    auto noteSpecificDrop = [&]() {
        __atomic_add_fetch(&s_droppedPoolFull, 1, __ATOMIC_RELAXED);
        switch (slotKind) {
            case SLOT_PROBE:
                __atomic_add_fetch(&s_producerProbeDrop, 1, __ATOMIC_RELAXED);
                break;
            case SLOT_DEVICE:
            case SLOT_NOISE_P3:
                __atomic_add_fetch(&s_producerDeviceDrop, 1, __ATOMIC_RELAXED);
                break;
            case SLOT_SHADOW:
            default:
                break;
        }
    };

    xSemaphoreTake(s_enqueueMux, portMAX_DELAY);
    do {
        const uint16_t freeBefore = s_freeQ
            ? static_cast<uint16_t>(uxQueueMessagesWaiting(s_freeQ))
            : 0U;
        _updatePressureState();

        if (xQueueReceive(s_freeQ, &idx, 0) != pdTRUE) {
            uint16_t evictedIdx = 0;
            if (xQueueReceive(s_laneQ[p3Index], &evictedIdx, 0) != pdTRUE) {
                switch (classification.priority) {
                    case PRIO_P1:
                    case PRIO_P0:
                        __atomic_add_fetch(&s_p1EnqueueFail, 1, __ATOMIC_RELAXED);
                        noteSpecificDrop();
                        DLOG_WARN("STORAGE",
                                  "RAMSpool P1 enqueue fail type=%s free=%u pressure=%u",
                                  type,
                                  static_cast<unsigned>(freeBefore),
                                  static_cast<unsigned>(
                                      __atomic_load_n(&s_pressureState, __ATOMIC_RELAXED)));
                        break;
                    case PRIO_P2:
                        __atomic_add_fetch(&s_p2PressureDrop, 1, __ATOMIC_RELAXED);
                        noteSpecificDrop();
                        break;
                    case PRIO_P3:
                    default:
                        __atomic_add_fetch(&s_p3PressureDrop, 1, __ATOMIC_RELAXED);
                        noteSpecificDrop();
                        break;
                }
                break;
            }

            __atomic_add_fetch(&s_p3EvictedOldest, 1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&s_droppedPoolFull, 1, __ATOMIC_RELAXED);
            xQueueSend(s_freeQ, &evictedIdx, 0);
            if (xQueueReceive(s_freeQ, &idx, 0) != pdTRUE) {
                switch (classification.priority) {
                    case PRIO_P1:
                    case PRIO_P0:
                        __atomic_add_fetch(&s_p1EnqueueFail, 1, __ATOMIC_RELAXED);
                        break;
                    case PRIO_P2:
                        __atomic_add_fetch(&s_p2PressureDrop, 1, __ATOMIC_RELAXED);
                        break;
                    case PRIO_P3:
                    default:
                        __atomic_add_fetch(&s_p3PressureDrop, 1, __ATOMIC_RELAXED);
                        break;
                }
                noteSpecificDrop();
                break;
            }
        }

        EventSlot& slot = s_pool[idx];
        slot.enqueueSeq   = __atomic_add_fetch(&s_seq, 1, __ATOMIC_RELAXED);
        slot.tsMs         = millis();
        slot.provPriority = static_cast<uint8_t>(classification.priority);
        slot.provLane     = static_cast<uint8_t>(classification.lane);
        slot.enrichEligible = static_cast<uint8_t>(classification.enrichEligible);
        slot.valueScore   = classification.valueScore;
        slot.slotKind     = static_cast<uint8_t>(slotKind);
        strlcpy(slot.type, type, sizeof(slot.type));

        const size_t written = serializeJson(payload, slot.payload, MAX_PAYLOAD);
        slot.payloadLen = static_cast<uint16_t>(written);

        if (xQueueSend(s_laneQ[laneIndex], &idx, 0) != pdTRUE) {
            xQueueSend(s_freeQ, &idx, 0);
            noteSpecificDrop();
            break;
        }

        __atomic_add_fetch(&s_enqueued, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&s_priorityEnqueued[static_cast<uint8_t>(classification.priority)],
                           1,
                           __ATOMIC_RELAXED);
        switch (slotKind) {
            case SLOT_PROBE:
                __atomic_add_fetch(&s_producerProbeOk, 1, __ATOMIC_RELAXED);
                break;
            case SLOT_DEVICE:
            case SLOT_NOISE_P3:
                __atomic_add_fetch(&s_producerDeviceOk, 1, __ATOMIC_RELAXED);
                break;
            case SLOT_SHADOW:
            default:
                break;
        }

        const uint16_t freeSlots = static_cast<uint16_t>(uxQueueMessagesWaiting(s_freeQ));
        const uint16_t inflight = static_cast<uint16_t>(POOL_SIZE - freeSlots);
        const uint16_t laneDepth = static_cast<uint16_t>(uxQueueMessagesWaiting(s_laneQ[laneIndex]));
        uint16_t prevLanePeak = __atomic_load_n(&s_lanePeakDepth[laneIndex], __ATOMIC_RELAXED);
        if (laneDepth > prevLanePeak) {
            __atomic_store_n(&s_lanePeakDepth[laneIndex], laneDepth, __ATOMIC_RELAXED);
        }
        uint32_t prevHigh = __atomic_load_n(&s_inflightHigh, __ATOMIC_RELAXED);
        if (static_cast<uint32_t>(inflight) > prevHigh) {
            __atomic_store_n(&s_inflightHigh,
                             static_cast<uint32_t>(inflight),
                             __ATOMIC_RELAXED);
        }
        __atomic_store_n(&s_pressureState,
                         _pressureStateForFreeSlots(freeSlots),
                         __ATOMIC_RELAXED);

        if (s_workerTask) {
            xTaskNotifyGive(s_workerTask);
        }
        ok = true;
    } while (false);
    xSemaphoreGive(s_enqueueMux);
    return ok;
}

Stats snapshot() {
    Stats s = {};
    s.enqueued            = __atomic_load_n(&s_enqueued, __ATOMIC_RELAXED);
    s.droppedPoolFull     = __atomic_load_n(&s_droppedPoolFull, __ATOMIC_RELAXED);
    s.droppedTooLarge     = __atomic_load_n(&s_droppedTooLarge, __ATOMIC_RELAXED);
    s.droppedNoiseSquelch = __atomic_load_n(&s_droppedNoiseSquelch, __ATOMIC_RELAXED);
    s.consumed            = __atomic_load_n(&s_consumed, __ATOMIC_RELAXED);
    s.inflightHigh        = static_cast<uint16_t>(
        __atomic_load_n(&s_inflightHigh, __ATOMIC_RELAXED));
    s.freeSlots = s_freeQ
        ? static_cast<uint16_t>(uxQueueMessagesWaiting(s_freeQ)) : 0;
    s.inflight = static_cast<uint16_t>(POOL_SIZE - s.freeSlots);
    s.producerProbeOk     = __atomic_load_n(&s_producerProbeOk, __ATOMIC_RELAXED);
    s.producerProbeDrop   = __atomic_load_n(&s_producerProbeDrop, __ATOMIC_RELAXED);
    s.producerDeviceOk    = __atomic_load_n(&s_producerDeviceOk, __ATOMIC_RELAXED);
    s.producerDeviceDrop  = __atomic_load_n(&s_producerDeviceDrop, __ATOMIC_RELAXED);
    s.workerProbeOk       = __atomic_load_n(&s_workerProbeOk, __ATOMIC_RELAXED);
    s.workerProbeSuppressed = __atomic_load_n(&s_workerProbeSuppressed, __ATOMIC_RELAXED);
    s.workerProbeDropped   = __atomic_load_n(&s_workerProbeDropped, __ATOMIC_RELAXED);
    s.workerProbeFail     = __atomic_load_n(&s_workerProbeFail, __ATOMIC_RELAXED);
    s.workerProbeSlow     = __atomic_load_n(&s_workerProbeSlow, __ATOMIC_RELAXED);
    s.workerDeviceOk      = __atomic_load_n(&s_workerDeviceOk, __ATOMIC_RELAXED);
    s.workerDeviceSuppressed = __atomic_load_n(&s_workerDeviceSuppressed, __ATOMIC_RELAXED);
    s.workerDeviceDropped = __atomic_load_n(&s_workerDeviceDropped, __ATOMIC_RELAXED);
    s.workerDeviceFail    = __atomic_load_n(&s_workerDeviceFail, __ATOMIC_RELAXED);
    s.workerDeviceSlow    = __atomic_load_n(&s_workerDeviceSlow, __ATOMIC_RELAXED);
    s.workerSlow          = __atomic_load_n(&s_workerSlow, __ATOMIC_RELAXED);
    s.workerSlowMaxMs     = __atomic_load_n(&s_workerSlowMax, __ATOMIC_RELAXED);
    s.p3EvictedOldest     = __atomic_load_n(&s_p3EvictedOldest, __ATOMIC_RELAXED);
    s.p3PressureDrop     = __atomic_load_n(&s_p3PressureDrop, __ATOMIC_RELAXED);
    s.p2PressureDrop     = __atomic_load_n(&s_p2PressureDrop, __ATOMIC_RELAXED);
    s.p1EnqueueFail      = __atomic_load_n(&s_p1EnqueueFail, __ATOMIC_RELAXED);
    s.priorityP0Enqueued = __atomic_load_n(&s_priorityEnqueued[0], __ATOMIC_RELAXED);
    s.priorityP1Enqueued = __atomic_load_n(&s_priorityEnqueued[1], __ATOMIC_RELAXED);
    s.priorityP2Enqueued = __atomic_load_n(&s_priorityEnqueued[2], __ATOMIC_RELAXED);
    s.priorityP3Enqueued = __atomic_load_n(&s_priorityEnqueued[3], __ATOMIC_RELAXED);
    s.pressureState      = __atomic_load_n(&s_pressureState, __ATOMIC_RELAXED);
    s.laneP1Depth       = s_laneQ[1] ? static_cast<uint16_t>(uxQueueMessagesWaiting(s_laneQ[1])) : 0;
    s.laneP2Depth       = s_laneQ[2] ? static_cast<uint16_t>(uxQueueMessagesWaiting(s_laneQ[2])) : 0;
    s.laneP3Depth       = s_laneQ[3] ? static_cast<uint16_t>(uxQueueMessagesWaiting(s_laneQ[3])) : 0;
    s.laneP1PeakDepth   = __atomic_load_n(&s_lanePeakDepth[1], __ATOMIC_RELAXED);
    s.laneP2PeakDepth   = __atomic_load_n(&s_lanePeakDepth[2], __ATOMIC_RELAXED);
    s.laneP3PeakDepth   = __atomic_load_n(&s_lanePeakDepth[3], __ATOMIC_RELAXED);
    return s;
}

void logStats() {
    if (!isReady()) return;
    Stats s = snapshot();
    DLOG_INFO("STORAGE",
              "RAMSpool enq=%lu cons=%lu inflight=%u/%u high=%u pressure=%u "
              "drop[full=%lu big=%lu noise=%lu p3Evict=%lu p3Drop=%lu p2Drop=%lu p1Fail=%lu] free=%u",
              (unsigned long)s.enqueued,
              (unsigned long)s.consumed,
              (unsigned)s.inflight,
              (unsigned)POOL_SIZE,
              (unsigned)s.inflightHigh,
              (unsigned)s.pressureState,
              (unsigned long)s.droppedPoolFull,
              (unsigned long)s.droppedTooLarge,
              (unsigned long)s.droppedNoiseSquelch,
              (unsigned long)s.p3EvictedOldest,
              (unsigned long)s.p3PressureDrop,
              (unsigned long)s.p2PressureDrop,
              (unsigned long)s.p1EnqueueFail,
              (unsigned)s.freeSlots);
    DLOG_INFO("STORAGE",
              "RAMSpool enqueue probe[ok=%lu drop=%lu] device[ok=%lu drop=%lu]",
              (unsigned long)s.producerProbeOk,
              (unsigned long)s.producerProbeDrop,
              (unsigned long)s.producerDeviceOk,
              (unsigned long)s.producerDeviceDrop);
    DLOG_INFO("STORAGE",
              "RAMSpool worker probe[ok=%lu sup=%lu drop=%lu fail=%lu slow=%lu] "
              "device[ok=%lu sup=%lu drop=%lu fail=%lu slow=%lu] slowTotal=%lu maxMs=%lu",
              (unsigned long)s.workerProbeOk,
              (unsigned long)s.workerProbeSuppressed,
              (unsigned long)s.workerProbeDropped,
              (unsigned long)s.workerProbeFail,
              (unsigned long)s.workerProbeSlow,
              (unsigned long)s.workerDeviceOk,
              (unsigned long)s.workerDeviceSuppressed,
              (unsigned long)s.workerDeviceDropped,
              (unsigned long)s.workerDeviceFail,
              (unsigned long)s.workerDeviceSlow,
              (unsigned long)s.workerSlow,
              (unsigned long)s.workerSlowMaxMs);
    DLOG_INFO("STORAGE",
              "RAMSpool prioEnq p0=%lu p1=%lu p2=%lu p3=%lu",
              (unsigned long)s.priorityP0Enqueued,
              (unsigned long)s.priorityP1Enqueued,
              (unsigned long)s.priorityP2Enqueued,
              (unsigned long)s.priorityP3Enqueued);
    DLOG_INFO("STORAGE",
              "RAMSpool lanes p1=%u/%u p2=%u/%u p3=%u/%u",
              (unsigned)s.laneP1Depth,
              (unsigned)s.laneP1PeakDepth,
              (unsigned)s.laneP2Depth,
              (unsigned)s.laneP2PeakDepth,
              (unsigned)s.laneP3Depth,
              (unsigned)s.laneP3PeakDepth);
}

}  // namespace RAMSpool
