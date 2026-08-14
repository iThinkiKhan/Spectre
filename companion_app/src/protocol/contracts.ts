import {utf8ToBytes} from './base64';

export const PHONE_GPS_FRAME_SIZE = 20;
export const PHONE_CONTROL_FRAME_SIZE = 4;
export const PHONE_STORAGE_FRAME_SIZE = 68;
export const EVENT_BATCH_RECORD_SIZE = 10;
export const ENRICHMENT_RECORD_SIZE = 47;

// COMMAND channel (slice #2) — request/response over PHONE_SECURE_CHANNEL_COMMAND.
// Keep these in lock-step with src/protocol/CompanionProtocol.h on the device.
export const PHONE_COMMAND_VERSION = 1;
export const PHONE_COMMAND_REQ_HEADER_SIZE = 4;
export const PHONE_COMMAND_RESP_HEADER_SIZE = 8;
// Matches the native S3 single-write ceiling; larger values require explicit
// application-level fragmentation rather than an ATT long write.
export const PHONE_COMMAND_PAYLOAD_MAX = 192;
export const PHONE_COMMAND_REQ_FRAME_MAX =
  PHONE_COMMAND_REQ_HEADER_SIZE + PHONE_COMMAND_PAYLOAD_MAX;
export const PHONE_COMMAND_RESP_FRAME_MAX =
  PHONE_COMMAND_RESP_HEADER_SIZE + PHONE_COMMAND_PAYLOAD_MAX;

export const PHONE_SECURE_CHANNEL_COMMAND = 0x07;

// Opcodes (read-only set for slice #2).
export const CMD_OP_GET_STATUS = 0x01;
export const CMD_OP_GET_HEALTH = 0x02;
export const CMD_OP_GET_STORAGE = 0x03;
export const CMD_OP_GET_WIO_STATUS = 0x04;
export const CMD_OP_GET_LOG_TAIL = 0x05;
// Opcode (slice #4 — request-driven dashboard snapshot).
export const CMD_OP_GET_DASHBOARD = 0x06;

// Opcodes (slice #3 — log streaming lease).
export const CMD_OP_START_LOG_STREAM = 0x10;
export const CMD_OP_STOP_LOG_STREAM = 0x11;
// Opcodes (slice #4 — dashboard streaming lease).
export const CMD_OP_START_DASHBOARD_STREAM = 0x12;
export const CMD_OP_STOP_DASHBOARD_STREAM = 0x13;

// Opcodes (slice #5 — safe write commands, non-destructive).
export const CMD_OP_ENRICH_NOW      = 0x20;
export const CMD_OP_UPLOAD_NOW      = 0x21;
export const CMD_OP_TAG_SESSION     = 0x22;
export const CMD_OP_SAVE_LOCATION   = 0x23;
export const CMD_OP_SCREEN_CHANGE   = 0x24;
export const CMD_OP_DEBRIEF_REQUEST = 0x25;

// Slice #8 — authenticated, durable phone offload.
export const CMD_OP_OFFLOAD_BEGIN = 0x30;
export const CMD_OP_OFFLOAD_NEXT = 0x31;
export const CMD_OP_OFFLOAD_ACK = 0x32;
export const CMD_OP_OFFLOAD_END = 0x33;
export const CMD_OP_WIFI_OFFLOAD_BEGIN = 0x34;
export const PHONE_OFFLOAD_VERSION = 1;
export const PHONE_OFFLOAD_FLAG_END = 0x01;
export const PHONE_OFFLOAD_FLAG_RECORD = 0x02;
export const PHONE_OFFLOAD_FLAG_INDEX_TRUNCATED = 0x04;
export const PHONE_OFFLOAD_BEGIN_RESPONSE_SIZE = 12;
export const PHONE_OFFLOAD_NEXT_REQUEST_SIZE = 8;
export const PHONE_OFFLOAD_CHUNK_HEADER_SIZE = 16;
export const PHONE_OFFLOAD_ACK_REQUEST_SIZE = 8;
export const PHONE_OFFLOAD_ACK_RESPONSE_SIZE = 4;

export const CMD_TAG_PAYLOAD_MAX_LEN = 31;

// Response status codes.
export const CMD_STATUS_OK = 0x00;
export const CMD_STATUS_UNKNOWN_OP = 0x01;
export const CMD_STATUS_BAD_PAYLOAD = 0x02;
export const CMD_STATUS_PAYLOAD_TOO_BIG = 0x03;
export const CMD_STATUS_NOT_READY = 0x04;
export const CMD_STATUS_INTERNAL_ERROR = 0x05;

// Sizes of each opcode response payload struct.  Kept here so the codec layer
// can validate at decode time without each call site duplicating the constant.
export const CMD_STATUS_RESPONSE_SIZE = 12;
export const CMD_HEALTH_RESPONSE_SIZE = 16;
export const CMD_WIO_STATUS_RESPONSE_SIZE = 20;
export const CMD_LOG_TAIL_RESPONSE_HEADER_SIZE = 4;
export const CMD_DASHBOARD_RESPONSE_SIZE = 72;

// Slice #3 — log streaming lease (request/response payload sizes).
export const CMD_START_LOG_STREAM_REQ_SIZE = 6;   // u32 leaseMs + u16 lineCap
export const CMD_START_LOG_STREAM_RESP_SIZE = 8;  // u8 streamId + u8 reserved + u16 grantedCap + u32 grantedMs
export const CMD_STOP_LOG_STREAM_REQ_SIZE = 2;    // u8 streamId + u8 reserved

// LogStreamChunkV1 header layout — version(1) streamId(1) seq(2) dropped(2)
// lineCount(2) totalBytes(2) flags(1) reserved(1) = 12 bytes.
export const LOG_STREAM_CHUNK_HEADER_SIZE = 12;
export const LOG_STREAM_CHUNK_VERSION = 1;
export const LOG_STREAM_FLAG_END = 0x01;

export const PHONE_SECURE_CHANNEL_LOG_STREAM = 0x08;

export const PHONE_LOG_STREAM_LEASE_MIN_MS = 10_000;
export const PHONE_LOG_STREAM_LEASE_MAX_MS = 300_000;
export const PHONE_LOG_STREAM_DEFAULT_LEASE_MS = 60_000;

// Slice #4 — dashboard streaming lease.
export const PHONE_SECURE_CHANNEL_DASHBOARD_STREAM = 0x09;
export const PHONE_DASHBOARD_STREAM_LEASE_MIN_MS = 10_000;
export const PHONE_DASHBOARD_STREAM_LEASE_MAX_MS = 300_000;
export const PHONE_DASHBOARD_STREAM_DEFAULT_LEASE_MS = 60_000;
export const PHONE_DASHBOARD_STREAM_INTERVAL_MIN_MS = 200;
export const PHONE_DASHBOARD_STREAM_INTERVAL_MAX_MS = 60_000;
export const PHONE_DASHBOARD_STREAM_INTERVAL_DEFAULT_MS = 1_000;

export const CMD_START_DASHBOARD_STREAM_REQ_SIZE = 8;   // u32 leaseMs + u32 intervalMs
export const CMD_START_DASHBOARD_STREAM_RESP_SIZE = 12; // streamId+reserved+reserved2(2) + grantedIntervalMs + grantedDurationMs
export const CMD_STOP_DASHBOARD_STREAM_REQ_SIZE = 2;

// DashboardStreamChunkV1 header: version(1) streamId(1) seq(2) flags(1) reserved(1) = 6 bytes.
export const DASHBOARD_STREAM_CHUNK_HEADER_SIZE = 6;
export const DASHBOARD_STREAM_CHUNK_VERSION = 1;
export const DASHBOARD_STREAM_FLAG_END = 0x01;

// Slice #7 — throttled notifications.
export const PHONE_SECURE_CHANNEL_NOTIFICATION = 0x0a;

// PhoneNotificationV1Header: version(1) type(1) severity(1) flags(1) seq(2)
//   collapsedCount(2) deviceUptimeMs(4) textLen(1) = 13 bytes.
export const PHONE_NOTIFICATION_HEADER_SIZE = 13;
export const PHONE_NOTIFICATION_VERSION = 1;
export const PHONE_NOTIF_TEXT_MAX = 96;
export const PHONE_NOTIF_FLAG_COLLAPSED = 0x01;

// Type IDs mirror src/core/NotifTypes.h.
export const PHONE_NOTIF_TYPE_DRONE        = 1;
export const PHONE_NOTIF_TYPE_PMKID        = 2;
export const PHONE_NOTIF_TYPE_DEAUTH       = 3;
export const PHONE_NOTIF_TYPE_HANDSHAKE    = 4;
export const PHONE_NOTIF_TYPE_DEVICE_NEW   = 5;
export const PHONE_NOTIF_TYPE_HOMELAB_SYNC = 6;
export const PHONE_NOTIF_TYPE_EXPORT       = 7;
export const PHONE_NOTIF_TYPE_STORAGE      = 8;
export const PHONE_NOTIF_TYPE_POWER        = 9;

export const PHONE_NOTIF_SEVERITY_INFO     = 0;
export const PHONE_NOTIF_SEVERITY_WARN     = 1;
export const PHONE_NOTIF_SEVERITY_CRITICAL = 2;

export function notificationTypeName(type: number): string {
  switch (type) {
    case PHONE_NOTIF_TYPE_DRONE:        return 'drone';
    case PHONE_NOTIF_TYPE_PMKID:        return 'pmkid';
    case PHONE_NOTIF_TYPE_DEAUTH:       return 'deauth';
    case PHONE_NOTIF_TYPE_HANDSHAKE:    return 'handshake';
    case PHONE_NOTIF_TYPE_DEVICE_NEW:   return 'device';
    case PHONE_NOTIF_TYPE_HOMELAB_SYNC: return 'sync';
    case PHONE_NOTIF_TYPE_EXPORT:       return 'export';
    case PHONE_NOTIF_TYPE_STORAGE:      return 'storage';
    case PHONE_NOTIF_TYPE_POWER:        return 'power';
    default:                            return `type-${type}`;
  }
}
export const BADUSB_UPLOAD_PROTOCOL_VERSION = 1;
export const BADUSB_UPLOAD_COMMAND_MAX_BYTES = 192;
export const BADUSB_UPLOAD_STATUS_MAX_BYTES = 120;
export const BADUSB_UPLOAD_HEADER_BYTES = 9;
export const BADUSB_UPLOAD_CHUNK_DATA_BYTES = 128;
export const BADUSB_UPLOAD_MAX_SCRIPT_BYTES = 8192;
export const BADUSB_FILE_MAX_BYTES = 39;
export const BADUSB_NAME_MAX_BYTES = 31;
export const BADUSB_DESC_MAX_BYTES = 47;

export const PHONE_EVENT_BATCH_MAX_RECORDS = 512;
export const PHONE_EVENT_BATCH_MAX_BYTES =
  PHONE_EVENT_BATCH_MAX_RECORDS * EVENT_BATCH_RECORD_SIZE;

export const TEXT_PROMPT_MAX_BYTES = 23;
export const TEXT_INPUT_MAX_BYTES = 63;
export const ENRICHMENT_TAG_MAX_BYTES = 23;
export const PHONE_METADATA_MAX_BYTES = 23;

const ASCII_CONTROL_BYTE_MAX = 0x1f;

const RETRYABLE_RECEIPTS = new Set(['BUSY', 'REJECTED', 'TIMEOUT', 'CANCELLED']);
const COMPLETED_RECEIPTS = new Set(['RECEIVED', 'CONSUMED']);

export type PromptKind = 'sessionTag' | 'saveLocation' | 'generic' | 'none';
export type PromptReplyValidation =
  | {
      ok: true;
      value: string;
      byteLength: number;
    }
  | {
      ok: false;
      reason: string;
    };

export type EventBatchPayloadValidation =
  | {
      ok: true;
      recordCount: number;
    }
  | {
      ok: false;
      reason: string;
    };

function trimTrailingLineBreaks(value: string) {
  return value.replace(/[\r\n]+$/g, '');
}

export function classifyPromptKind(promptText?: string | null): PromptKind {
  const normalized = promptText?.trim().toLowerCase() ?? '';
  if (!normalized) {
    return 'none';
  }
  if (normalized.startsWith('tag this session:')) {
    return 'sessionTag';
  }
  if (normalized.startsWith('save location:')) {
    return 'saveLocation';
  }
  return 'generic';
}

export function isRetryableReceipt(receipt?: string | null) {
  return !!receipt && RETRYABLE_RECEIPTS.has(receipt.toUpperCase());
}

export function isCompletedReceipt(receipt?: string | null) {
  return !!receipt && COMPLETED_RECEIPTS.has(receipt.toUpperCase());
}

export function validatePromptReply(input: string): PromptReplyValidation {
  const trimmed = trimTrailingLineBreaks(input).trim();
  if (!trimmed.length) {
    return {
      ok: false,
      reason: 'Reply cannot be empty.',
    };
  }

  const bytes = utf8ToBytes(trimmed);
  if (!bytes.length) {
    return {
      ok: false,
      reason: 'Reply cannot be empty.',
    };
  }

  if (bytes.length > TEXT_INPUT_MAX_BYTES) {
    return {
      ok: false,
      reason: `Reply is too long for Spectre (${bytes.length}/${TEXT_INPUT_MAX_BYTES} bytes).`,
    };
  }

  for (const byte of bytes) {
    if (byte <= ASCII_CONTROL_BYTE_MAX) {
      return {
        ok: false,
        reason: 'Reply contains control characters Spectre will reject.',
      };
    }
  }

  return {
    ok: true,
    value: trimmed,
    byteLength: bytes.length,
  };
}

export function truncateUtf8(value: string, maxBytes: number) {
  let output = '';

  for (const char of value) {
    const next = `${output}${char}`;
    if (utf8ToBytes(next).length > maxBytes) {
      break;
    }
    output = next;
  }

  return output;
}

export function normalizeEnrichmentTag(tag: string) {
  const trimmed = tag.trim();
  const normalized = truncateUtf8(trimmed, ENRICHMENT_TAG_MAX_BYTES);
  return {
    value: normalized,
    byteLength: utf8ToBytes(normalized).length,
    truncated: normalized !== trimmed,
  };
}

export function buildCompanionMetadata(options: {
  gpsMode: 'device' | 'manual' | 'off';
  autoApply: boolean;
}) {
  const gpsMode =
    options.gpsMode === 'device'
      ? 'd'
      : options.gpsMode === 'manual'
        ? 'm'
        : 'o';
  const metadata = `sp;v=1;g=${gpsMode};a=${options.autoApply ? 1 : 0}`;
  const normalized = truncateUtf8(metadata, PHONE_METADATA_MAX_BYTES);

  return {
    value: normalized,
    truncated: normalized !== metadata,
  };
}

export function validateEventBatchPayloadLength(
  length: number,
): EventBatchPayloadValidation {
  if (!Number.isFinite(length) || length <= 0) {
    return {
      ok: false,
      reason: 'Event batch payload is empty.',
    };
  }

  if (length > PHONE_EVENT_BATCH_MAX_BYTES) {
    return {
      ok: false,
      reason: `Event batch payload exceeds the phone contract (${length} bytes).`,
    };
  }

  if (length % EVENT_BATCH_RECORD_SIZE !== 0) {
    return {
      ok: false,
      reason: `Event batch payload is misaligned (${length} bytes).`,
    };
  }

  return {
    ok: true,
    recordCount: length / EVENT_BATCH_RECORD_SIZE,
  };
}
