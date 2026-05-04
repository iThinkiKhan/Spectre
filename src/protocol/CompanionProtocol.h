
#pragma once

#include <stddef.h>
#include <stdint.h>

// Companion BLE wire contract shared by the firmware and phone app.

static constexpr uint8_t COMPANION_PROTOCOL_VERSION = 1;

static constexpr size_t PHONE_GPS_FRAME_SIZE = 20;
static constexpr size_t PHONE_CONTROL_FRAME_SIZE = 4;
static constexpr size_t EVENT_BATCH_RECORD_SIZE = 12;
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
    uint32_t timestampMs;  // offset  4
    uint8_t  type;         // offset  8
    uint8_t  status;       // offset  9
    uint8_t  lane;         // offset 10 — StorageLane (0=mission, 1=noise)
    uint8_t  priority;     // offset 11 — StoragePriority (0=P0 … 3=P3)
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

static_assert(sizeof(PhoneGpsFrameV1) == PHONE_GPS_FRAME_SIZE);
static_assert(sizeof(PhoneControlFrameV1) == PHONE_CONTROL_FRAME_SIZE);
static_assert(sizeof(EventBatchRecord) == EVENT_BATCH_RECORD_SIZE);
static_assert(sizeof(EnrichmentRecordWire) == ENRICHMENT_RECORD_SIZE);

static constexpr uint8_t PHONE_GPS_FLAG_VALID = 0x01;
static constexpr uint8_t PHONE_GPS_FLAG_TIME_TRUSTED = 0x02;

static constexpr uint8_t PHONE_CTRL_FLAG_WG_ACTIVE = 0x01;
static constexpr uint8_t PHONE_CTRL_FLAG_DUMP_REQ = 0x02;
static constexpr uint8_t PHONE_CTRL_FLAG_CANCEL = 0x04;
static constexpr uint8_t PHONE_CTRL_FLAG_BATCH_RX = 0x08;

static constexpr const char* PHONE_SERVICE_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0001";
static constexpr const char* PHONE_GPS_CHAR_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0002";
static constexpr const char* PHONE_CONTROL_CHAR_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0003";
static constexpr const char* PHONE_META_CHAR_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0004";
static constexpr const char* PHONE_EVENT_BATCH_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0005";
static constexpr const char* PHONE_ENRICHMENT_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0006";
static constexpr const char* PHONE_AUTH_CHAR_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0007";

static constexpr const char* TEXT_SERVICE_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a1001";
static constexpr const char* TEXT_PROMPT_CHAR_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a1002";
static constexpr const char* TEXT_INPUT_CHAR_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a1003";
static constexpr const char* TEXT_RECEIPT_CHAR_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a1004";
static constexpr const char* TEXT_STATUS_CHAR_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a1005";

