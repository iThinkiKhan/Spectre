# Entity storage tabletop — 80% full

## Starting state

- LittleFS partition: `0x7F0000` = 8,323,072 bytes (7.94 MiB).
- At the WATCH threshold (80%): about 6.35 MiB used and 1.59 MiB free.
- Entity table: 8,192 Entities plus 98,304 event links in a 131,072-slot (75% maximum load) PSRAM index, about 3.0 MiB measured on-device. It consumes no LittleFS space.
- Enrichment batch: 18 records. Preflight budgets 96 bytes per delta plus 4 KiB batch/rotation overhead and preserves a 256 KiB metadata reserve.
- Upload bucket: 64 records in PSRAM. Upload reads resolved events, so persisted enrichment fields (`lat`, `lon`, `alt`, `acc`, tag, GPS time, and enrichment state) remain in the MQTT payload.

## Walkthrough and invariants

### 1. Capture at 80%

WATCH keeps the normal retention policy, so P0–P3 records can still be accepted. Crossing into WATCH now schedules `delete_drained` maintenance instead of merely changing the warning icon. Each accepted record passes one boundary:

`RAMSpool -> durable spool append -> StorageManager counter delta -> EntityManager observation`

An enqueue eviction, policy drop, dedup suppression, parse failure, or I/O failure cannot increment Entity or session counters.

### 2. Enrichment at 80%

Before an enrichment batch, the filesystem is measured rather than relying on an old UI sample. The batch proceeds only when its worst-case budget plus the 256 KiB reserve fits. A successful delta is then applied to the event-to-Entity link exactly once. A no-data delta closes the enrichment obligation without inventing a `(0,0)` location.

Observer location and a drone's self-reported location are stored as different facts. A PMKID/handshake location applies to both AP and client Entities. If preflight cannot preserve the reserve, the source records stay pending and reclaim maintenance is scheduled.

### 3. Upload at 80%

The upload index and 64-record publish bucket remain in PSRAM. The resolved spool reader overlays enrichment deltas before MQTT serialization; the publisher removes only local bookkeeping fields and keeps enrichment content. QoS acknowledgement advances the upload watermark through the single StorageManager counter-delta path.

No record is removed merely because it was staged or published. Only acknowledged watermarks make a segment drainable.

### 4. Reclaim after upload

`delete_drained` rotates a fully uploaded active segment, gives the just-closed file one grace pass, then deletes only segments whose pending scan is empty. Failed scans or failed deletes retain the segment, degrade counter trust, and schedule an audit.

### 5. Continuing toward FULL and OVERRUN

- At 92% (FULL), retention becomes reduced: P3 is rejected.
- At 97% (OVERRUN), only P0/P1 remains eligible.
- Below 256 KiB free, P2/P3 are rejected even if a cached percentage lags.
- At or below 64 KiB free, all new event writes are rejected to leave recovery room.
- Enrichment never consumes the 256 KiB metadata reserve.

## Capacity findings

The Entity design scales without additional flash, but enrichment deltas still scale linearly with observations. At a conservative 96-byte budget, 80,000 deltas can require up to 7.3 MiB; an already-80%-full filesystem cannot enrich that entire backlog before upload/reclaim. The safe operating sequence is therefore iterative:

1. enrich while the reserve permits;
2. upload acknowledged resolved records;
3. reclaim drained segments;
4. continue enrichment/upload if pending records remain.

The next major storage optimization should be segment folding: rewrite a sealed event segment with its enrichment fields materialized, then atomically replace the event-plus-delta pair. That can reclaim delta overhead, but it needs a dedicated power-loss protocol and scratch-space proof; it is not safe to improvise in the current append-only format.

Boot reconstruction is also linear in retained records. The current 742-record rebuild took 2,422 ms, but an 80%-full filesystem has not been populated destructively for a timing test. Profile that state before setting a boot-time target. A durable Entity checkpoint could shorten startup, but it would add another flash consistency contract; sealed-segment folding is the better point to evaluate both problems together.

## Verification evidence

- Final build: internal RAM 39.4% (129,076 / 327,680), flash 54.1% (2,270,209 / 4,194,304); application image 2,270,720 bytes.
- Final live S3 trace: `diagnostics/entity-system-boot-selftest-final-20260808.log`.
- Boot contract self-test passed before capture: a synthetic observation and location enrichment remained idempotent when repeated, then the synthetic PSRAM state was discarded before reconstruction from the physical spool.
- Boot rebuild: 26 Entities from 742 retained observations in 2,422 ms; zero Entity/link drops.
- After the final capture trace: 764 durable observations. Session counters were 2 devices and 3 probes, exactly matching worker acceptance (2 devices + 3 probes) while excluding 6 dedup-suppressed probes.
- Live PSRAM after rebuild and active capture: 4,495 KiB free; Entity allocation reported 3,088 KiB.
- Live RAM spool: 1,024/1,024 slots free after the observed batch, with zero pool, size, pressure, or priority drops.
