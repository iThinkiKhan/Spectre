# Spectre field-week closeout — 2026-08-16

## Outcome

- Flashed the UI/upload firmware, diagnosed and fixed the real bulk-upload failure, and then delivered the full pre-closeout mission spool: **4,461/4,461 broker-acknowledged, 0 failed, 0 remaining**.
- Captured and analyzed FieldVault before cleanup. The live cursor was drained repeatedly with QoS1 acknowledgement (8 + 8 + 8 + 7 + 2 records in the final pass); the retained backup was preserved in the closeout log for analysis.
- The final firmware revision includes the upload-memory fix and the cross-segment enrichment-counter reconciliation fix. Its image was flashed and hash-verified.
- The last field soak remained firmware-clean for just over five minutes, including two BLE handoff cycles and live phone enrichment, but Windows then lost the entire ESP32 USB device. Because neither COM13 nor COM8 returned, the final test-record upload and FieldVault clear could not be performed.

## Week in the field

The authoritative pre-upload audit found:

| Measure | Result |
|---|---:|
| Total durable spool records | 7,025 |
| Captured event records | 4,474 |
| Enrichment delta records | 2,551 |
| Pending mission/event uploads | 4,461 |
| Already-uploaded retained events | 13 |
| Sessions | 39 |
| Valid segments | 67 |
| Storage used | about 2.62 MB |
| Invalid records | 0 |
| Quarantined records/segments | 0 |
| Unreadable segments | 0 |

The week’s retained FieldVault window contained 462 parsed records across sequence 6902–7369:

| FieldVault type | Count |
|---|---:|
| `run_sample` | 385 |
| `power_sample` | 51 |
| `boot` | 14 |
| `fs_audit_summary` | 6 |
| `reset_crash` | 4 |
| `upload_summary` | 2 |

All six filesystem audits were clean (97–102 files per audit, no invalid or unknown files). USB power stayed at 4,560–4,722 mV and 100%. The retained two-rotation window covered 16 session IDs; the longest individual runs were about 9.3 hours and 2.4 hours. The four retained crash records were older task-watchdog/panic evidence from the BLE/storage debugging period; the later stable run followed those fixes.

## Enrichment qualification

The first closeout pass reported a successful phone run (`requested=2033 applied=2033 failed=0`) and retired no-data candidates, but the later authoritative cross-segment audit exposed that the fast pending counter had incorrectly reached zero. Before delivery, **3,018 events still had no joined enrichment delta**. They were uploaded with their original data and explicit pending/unknown enrichment state; no location was fabricated.

The cause was a summary-model defect: an event segment was counted independently even though its enrichment delta could live in a later segment. The final firmware now performs a whole-spool delta-ID join before persisting audit-derived pending-enrichment counters. During the final field test, the activated phone authenticated successfully and enriched 26 newly captured events in two batches (18 + 8, zero failures), confirming the live phone path.

## Bulk upload diagnosis and repair

The original full-backlog attempt timed out on the first 533-byte event while startup FieldVault records uploaded normally. A workstation MQTT probe proved that the broker accepted QoS1 payloads from 300 through 600 bytes in 2–17 ms, ruling out a packet-size limit.

The decisive difference was internal RAM:

| State | Internal RAM free |
|---|---:|
| Original 4,459-record resident index | about 2 KB |
| Fixed 4,461-record resident index | 81.6 KB at prepare, about 65 KB while streaming |

Enrichment tags were held in thousands of Arduino `String` allocations in internal RAM even though the enclosing map nodes were in PSRAM. Tags now live inline in the PSRAM-backed node. The previously failing 533-byte event then received PUBACK in 14 ms, followed by all 4,461 records with zero failures.

## UI and controls

- Page order is now field-useful: Missions, Wi-Fi/Entities, Sub-GHz, Meshtastic, BLE/Phone, System, BadUSB, History.
- The top bar uses fixed non-overlapping slots and explicit states (`WIFI RX`, `WIFI UP`, `BLE GPS`, `PHONE+`, `SG ON`) plus the full page name.
- Mission names, metadata, objectives, and posture lines were shortened/rebalanced to fit the 320×128 display without overruns.
- Device upload is a first-class `UPLOAD` action from the Uplink mission and existing System/History long-button paths.
- Phone Mission view now has a prominent **Upload to broker** action with queued/uploading/nothing/error states.
- The phone command now schedules a real forced upload after returning its authenticated response; the prior implementation only resumed scheduling while reporting success.

## Stability and diagnostic results

- Prior final field run: 7 minutes monotonic, four companion probes, 26 clean scan stops, zero errors/panics/watchdogs/assertions/contracts/drops.
- Real backlog upload: 4,461 QoS1 acknowledgements, zero failures, stable internal heap.
- Final firmware field run before USB loss: two BLE probe/offload-preparation/cancel cycles, four successful phone authentications, live time sync, 26/26 enrichments, no panic, watchdog, assertion, contract violation, queue drop, invalid record, or quarantine.
- The USB device disappeared at approximately 311 seconds of the final soak. No COM13 application port, COM8 bootloader port, or ESP32 USB parent remained in Windows after an additional 60-second wait. The on-device log immediately before disappearance was healthy, so this is presently classified as an external USB/power disconnect, not a confirmed firmware failure.

## Build artifacts

- Firmware: `C:\PlatformIO\_build\Spectre\esp32-s3-devkitc1-n16r8\firmware.bin`
  - 2,296,096 bytes
  - SHA-256 `74B02028B4A5F49230B4C2FE6C8F477138A2FC1F2283BAA60D19BB560BEF8D5A`
- Android debug APK: `C:\PlatformIO\Projects\Spectre\companion_app\android\app\build\outputs\apk\debug\app-debug.apk`
  - 116,043,201 bytes
  - SHA-256 `A65A50DA189DE03322CB9C7272FA934D874D031E5148BD1BACF5FA8275A55C3E`
- Firmware compile: clean; RAM 33.9%, flash 54.7%.
- Phone TypeScript check and Android `assembleDebug`: clean.

## Remaining closeout action

Reconnect the Spectre USB device. Then upload the roughly 50 records generated by the final field test, run the final spool/FieldVault audit, clear the addressed FieldVault, and install the APK if the phone is attached through ADB. No ADB device was available during this run.
