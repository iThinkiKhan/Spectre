
#pragma once
#ifndef SPECTRE_COMPANION_PROTOCOL_H
#define SPECTRE_COMPANION_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

// Companion BLE wire contract shared by the firmware and phone app.

static constexpr uint8_t COMPANION_PROTOCOL_VERSION = 1;

static constexpr size_t PHONE_GPS_FRAME_SIZE = 20;
static constexpr size_t PHONE_CONTROL_FRAME_SIZE = 4;
static constexpr size_t PHONE_STORAGE_FRAME_SIZE = 68;
static constexpr size_t EVENT_BATCH_RECORD_SIZE = 10;
static constexpr size_t ENRICHMENT_RECORD_SIZE = 47;

static constexpr size_t PHONE_AUTH_NONCE_SIZE = 32;
static constexpr size_t PHONE_AUTH_P256_PUBLIC_KEY_SIZE = 65;
static constexpr size_t PHONE_AUTH_P256_PRIVATE_KEY_SIZE = 32;
static constexpr size_t PHONE_AUTH_P256_SIGNATURE_SIZE = 64;
static constexpr size_t PHONE_AUTH_FRAME_SIZE =
    2 + PHONE_AUTH_NONCE_SIZE + PHONE_AUTH_P256_PUBLIC_KEY_SIZE +
    PHONE_AUTH_P256_SIGNATURE_SIZE;

static constexpr uint8_t PHONE_AUTH_OP_CHALLENGE = 0x01;
static constexpr uint8_t PHONE_AUTH_OP_RESPONSE = 0x02;

static constexpr uint8_t PHONE_SECURE_CHANNEL_GPS = 0x01;
static constexpr uint8_t PHONE_SECURE_CHANNEL_CONTROL = 0x02;
static constexpr uint8_t PHONE_SECURE_CHANNEL_META = 0x03;
static constexpr uint8_t PHONE_SECURE_CHANNEL_EVENT_BATCH = 0x04;
static constexpr uint8_t PHONE_SECURE_CHANNEL_ENRICHMENT = 0x05;
static constexpr uint8_t PHONE_SECURE_CHANNEL_STORAGE = 0x06;
static constexpr uint8_t PHONE_SECURE_CHANNEL_COMMAND = 0x07;
static constexpr uint8_t PHONE_SECURE_CHANNEL_LOG_STREAM = 0x08;
static constexpr uint8_t PHONE_SECURE_CHANNEL_DASHBOARD_STREAM = 0x09;
static constexpr uint8_t PHONE_SECURE_CHANNEL_NOTIFICATION = 0x0a;

static constexpr size_t PHONE_SECURE_HEADER_SIZE = 6;
static constexpr size_t PHONE_SECURE_TAG_SIZE = 16;
static constexpr size_t PHONE_SECURE_ENVELOPE_OVERHEAD =
    PHONE_SECURE_HEADER_SIZE + PHONE_SECURE_TAG_SIZE;

struct __attribute__((packed)) PhoneGpsFrameV1 {
    uint8_t  version;
    int32_t  latE7;
    int32_t  lonE7;
    int32_t  altCm;
    uint16_t accuracyDm;
    uint32_t epochUtc;
    uint8_t  flags;
};

struct __attribute__((packed)) PhoneControlFrameV1 {
    uint8_t  version;
    uint8_t  flags;
    uint16_t counter;
};

struct __attribute__((packed)) EventBatchRecord {
    uint32_t eventId;      // offset  0
    uint32_t timestampMs;  // offset  4; epoch seconds when time is trusted
    uint8_t  type;         // offset  8
    uint8_t  status;       // offset  9
};

struct __attribute__((packed)) EnrichmentRecordWire {
    uint32_t eventId;
    int32_t  latE7;
    int32_t  lonE7;
    int32_t  altCm;
    uint16_t accuracyDm;
    uint32_t epochUtc;
    uint8_t  flags;
    char     tag[24];
};

struct __attribute__((packed)) PhoneStorageFrameV1 {
    uint8_t  version;
    uint8_t  flags;
    uint8_t  storageMode;
    uint8_t  retentionPolicy;
    uint16_t usedPct;
    uint16_t reserved;
    uint32_t freeBytes;
    uint32_t missionTotal;
    uint32_t noiseTotal;
    uint32_t p0Total;
    uint32_t p1Total;
    uint32_t p2Total;
    uint32_t p3Total;
    uint32_t pendingUploadMission;
    uint32_t pendingUploadNoise;
    uint32_t pendingEnrichMission;
    uint32_t pendingEnrichNoise;
    uint32_t enrichmentDeltas;
    uint32_t firstEventId;
    uint32_t lastEventId;
    uint32_t updatedMs;
};

static_assert(sizeof(PhoneGpsFrameV1) == PHONE_GPS_FRAME_SIZE);
static_assert(sizeof(PhoneControlFrameV1) == PHONE_CONTROL_FRAME_SIZE);
static_assert(sizeof(PhoneStorageFrameV1) == PHONE_STORAGE_FRAME_SIZE);
static_assert(sizeof(EventBatchRecord) == EVENT_BATCH_RECORD_SIZE);
static_assert(sizeof(EnrichmentRecordWire) == ENRICHMENT_RECORD_SIZE);

static constexpr uint8_t PHONE_GPS_FLAG_VALID = 0x01;
static constexpr uint8_t PHONE_GPS_FLAG_TIME_TRUSTED = 0x02;

static constexpr uint8_t PHONE_CTRL_FLAG_WG_ACTIVE = 0x01;
static constexpr uint8_t PHONE_CTRL_FLAG_DUMP_REQ = 0x02;
static constexpr uint8_t PHONE_CTRL_FLAG_CANCEL = 0x04;
static constexpr uint8_t PHONE_CTRL_FLAG_BATCH_RX = 0x08;

static constexpr uint8_t PHONE_STORAGE_FLAG_VALID = 0x01;
static constexpr uint8_t PHONE_STORAGE_FLAG_UPLOAD_ACTIVE = 0x02;
static constexpr uint8_t PHONE_STORAGE_FLAG_NEARLY_FULL = 0x04;
static constexpr uint8_t PHONE_STORAGE_FLAG_FULL = 0x08;
static constexpr uint8_t PHONE_STORAGE_FLAG_OVERRUN = 0x10;

static constexpr const char* PHONE_SERVICE_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0001";
static constexpr const char* PHONE_GPS_CHAR_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0002";
static constexpr const char* PHONE_CONTROL_CHAR_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0003";
static constexpr const char* PHONE_META_CHAR_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0004";
static constexpr const char* PHONE_EVENT_BATCH_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0005";
static constexpr const char* PHONE_ENRICHMENT_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0006";
static constexpr const char* PHONE_AUTH_CHAR_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0007";
static constexpr const char* PHONE_STORAGE_CHAR_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0008";
static constexpr const char* PHONE_COMMAND_REQ_CHAR_UUID  = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0009";
static constexpr const char* PHONE_COMMAND_RESP_CHAR_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a000a";
static constexpr const char* PHONE_LOG_STREAM_CHAR_UUID         = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a000b";
static constexpr const char* PHONE_DASHBOARD_STREAM_CHAR_UUID   = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a000c";
static constexpr const char* PHONE_NOTIFICATION_CHAR_UUID       = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a000d";

// ─────────────────────────────────────────────────────────────────────
// Phone command/control channel (slice #2)
// Phone notifies a PhoneCommandRequestV1 on PHONE_COMMAND_REQ_CHAR_UUID.
// Device handles it and writes a PhoneCommandResponseV1 on
// PHONE_COMMAND_RESP_CHAR_UUID.  Both directions are encrypted on
// PHONE_SECURE_CHANNEL_COMMAND.
// ─────────────────────────────────────────────────────────────────────

static constexpr uint8_t PHONE_COMMAND_VERSION = 1;

// Header sizes (the payload follows the header immediately, byte-packed).
static constexpr size_t PHONE_COMMAND_REQ_HEADER_SIZE  = 4;
static constexpr size_t PHONE_COMMAND_RESP_HEADER_SIZE = 8;

// Maximum payload bytes (post-decrypt) on either direction.  Sized so the
// encrypted envelope still fits inside a single 244-byte BLE notification
// after PHONE_SECURE_ENVELOPE_OVERHEAD (22 bytes).
static constexpr size_t PHONE_COMMAND_PAYLOAD_MAX = 192;

static constexpr size_t PHONE_COMMAND_REQ_FRAME_MAX  =
    PHONE_COMMAND_REQ_HEADER_SIZE  + PHONE_COMMAND_PAYLOAD_MAX;
static constexpr size_t PHONE_COMMAND_RESP_FRAME_MAX =
    PHONE_COMMAND_RESP_HEADER_SIZE + PHONE_COMMAND_PAYLOAD_MAX;

struct __attribute__((packed)) PhoneCommandRequestV1 {
    uint8_t  version;     // = PHONE_COMMAND_VERSION
    uint8_t  opcode;      // CMD_OP_*
    uint16_t requestId;   // phone-allocated, echoed in response
    // payload[] follows
};

struct __attribute__((packed)) PhoneCommandResponseV1 {
    uint8_t  version;     // = PHONE_COMMAND_VERSION
    uint8_t  opcode;      // echoed from request
    uint16_t requestId;   // echoed from request
    uint8_t  status;      // CMD_STATUS_*
    uint8_t  reserved;
    uint16_t payloadLen;  // bytes that follow this header
    // payload[] follows
};

static_assert(sizeof(PhoneCommandRequestV1)  == PHONE_COMMAND_REQ_HEADER_SIZE);
static_assert(sizeof(PhoneCommandResponseV1) == PHONE_COMMAND_RESP_HEADER_SIZE);

// Opcodes (slice #2 — read-only).
static constexpr uint8_t CMD_OP_GET_STATUS     = 0x01;
static constexpr uint8_t CMD_OP_GET_HEALTH     = 0x02;
static constexpr uint8_t CMD_OP_GET_STORAGE    = 0x03;
static constexpr uint8_t CMD_OP_GET_WIO_STATUS = 0x04;
static constexpr uint8_t CMD_OP_GET_LOG_TAIL   = 0x05;
// Opcodes (slice #4 — request-driven dashboard snapshot).
static constexpr uint8_t CMD_OP_GET_DASHBOARD  = 0x06;

// Opcodes (slice #3 — log streaming lease).
static constexpr uint8_t CMD_OP_START_LOG_STREAM = 0x10;
static constexpr uint8_t CMD_OP_STOP_LOG_STREAM  = 0x11;

// Opcodes (slice #4 — dashboard streaming lease).
static constexpr uint8_t CMD_OP_START_DASHBOARD_STREAM = 0x12;
static constexpr uint8_t CMD_OP_STOP_DASHBOARD_STREAM  = 0x13;

// Opcodes (slice #5 — safe write commands, non-destructive).
static constexpr uint8_t CMD_OP_ENRICH_NOW       = 0x20;
static constexpr uint8_t CMD_OP_UPLOAD_NOW       = 0x21;
static constexpr uint8_t CMD_OP_TAG_SESSION      = 0x22;
static constexpr uint8_t CMD_OP_SAVE_LOCATION    = 0x23;
static constexpr uint8_t CMD_OP_SCREEN_CHANGE    = 0x24;
static constexpr uint8_t CMD_OP_DEBRIEF_REQUEST  = 0x25;

// CMD_OP_TAG_SESSION / CMD_OP_SAVE_LOCATION request payload: 1-byte length
// + UTF-8 bytes (no null terminator on the wire).  Tag truncated to 31 chars
// before storage.
static constexpr size_t CMD_TAG_PAYLOAD_MAX_LEN = 31;

struct __attribute__((packed)) CmdTagPayloadHeaderV1 {
    uint8_t tagLen;       // 0..CMD_TAG_PAYLOAD_MAX_LEN; bytes follow
};

struct __attribute__((packed)) CmdScreenChangeRequestV1 {
    uint8_t targetScreen; // Screen enum value
};

// Response status codes.
static constexpr uint8_t CMD_STATUS_OK              = 0x00;
static constexpr uint8_t CMD_STATUS_UNKNOWN_OP      = 0x01;
static constexpr uint8_t CMD_STATUS_BAD_PAYLOAD     = 0x02;
static constexpr uint8_t CMD_STATUS_PAYLOAD_TOO_BIG = 0x03;
static constexpr uint8_t CMD_STATUS_NOT_READY       = 0x04;
static constexpr uint8_t CMD_STATUS_INTERNAL_ERROR  = 0x05;

// CMD_OP_GET_STATUS response payload.
struct __attribute__((packed)) CmdStatusResponseV1 {
    uint32_t uptimeMs;
    uint8_t  missionProfile;
    uint8_t  screenEnum;
    uint8_t  radioOwner;
    uint8_t  transportKind;     // matches PhoneTransportKind
    uint32_t bootMs;            // device-local boot timestamp for sanity
};

// CMD_OP_GET_HEALTH response payload.
struct __attribute__((packed)) CmdHealthResponseV1 {
    uint8_t  batteryPct;
    uint8_t  charging;          // 0/1
    uint16_t batteryMv;
    uint32_t freeHeap;
    uint32_t minFreeHeap;
    uint32_t uptimeMs;
};

// CMD_OP_GET_STORAGE response payload reuses PhoneStorageFrameV1 verbatim —
// same shape as the notification, just request-driven instead of pushed.

// CMD_OP_GET_WIO_STATUS response payload.
struct __attribute__((packed)) CmdWioStatusResponseV1 {
    uint8_t  transportKind;     // active transport (matches PhoneTransportKind)
    uint8_t  previousKind;
    uint16_t reserved;
    uint32_t lastChangeAgeMs;   // ms since the last transport switch
    uint32_t transitions;       // lifetime transport-change count
    uint32_t wioLastSeenAgeMs;  // ms since WIO accessory last sent a line
    int8_t   phoneRssi;         // last seen phone RSSI as reported by WIO
    uint8_t  phoneConnected;    // 0/1 (WIO's view of the phone)
    uint8_t  bleProxy;          // 1 = WIO has BLE_PROXY capability
    uint8_t  sx1262Present;     // 1 = WIO reports SX1262 onboard
};

// CMD_OP_GET_LOG_TAIL response payload — header followed by `lineCount`
// null-terminated UTF-8 strings packed back-to-back into `payload[]`.
// Total payload bytes (including header) never exceed PHONE_COMMAND_PAYLOAD_MAX.
struct __attribute__((packed)) CmdLogTailResponseV1Header {
    uint16_t lineCount;
    uint16_t totalBytes;        // bytes in the packed line buffer that follows
};

// CMD_OP_GET_DASHBOARD response payload — single aggregated read of the
// fields a phone-side dashboard would want.  Subset / complement of STATUS:
// session counters, wifi/lora/upload activity, drone summary.  Battery and
// storage are queried separately via GET_HEALTH and GET_STORAGE so each
// frame stays narrow.
struct __attribute__((packed)) CmdDashboardSnapshotV1 {
    uint32_t uptimeMs;
    uint8_t  missionProfile;
    uint8_t  screenEnum;
    uint8_t  radioOwner;
    uint8_t  transportKind;     // PhoneTransportKind

    // Companion link state.
    uint8_t  companionEnabled;
    uint8_t  companionPhone;    // 0 unknown / 1 available / 2 unavailable
    uint8_t  companionWork;     // 0 idle / 1 probing / 2 enriching
    uint8_t  bleConnected;

    // WiFi.
    uint8_t  wifiConnected;
    uint8_t  reserved1;
    uint16_t wifiNetworkCount;  // APs in last scan
    uint32_t probePacketCount;
    uint32_t pmkidCaptured;

    // LoRa / SubGhz.
    uint8_t  loraReady;
    uint8_t  reserved2;
    uint16_t subGhzNodeCount;
    int16_t  loraRssi;
    int16_t  loraSnr;
    uint32_t loraPacketCount;

    // Upload pipeline.
    uint8_t  uploadActive;
    uint8_t  reserved3;
    uint16_t uploadPercentBp;   // 0..10000 (basis points; *100 of percent)
    uint32_t uploadPublished;
    uint32_t uploadTotal;

    // Session-scope counters.
    uint32_t sessionNetworks;
    uint32_t sessionDevices;
    uint32_t sessionProbes;
    uint32_t sessionPMKIDs;
    uint32_t sessionDrones;

    // Drone summary.
    uint16_t droneCount;
    uint8_t  droneAlert;
    uint8_t  reserved4;
};

// ─────────────────────────────────────────────────────────────────────
// Log stream channel (slice #3)
// Phone starts a bounded lease via CMD_OP_START_LOG_STREAM; device writes
// chunks to PHONE_LOG_STREAM_CHAR_UUID (encrypted on the LOG_STREAM channel).
// Lease auto-expires; phone may extend by re-issuing START, or end early via
// CMD_OP_STOP_LOG_STREAM.  Lossy on purpose — capture health beats fidelity.
// ─────────────────────────────────────────────────────────────────────

// Lease bounds (device clamps the requested duration into this range).
static constexpr uint32_t PHONE_LOG_STREAM_LEASE_MIN_MS = 10000UL;
static constexpr uint32_t PHONE_LOG_STREAM_LEASE_MAX_MS = 300000UL;

// CMD_OP_START_LOG_STREAM request payload.
struct __attribute__((packed)) CmdStartLogStreamRequestV1 {
    uint32_t leaseDurationMs;   // clamped to [MIN,MAX] by the device
    uint16_t lineCap;           // 0 = device default; otherwise max lines/chunk
};

// CMD_OP_START_LOG_STREAM response payload.
struct __attribute__((packed)) CmdStartLogStreamResponseV1 {
    uint8_t  streamId;          // non-zero on success; echoed on STOP and chunks
    uint8_t  reserved;
    uint16_t grantedLineCap;
    uint32_t grantedDurationMs; // actual (post-clamp) lease ms
};

// CMD_OP_STOP_LOG_STREAM request payload.
struct __attribute__((packed)) CmdStopLogStreamRequestV1 {
    uint8_t streamId;
    uint8_t reserved;
};

// Chunk frame written by the device on PHONE_LOG_STREAM_CHAR_UUID.  Header
// followed by `lineCount` null-terminated UTF-8 strings (same packing as
// CmdLogTailResponseV1 so the phone reuses one decoder).
struct __attribute__((packed)) LogStreamChunkV1Header {
    uint8_t  version;           // = 1
    uint8_t  streamId;
    uint16_t seq;               // monotonic, starts at 1
    uint16_t dropped;           // lines lost since last chunk (this chunk's gap)
    uint16_t lineCount;
    uint16_t totalBytes;        // bytes in the packed line buffer
    uint8_t  flags;             // bit 0 = stream ending after this chunk
    uint8_t  reserved;
};

static constexpr uint8_t LOG_STREAM_CHUNK_VERSION = 1;
static constexpr uint8_t LOG_STREAM_FLAG_END = 0x01;  // final chunk of a lease

// Per-chunk envelope must fit in one BLE notification after secure overhead.
static constexpr size_t LOG_STREAM_CHUNK_PAYLOAD_MAX = 200;
static constexpr size_t LOG_STREAM_CHUNK_FRAME_MAX =
    sizeof(LogStreamChunkV1Header) + LOG_STREAM_CHUNK_PAYLOAD_MAX;

// ─────────────────────────────────────────────────────────────────────
// Dashboard streaming channel (slice #4)
// Phone starts a bounded lease via CMD_OP_START_DASHBOARD_STREAM with an
// interval; device pushes a fresh CmdDashboardSnapshotV1 per tick to
// PHONE_DASHBOARD_STREAM_CHAR_UUID (encrypted on the DASHBOARD channel).
// Same lifecycle rules as log streaming — bounded lease, transport-flip
// auto-cancel, final END chunk.
// ─────────────────────────────────────────────────────────────────────

static constexpr uint32_t PHONE_DASHBOARD_STREAM_LEASE_MIN_MS = 10000UL;
static constexpr uint32_t PHONE_DASHBOARD_STREAM_LEASE_MAX_MS = 300000UL;
static constexpr uint32_t PHONE_DASHBOARD_STREAM_INTERVAL_MIN_MS = 200UL;
static constexpr uint32_t PHONE_DASHBOARD_STREAM_INTERVAL_MAX_MS = 60000UL;
static constexpr uint32_t PHONE_DASHBOARD_STREAM_INTERVAL_DEFAULT_MS = 1000UL;

struct __attribute__((packed)) CmdStartDashboardStreamRequestV1 {
    uint32_t leaseDurationMs;
    uint32_t intervalMs;        // 0 → device default
};

struct __attribute__((packed)) CmdStartDashboardStreamResponseV1 {
    uint8_t  streamId;
    uint8_t  reserved;
    uint16_t reserved2;
    uint32_t grantedIntervalMs;
    uint32_t grantedDurationMs;
};

struct __attribute__((packed)) CmdStopDashboardStreamRequestV1 {
    uint8_t streamId;
    uint8_t reserved;
};

// Chunk frame written by the device on PHONE_DASHBOARD_STREAM_CHAR_UUID.
// Header followed by exactly one CmdDashboardSnapshotV1 payload.
struct __attribute__((packed)) DashboardStreamChunkV1Header {
    uint8_t  version;
    uint8_t  streamId;
    uint16_t seq;
    uint8_t  flags;             // bit 0 = stream ending after this chunk
    uint8_t  reserved;
};

static constexpr uint8_t DASHBOARD_STREAM_CHUNK_VERSION = 1;
static constexpr uint8_t DASHBOARD_STREAM_FLAG_END = 0x01;

static constexpr size_t DASHBOARD_STREAM_CHUNK_FRAME_MAX =
    sizeof(DashboardStreamChunkV1Header) + 72;  // CmdDashboardSnapshotV1

// ─────────────────────────────────────────────────────────────────────
// Throttled phone notifications (slice #7)
// Device pushes "meaningful event" notifications (PMKID, handshake, drone,
// battery low, storage warning, etc.) to PHONE_NOTIFICATION_CHAR_UUID,
// encrypted on PHONE_SECURE_CHANNEL_NOTIFICATION.  The device throttles per
// type and collapses duplicates within a window — phone sees one
// notification per unique event, with collapsedCount > 0 when the device
// folded repeats during the throttle window.
// ─────────────────────────────────────────────────────────────────────

// Internal notification type IDs (mirrors src/core/NotifTypes.h).
static constexpr uint8_t PHONE_NOTIF_TYPE_DRONE        = 1;
static constexpr uint8_t PHONE_NOTIF_TYPE_PMKID        = 2;
static constexpr uint8_t PHONE_NOTIF_TYPE_DEAUTH       = 3;
static constexpr uint8_t PHONE_NOTIF_TYPE_HANDSHAKE    = 4;
static constexpr uint8_t PHONE_NOTIF_TYPE_DEVICE_NEW   = 5;
static constexpr uint8_t PHONE_NOTIF_TYPE_HOMELAB_SYNC = 6;
static constexpr uint8_t PHONE_NOTIF_TYPE_EXPORT       = 7;
static constexpr uint8_t PHONE_NOTIF_TYPE_STORAGE      = 8;
static constexpr uint8_t PHONE_NOTIF_TYPE_POWER        = 9;

static constexpr uint8_t PHONE_NOTIF_SEVERITY_INFO     = 0;
static constexpr uint8_t PHONE_NOTIF_SEVERITY_WARN     = 1;
static constexpr uint8_t PHONE_NOTIF_SEVERITY_CRITICAL = 2;

static constexpr uint8_t PHONE_NOTIF_FLAG_COLLAPSED    = 0x01;

static constexpr size_t PHONE_NOTIF_TEXT_MAX = 96;

struct __attribute__((packed)) PhoneNotificationV1Header {
    uint8_t  version;        // = 1
    uint8_t  type;           // PHONE_NOTIF_TYPE_*
    uint8_t  severity;       // PHONE_NOTIF_SEVERITY_*
    uint8_t  flags;          // PHONE_NOTIF_FLAG_*
    uint16_t seq;            // monotonic, starts at 1
    uint16_t collapsedCount; // duplicates folded during the throttle window
    uint32_t deviceUptimeMs; // device millis() at emit time
    uint8_t  textLen;        // 0..PHONE_NOTIF_TEXT_MAX; bytes follow
};

static constexpr uint8_t PHONE_NOTIF_VERSION = 1;

static constexpr size_t PHONE_NOTIFICATION_FRAME_MAX =
    sizeof(PhoneNotificationV1Header) + PHONE_NOTIF_TEXT_MAX;

static constexpr const char* TEXT_SERVICE_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a1001";
static constexpr const char* TEXT_PROMPT_CHAR_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a1002";
static constexpr const char* TEXT_INPUT_CHAR_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a1003";
static constexpr const char* TEXT_RECEIPT_CHAR_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a1004";
static constexpr const char* TEXT_STATUS_CHAR_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a1005";

#endif // SPECTRE_COMPANION_PROTOCOL_H


