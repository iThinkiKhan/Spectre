
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// RAMSpool — dynamic priority-lane spool for the pinned storage worker.
// Shared 1024-slot pool; the producer classifies events cheaply before
// enqueue, and the worker drains lanes in priority order.
//
// Producer-side fast path: capture tasks classify, enqueue() the prepared
// payload doc into PSRAM, and post the slot index onto an MPSC queue.
// Returns immediately, no LittleFS.
//
// Consumer-side: TaskStorage (pinned to Core 1) drains the inflight queue
// and persists probe slots through StorageManager::appendQueuedRecord().
//
// Provisional priority/lane/valueScore is computed by the producer cheaply
// (no g_stateMux). Final stored priority and final policy priority are
// recomputed by the storage worker / retention pass when needed.

namespace RAMSpool {

enum EventLane : uint8_t { LANE_NOISE = 0, LANE_MISSION = 1 };

enum SlotKind : uint8_t {
    SLOT_SHADOW = 0,    // not migrated yet; worker drops after wakeup
    SLOT_PROBE = 1,      // migrated probe capture
    SLOT_DEVICE = 2,     // migrated device capture
    SLOT_NOISE_P3 = 3,   // migrated low-value noise/P3 capture
};

// Producer-visible "provisional" priority. Storage worker recomputes the
// final stored priority once it has full event context (envelope etc.).
enum EventPrio : uint8_t {
    PRIO_P0 = 0,  // critical / mission alert
    PRIO_P1 = 1,  // mission
    PRIO_P2 = 2,  // operational
    PRIO_P3 = 3,  // noise default
};

constexpr size_t POOL_SIZE   = 1024;
constexpr size_t MAX_PAYLOAD = 336;  // binary field-map payload bytes per slot

struct EventSlot {
    uint32_t enqueueSeq;
    uint32_t tsMs;
    uint16_t payloadLen;
    uint8_t  provPriority;
    uint8_t  provLane;
    uint8_t  enrichEligible;
    uint8_t  valueScore;
    uint8_t  slotKind;
    char     type[16];
    uint8_t  payload[MAX_PAYLOAD];
};

struct CaptureClassification {
    EventPrio priority = PRIO_P3;
    EventLane  lane = LANE_NOISE;
    bool       enrichEligible = false;
    uint8_t    valueScore = 0;
};

struct Stats {
    uint32_t enqueued;
    uint32_t droppedPoolFull;
    uint32_t droppedTooLarge;
    uint32_t droppedNoiseSquelch;
    uint32_t consumed;
    uint16_t inflight;
    uint16_t inflightHigh;
    uint16_t freeSlots;
    uint16_t laneP1Depth;
    uint16_t laneP2Depth;
    uint16_t laneP3Depth;
    uint8_t  pressureState;

    // Phase 2B — low-value worker write path. Producer side counts every
    // attempt to enqueue a probe/device slot; worker side counts the outcome
    // of the storage append. workerSlow is the count of writes whose wall
    // clock exceeded WORKER_SLOW_MS (logged separately).
    uint32_t producerProbeOk;
    uint32_t producerProbeDrop;
    uint32_t producerDeviceOk;
    uint32_t producerDeviceDrop;
    uint32_t workerProbeOk;
    uint32_t workerProbeSuppressed;
    uint32_t workerProbeDropped;
    uint32_t workerProbeFail;
    uint32_t workerProbeSlow;
    uint32_t workerDeviceOk;
    uint32_t workerDeviceSuppressed;
    uint32_t workerDeviceDropped;
    uint32_t workerDeviceFail;
    uint32_t workerDeviceSlow;
    uint32_t workerSlow;
    uint32_t workerSlowMaxMs;
    uint32_t p3EvictedOldest;
    uint32_t p3PressureDrop;
    uint32_t p2PressureDrop;
    uint32_t p1EnqueueFail;
    uint32_t priorityP0Enqueued;
    uint32_t priorityP1Enqueued;
    uint32_t priorityP2Enqueued;
    uint32_t priorityP3Enqueued;
    uint16_t laneP1PeakDepth;
    uint16_t laneP2PeakDepth;
    uint16_t laneP3PeakDepth;
};

bool   begin();
bool   isReady();

// Upload-mission gate. drainAndPauseWorker() blocks the storage worker from
// dequeuing new slots and waits (up to timeoutMs) for any in-flight slots to
// finish so the upload path can hold the LittleFS lock without contention.
// resumeWorker() releases the gate and wakes the worker. Producers (capture
// path) are already gated by the radio arbiter, so once the worker is paused
// no new flash writes happen from the spool path.
bool   drainAndPauseWorker(uint32_t timeoutMs);
void   resumeWorker();
bool   isWorkerPaused();
CaptureClassification classify(const char* type, const char* eventType);
CaptureClassification classify(const char* type, JsonObjectConst payload);
bool   enqueue(const char* type,
               JsonObjectConst payload,
               SlotKind slotKind,
               const CaptureClassification& classification);
Stats  snapshot();
void   logStats();
void   noteWorkerProbeOk(uint32_t ms);
void   noteWorkerProbeSuppressed(uint32_t ms);
void   noteWorkerProbeDropped(uint32_t ms);
void   noteWorkerProbeFail(uint32_t ms, uint32_t status);
void   noteWorkerDeviceOk(uint32_t ms);
void   noteWorkerDeviceSuppressed(uint32_t ms);
void   noteWorkerDeviceDropped(uint32_t ms);
void   noteWorkerDeviceFail(uint32_t ms, uint32_t status);

}  // namespace RAMSpool

