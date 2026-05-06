import {utf8ToBytes} from './base64';

export const PHONE_GPS_FRAME_SIZE = 20;
export const PHONE_CONTROL_FRAME_SIZE = 4;
export const PHONE_STORAGE_FRAME_SIZE = 68;
export const EVENT_BATCH_RECORD_SIZE = 10;
export const ENRICHMENT_RECORD_SIZE = 47;
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
