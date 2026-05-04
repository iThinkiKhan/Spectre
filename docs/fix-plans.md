# Fix Plans — Four Identified Bugs

---

## Fix 1: LittleFS inline flush (wrong ordering in MQTT_DONE / MQTT_FAILED)

### What's broken

`RADIO_ARB.release()` is not a passive state clear — it calls `_stopOwner()`,
`_clearActiveOwnerState()`, then `_serviceIdleOwner()`. That last call finds
`_fallbackOwner == RADIO_WIFI_CAPTURE` and immediately calls
`ensureDefaultCapture()` → `_switchTo(RADIO_WIFI_CAPTURE)` →
`WIFI_MGR.startPromiscuous()`, all **synchronously before `release()` returns**.

So in both `MQTT_DONE` and `MQTT_FAILED`, the call that follows `release()` —
`STORAGE.endUploadBatch()` — runs while WiFi is already live in promiscuous
mode, negating the entire batch-deferred-flush mechanism.

The comment "now is the quietest window" is factually wrong.

**Files:** `MQTTManager.cpp`, `StorageManager.cpp`

---

### Plan

**Step 1 — Swap `endUploadBatch()` before `release()` in both terminal states.**

In `MQTT_DONE` (and identically in `MQTT_FAILED`), change the order to:

```cpp
case MQTT_DONE:
    _setUploadUiState(false, "", 0, 0, false);
    _disconnect();

    // Flush BEFORE release. release() calls _serviceIdleOwner() which
    // immediately starts promiscuous capture — by the time release() returns
    // the radio is already live. Flush now while RADIO_WIFI_UPLOAD still
    // holds (WiFi is paused, not scanning/transmitting).
    STORAGE.endUploadBatch();
    _refreshPendingCount(true);

    RADIO_ARB.release(RADIO_WIFI_UPLOAD, "dump_complete");
    // ensureDefaultCapture() removed — release() calls _serviceIdleOwner()
    // which handles the capture fallback internally.
    DLOG_INFO("MQTT", "Dump complete");
    _lastDumpMs = millis();
    _state = MQTT_IDLE;
    break;
```

Apply the same swap to `MQTT_FAILED`.

**Step 2 — Fix the contract check in `StorageManager::endUploadBatch()`.**

The existing check fires when the batch closes while `RADIO_WIFI_UPLOAD` is
still the owner — which is now the *intended* path. The contract's intent was
"don't flush to flash while radio is actively RF-on," not "don't flush while
the upload lease is held." `RADIO_WIFI_UPLOAD` means `pauseRadio()` has been
called, so WiFi is already idle; the constraint is satisfied.

Change the contract condition to check the actual radio-activity condition:

```cpp
// Before (wrong predicate):
CONTRACT_WARN_ONCE(CONTRACT_UPLOAD_BATCH_OWNER_SYNC,
                   "STORAGE",
                   !_uploadBatchActive || !RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD),
                   "upload batch closing while radio owner still=%s", ...);

// After (correct predicate — UPLOAD owner is fine, active-RF owners are not):
CONTRACT_WARN_ONCE(CONTRACT_UPLOAD_BATCH_OWNER_SYNC,
                   "STORAGE",
                   !_uploadBatchActive ||
                       RADIO_ARB.isOwner(RADIO_WIFI_UPLOAD) ||
                       RADIO_ARB.currentOwner() == RADIO_NONE,
                   "upload batch closing while active-RF owner=%s", ...);
```

**Step 3 — Remove the now-redundant `ensureDefaultCapture()` calls.**

The trailing `RADIO_ARB.ensureDefaultCapture("mqtt_done")` and
`ensureDefaultCapture("mqtt_failed")` in both states are no-ops after the
reorder: `release()` already called `_serviceIdleOwner()` which called
`ensureDefaultCapture()` internally. Remove them and add a comment explaining
why they're gone.

**Step 4 — Update the comment in `MQTTManager.cpp`.**

Replace the misleading "Radio has been released + broker disconnected — now is
the quietest window" comment with one that accurately describes the new
ordering and explains why the batch must close before release.

**Verification:** Add a DLOG in `_persistSpoolIndex()` that logs the current
radio owner. After the fix, it should always show `RADIO_WIFI_UPLOAD` or
`RADIO_NONE`, never `RADIO_WIFI_CAPTURE`.

---

## Fix 2: portMUX-as-mutex in BLEManager

### What's broken

`BLEManager` uses `portMUX_TYPE _mux` (a spinlock) with
`portENTER_CRITICAL` / `portEXIT_CRITICAL` to guard its RX queues. Those
primitives disable interrupts on the current core for the duration of the
critical section.

The callbacks that write to the queues (`_queueGpsFrameFromCallback`,
`_queueControlFrameFromCallback`, `_queueEnrichmentChunkFromCallback`) are
invoked from NimBLE's internal FreeRTOS task — **not from an ISR**. Both the
enqueue side (NimBLE task) and the drain side (`_drainBleRxQueues()` in
TaskHardware) are regular tasks. Using `portENTER_CRITICAL` for task-to-task
sync is architecturally wrong: it pins interrupts off across both cores for the
duration.

The concrete harm: `_queueEnrichmentChunkFromCallback` and
`_drainEnrichmentRx` both hold the spinlock while copying up to 244 bytes
(`ENRICH_RX_CHUNK_MAX`). That's a non-trivial interrupt blackout window on
every enrichment chunk, which can disturb BLE timing and other ISR-driven
peripherals.

**Files:** `BLEManager.h`, `BLEManager.cpp`

---

### Plan

**Step 1 — Replace `portMUX_TYPE _mux` with a FreeRTOS mutex.**

In `BLEManager.h`:

```cpp
// Remove:
mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

// Add:
SemaphoreHandle_t _rxMutex = nullptr;
```

**Step 2 — Initialize and destroy the mutex.**

In `BLEManager::begin()`, before the NimBLE init:

```cpp
if (!_rxMutex) {
    _rxMutex = xSemaphoreCreateMutex();
    if (!_rxMutex) {
        DLOG_ERROR(TAG, "Failed to create BLE RX mutex");
        return false;
    }
}
```

In the `failBegin` lambda (and any matching teardown path), add:

```cpp
if (_rxMutex) {
    vSemaphoreDelete(_rxMutex);
    _rxMutex = nullptr;
}
```

**Step 3 — Replace all `portENTER_CRITICAL` / `portEXIT_CRITICAL` pairs.**

Search for every `portENTER_CRITICAL(&_mux)` in `BLEManager.cpp` and replace:

```cpp
// Before:
portENTER_CRITICAL(&_mux);
...
portEXIT_CRITICAL(&_mux);

// After:
xSemaphoreTake(_rxMutex, portMAX_DELAY);
...
xSemaphoreGive(_rxMutex);
```

There are ~12 sites across the file. All pairs are symmetric (no early-exit
paths that skip the release), so mechanical replacement is safe. Verify no
`portEXIT_CRITICAL` appears inside an early-return branch without a matching
`portENTER_CRITICAL` — the drain loop in `_drainEnrichmentRx` does have a
conditional `portEXIT_CRITICAL` inside the `if (_enrichRxDrops > 0)` block;
replace that with `xSemaphoreGive` before the early `break`.

**Step 4 — Narrow the critical section in `_queueEnrichmentChunkFromCallback`.**

Even with a mutex instead of a spinlock, holding it across a 244-byte memcpy
blocks the drain side unnecessarily. Restructure enqueue to copy outside the
lock:

```cpp
void BLEManager::_queueEnrichmentChunkFromCallback(const uint8_t* data, size_t len) {
    if (!data || len == 0 || len > ENRICH_RX_CHUNK_MAX) {
        xSemaphoreTake(_rxMutex, portMAX_DELAY);
        _enrichRxDrops++;
        xSemaphoreGive(_rxMutex);
        return;
    }

    // Copy into local scratch BEFORE acquiring the mutex — interrupts stay on,
    // and the mutex holder (drain side) can finish without contention.
    uint8_t local[ENRICH_RX_CHUNK_MAX];
    memcpy(local, data, len);

    xSemaphoreTake(_rxMutex, portMAX_DELAY);
    if (_enrichRxCount >= ENRICH_RX_SLOTS) {
        _enrichRxDrops++;
        xSemaphoreGive(_rxMutex);
        return;
    }
    BleRxChunk& slot = _enrichRx[_enrichRxTail];
    slot.len = static_cast<uint16_t>(len);
    memcpy(slot.data, local, len);   // slot→slot copy; still short
    _enrichRxTail = (_enrichRxTail + 1) % ENRICH_RX_SLOTS;
    _enrichRxCount++;
    xSemaphoreGive(_rxMutex);
}
```

Apply the same pre-copy pattern to `_queueGpsFrameFromCallback` and
`_queueControlFrameFromCallback` for consistency, though their payloads are
smaller and the impact is lower.

**Verification:** After the change, use `uxSemaphoreGetCount()` in a debug log
to confirm the mutex is not contended (count always 1 at tick boundaries). Run
a BLE enrichment session and confirm no dropped chunks appear in the log that
weren't present before.

---

## Fix 3: BLE handoff delay — double 150ms settle in RadioArbiter

### What's broken

`RadioArbiter::_startOwner()` for BLE modes:

```cpp
WIFI_MGR.suspendRadio();   // internally: disconnect + delay(150) + WiFi.mode(WIFI_OFF)
delay(150);                // ← redundant second settle added on top
if (!BLE_MGR.begin()) { ...
```

`suspendRadio()` already performs the full WiFi teardown sequence including the
150ms drain for in-flight disconnect events, and sets the mode to `WIFI_OFF`
before returning. The 300ms total means every WiFi→BLE handoff blocks
TaskHardware for 300ms during which no arbiter ticks can run, no mission state
can advance, and no button events are processed.

Additionally, neither delay is event-driven — they're empirical guesses with no
confirmation that the WiFi driver has actually quiesced.

**Files:** `RadioArbiter.cpp`, `WiFiManager.cpp`

---

### Plan

**Step 1 — Remove the redundant delay in `_startOwner()`.**

The extra 150ms after `suspendRadio()` returns provides no additional
guarantee. `suspendRadio()` already:
1. Calls `esp_wifi_set_promiscuous(false)` (synchronous)
2. Calls `WiFi.disconnect(true, true)` (queues disconnect)
3. Waits 150ms for the in-flight disconnect event to drain
4. Calls `WiFi.mode(WIFI_OFF)` (transitions driver to idle)

By the time `suspendRadio()` returns, the RF chain is idle. Remove the
`delay(150)` from `_startOwner()`:

```cpp
case RADIO_BLE_TEXT:
case RADIO_BLE_GPS:
    WIFI_MGR.suspendRadio();
    // No additional settle needed — suspendRadio() completes the full
    // WiFi driver teardown including the disconnect-event drain.
    if (!BLE_MGR.begin()) {
        DLOG_ERROR(TAG, "BLE begin failed");
        release(owner, "ble_begin_failed");
        return false;
    }
    BLE_MGR.setRadioEnabled(true);
    return true;
```

**Step 2 — Make `suspendRadio()`'s drain event-based (replaces the fixed
150ms).**

The 150ms in `suspendRadio()` is the real remaining empirical delay. It was
added to cover the race documented in the memory notes. Replace it with a
poll loop that exits as soon as the mode confirms as `WIFI_MODE_NULL`, with
the 150ms as a hard ceiling:

```cpp
void WiFiManager::suspendRadio() {
    esp_wifi_set_promiscuous(false);
    WiFi.disconnect(true, true);

    // Poll until the driver reports WIFI_MODE_NULL (disconnect event drained)
    // instead of a fixed delay. Cap at 150ms for hardware that's slow to drain.
    {
        const uint32_t deadline = millis() + 150;
        wifi_mode_t m = WIFI_MODE_STA;
        while (millis() < deadline) {
            if (esp_wifi_get_mode(&m) == ESP_OK && m == WIFI_MODE_NULL) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (m != WIFI_MODE_NULL) {
            DLOG_WARN("WIFI", "suspendRadio: drain timeout, mode=%d", m);
        }
    }

    WiFi.mode(WIFI_OFF);
    _mode = WIFI_OP_IDLE;
    _radioReady = false;
    _disarmPwny("SUSPENDED");
    ...
}
```

Apply the same pattern to `stopAll()` which has an identical `delay(150)` with
the same drain intent.

**Step 3 — Apply the same poll-loop pattern to `_ensureRadioReady()`.**

`_ensureRadioReady()` has three sequential fixed delays (150 + 40 + 120ms =
310ms). Replace each with a mode-confirmation poll:

```cpp
// Replace delay(150) + WiFi.mode(WIFI_OFF):
{
    esp_wifi_stop();
    const uint32_t dl = millis() + 150;
    wifi_mode_t m = WIFI_MODE_STA;
    while (millis() < dl) {
        if (esp_wifi_get_mode(&m) == ESP_OK && m == WIFI_MODE_NULL) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
WiFi.mode(WIFI_OFF);

// Replace delay(40) between WIFI_OFF and WIFI_STA:
// Keep as vTaskDelay(pdMS_TO_TICKS(40)) — this is a driver-settle between
// mode transitions that has no event to poll against.

// Replace delay(120) after WiFi.mode(WIFI_STA):
{
    const uint32_t dl = millis() + 120;
    wifi_mode_t m = WIFI_MODE_NULL;
    while (millis() < dl) {
        if (esp_wifi_get_mode(&m) == ESP_OK && m == WIFI_MODE_STA) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

The 40ms between WIFI_OFF→WIFI_STA is the one delay that genuinely has nothing
to poll — keep it, but rename to `vTaskDelay(pdMS_TO_TICKS(40))` to make the
RTOS yield explicit.

**Verification:** Log `millis()` before and after each former `delay()` site.
Compare actual wait time vs 150ms ceiling in normal conditions. On fast
hardware the drain should complete well under 150ms; the poll version will exit
early and reclaim that latency on every BLE handoff.

---

## Fix 4: radio-task `delay()` calls — blocking WiFi connect loop

### What's broken

`WiFiManager::connectTo()` contains a synchronous blocking poll:

```cpp
WiFi.begin(ssid, pass);
uint32_t start = millis();
while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(200);
}
```

This blocks the calling task for up to 10 seconds. `delay()` on ESP32-Arduino
is `vTaskDelay()`, so it yields the scheduler, but the task loop itself cannot
make forward progress for the entire connect window — no button presses, no
arbiter ticks, no mission state updates.

The good news: `connectTo()` is currently dead code. A grep of the entire `src/`
tree shows it is defined in `WiFiManager.cpp` and declared in `WiFiManager.h`,
but **never called from any other file**. The MQTT path uses the non-blocking
`_connectWiFi()` → `WiFi.begin()` + state machine tick pattern instead.

However, the function is `public`, so it can be called by future callers, and
the pattern is wrong.

**File:** `WiFiManager.cpp`, `WiFiManager.h`

---

### Plan

**Step 1 — Delete or privatize `connectTo()`.**

Since `connectTo()` is dead code, the cleanest fix is to remove it:
- Delete the implementation in `WiFiManager.cpp`
- Remove the declaration from `WiFiManager.h`

If there's intent to keep it for future use (e.g., provisioning flows), move
the declaration to `private` and add a `static_assert(false)` or `#error`
comment to prevent accidental use until it's converted to non-blocking.

**Step 2 — If `connectTo()` must stay, convert it to a state-machine pattern.**

The non-blocking version returns immediately after `WiFi.begin()` and exposes
a `connectTick()` method that the caller pumps:

```cpp
// WiFiManager.h — new interface:
bool connectToBegin(const char* ssid, const char* pass);  // starts connect, returns immediately
bool connectToTick();   // call each task tick; returns true when done (connected or failed)
bool connectToResult(); // returns true if last connect succeeded

// WiFiManager.cpp:
bool WiFiManager::connectToBegin(const char* ssid, const char* pass) {
    if (!_ensureRadioReady()) return false;
    esp_wifi_set_promiscuous(false);
    _mode = WIFI_OP_CONNECT;
    _connectStartMs = millis();
    WiFi.begin(ssid, pass);
    return true;
}

bool WiFiManager::connectToTick() {
    if (_mode != WIFI_OP_CONNECT) return true;  // already resolved
    if (WiFi.status() == WL_CONNECTED) {
        _mode = WIFI_OP_CONNECTED;
        ...
        return true;
    }
    if (millis() - _connectStartMs > 10000) {
        DLOG_WARN("WIFI", "Connect timeout");
        _mode = WIFI_OP_IDLE;
        return true;
    }
    return false;
}
```

**Step 3 — Audit remaining `delay()` calls in `WiFiManager.cpp` for correctness.**

The remaining `delay()` / `vTaskDelay()` sites are the settle chains addressed
in Fix 3 above. After applying Fix 3 those become poll loops. No other blocking
loops remain in WiFiManager.

**Verification:** With `connectTo()` deleted, the build will catch any callsite
that was missed by the grep. If the function is kept and converted, run a
WiFi-connect scenario and log the task at each tick to confirm TaskHardware
stays responsive (processes button events, advances arbiter) during the connect
wait.

---

## Suggested fix order

1. **LittleFS flush ordering** — highest risk, affects every successful upload,
   root cause is a misleading comment that made the bug invisible.

2. **portMUX-as-mutex** — architecturally wrong, but current payload sizes
   mean the interrupt window is short. Medium risk; easier to review because
   the change is mechanical.

3. **BLE handoff delay** — safe to remove the second 150ms immediately; the
   poll-loop improvements to `suspendRadio()` and `_ensureRadioReady()` are
   independent and can be staged separately.

4. **`connectTo()` dead code** — lowest urgency; delete it or mark private
   before someone calls it from a provisioning screen.
