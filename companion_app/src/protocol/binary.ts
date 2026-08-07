/* eslint-disable no-bitwise */

import type {
  CmdDashboardSnapshotV1,
  CmdHealthResponseV1,
  CmdLogTailResponseV1,
  CmdStartDashboardStreamRequestV1,
  CmdStartDashboardStreamResponseV1,
  CmdStartLogStreamRequestV1,
  CmdStartLogStreamResponseV1,
  CmdStatusResponseV1,
  CmdStopDashboardStreamRequestV1,
  CmdStopLogStreamRequestV1,
  CmdWioStatusResponseV1,
  DashboardStreamChunkV1,
  EnrichmentRecord,
  EventBatchRecord,
  LogStreamChunkV1,
  PhoneCommandRequestV1,
  PhoneCommandResponseV1,
  PhoneControlFrameV1,
  PhoneGpsFrameV1,
  PhoneNotificationV1,
  PhoneStorageFrameV1,
  PhoneTransportKind,
} from './types';
import {base64ToBytes, bytesToBase64, bytesToUtf8, utf8ToBytes} from './base64';
import {
  CMD_DASHBOARD_RESPONSE_SIZE,
  CMD_HEALTH_RESPONSE_SIZE,
  CMD_LOG_TAIL_RESPONSE_HEADER_SIZE,
  CMD_TAG_PAYLOAD_MAX_LEN,
  CMD_START_DASHBOARD_STREAM_REQ_SIZE,
  CMD_START_DASHBOARD_STREAM_RESP_SIZE,
  CMD_START_LOG_STREAM_REQ_SIZE,
  CMD_START_LOG_STREAM_RESP_SIZE,
  CMD_STATUS_RESPONSE_SIZE,
  CMD_STOP_DASHBOARD_STREAM_REQ_SIZE,
  CMD_STOP_LOG_STREAM_REQ_SIZE,
  CMD_WIO_STATUS_RESPONSE_SIZE,
  DASHBOARD_STREAM_CHUNK_HEADER_SIZE,
  DASHBOARD_STREAM_FLAG_END,
  PHONE_NOTIFICATION_HEADER_SIZE,
  PHONE_NOTIF_FLAG_COLLAPSED,
  ENRICHMENT_RECORD_SIZE,
  EVENT_BATCH_RECORD_SIZE,
  LOG_STREAM_CHUNK_HEADER_SIZE,
  LOG_STREAM_FLAG_END,
  PHONE_COMMAND_PAYLOAD_MAX,
  PHONE_COMMAND_REQ_HEADER_SIZE,
  PHONE_COMMAND_RESP_HEADER_SIZE,
  PHONE_COMMAND_VERSION,
  PHONE_CONTROL_FRAME_SIZE,
  PHONE_GPS_FRAME_SIZE,
  PHONE_STORAGE_FRAME_SIZE,
} from './contracts';

export const PHONE_GPS_FLAG_VALID = 0x01;
export const PHONE_GPS_FLAG_TRUSTED_TIME = 0x02;
export const PHONE_ENRICH_FLAG_TAG_PRESENT = 0x01;
export const PHONE_ENRICH_FLAG_NO_DATA = 0x02;

export const PHONE_CONTROL_FLAG_WG_ACTIVE = 0x01;
export const PHONE_CONTROL_FLAG_DUMP_REQUEST = 0x02;
export const PHONE_CONTROL_FLAG_CANCEL = 0x04;
export const PHONE_CONTROL_FLAG_BATCH_RECEIVED = 0x08;

export const PHONE_STORAGE_FLAG_VALID = 0x01;
export const PHONE_STORAGE_FLAG_UPLOAD_ACTIVE = 0x02;
export const PHONE_STORAGE_FLAG_NEARLY_FULL = 0x04;
export const PHONE_STORAGE_FLAG_FULL = 0x08;
export const PHONE_STORAGE_FLAG_OVERRUN = 0x10;

const EVENT_EPOCH_SECONDS_MIN = 1_600_000_000;

export function eventTimestampToUnixMs(timestamp: number): number | null {
  if (timestamp >= EVENT_EPOCH_SECONDS_MIN) {
    return timestamp * 1000;
  }

  return null;
}

export function encodePhoneGpsFrame(frame: PhoneGpsFrameV1): string {
  const bytes = new Uint8Array(PHONE_GPS_FRAME_SIZE);
  const view = new DataView(bytes.buffer);

  view.setUint8(0, frame.version);
  view.setInt32(1, frame.latE7, true);
  view.setInt32(5, frame.lonE7, true);
  view.setInt32(9, frame.altCm, true);
  view.setUint16(13, frame.accuracyDm, true);
  view.setUint32(15, frame.epochUtc, true);
  view.setUint8(19, frame.flags);

  return bytesToBase64(bytes);
}

export function encodePhoneControlFrame(frame: PhoneControlFrameV1): string {
  const bytes = new Uint8Array(PHONE_CONTROL_FRAME_SIZE);
  const view = new DataView(bytes.buffer);

  view.setUint8(0, frame.version);
  view.setUint8(1, frame.flags);
  view.setUint16(2, frame.counter & 0xffff, true);

  return bytesToBase64(bytes);
}

export function decodePhoneStorageFrame(
  base64Value: string,
): PhoneStorageFrameV1 {
  const bytes = base64ToBytes(base64Value);
  if (bytes.length !== PHONE_STORAGE_FRAME_SIZE) {
    throw new Error(`Storage frame must be ${PHONE_STORAGE_FRAME_SIZE} bytes`);
  }

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const flags = view.getUint8(1);

  return {
    version: view.getUint8(0),
    flags,
    storageValid: !!(flags & PHONE_STORAGE_FLAG_VALID),
    uploadActive: !!(flags & PHONE_STORAGE_FLAG_UPLOAD_ACTIVE),
    storageNearlyFull: !!(flags & PHONE_STORAGE_FLAG_NEARLY_FULL),
    storageFull: !!(flags & PHONE_STORAGE_FLAG_FULL),
    storageOverrun: !!(flags & PHONE_STORAGE_FLAG_OVERRUN),
    storageMode: view.getUint8(2),
    retentionPolicy: view.getUint8(3),
    usedPct: view.getUint16(4, true),
    freeBytes: view.getUint32(8, true),
    missionTotal: view.getUint32(12, true),
    noiseTotal: view.getUint32(16, true),
    p0Total: view.getUint32(20, true),
    p1Total: view.getUint32(24, true),
    p2Total: view.getUint32(28, true),
    p3Total: view.getUint32(32, true),
    pendingUploadMission: view.getUint32(36, true),
    pendingUploadNoise: view.getUint32(40, true),
    pendingEnrichMission: view.getUint32(44, true),
    pendingEnrichNoise: view.getUint32(48, true),
    enrichmentDeltas: view.getUint32(52, true),
    firstEventId: view.getUint32(56, true),
    lastEventId: view.getUint32(60, true),
    updatedMs: view.getUint32(64, true),
  };
}

export function decodeEventBatchRecords(base64Value: string): EventBatchRecord[] {
  const bytes = base64ToBytes(base64Value);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const records: EventBatchRecord[] = [];

  for (
    let offset = 0;
    offset + EVENT_BATCH_RECORD_SIZE <= bytes.length;
    offset += EVENT_BATCH_RECORD_SIZE
  ) {
    records.push({
      eventId: view.getUint32(offset, true),
      timestampMs: view.getUint32(offset + 4, true),
      type: view.getUint8(offset + 8) as EventBatchRecord['type'],
      status: view.getUint8(offset + 9) as EventBatchRecord['status'],
      lane: 1,
      priority: 3,
    });
  }

  return records;
}

export function encodeEventBatchRecords(records: EventBatchRecord[]): string {
  const bytes = new Uint8Array(records.length * EVENT_BATCH_RECORD_SIZE);
  const view = new DataView(bytes.buffer);

  records.forEach((record, index) => {
    const offset = index * EVENT_BATCH_RECORD_SIZE;
    view.setUint32(offset, record.eventId, true);
    view.setUint32(offset + 4, record.timestampMs, true);
    view.setUint8(offset + 8, record.type);
    view.setUint8(offset + 9, record.status);
  });

  return bytesToBase64(bytes);
}

export function encodeEnrichmentRecords(records: EnrichmentRecord[]): string {
  const bytes = new Uint8Array(records.length * ENRICHMENT_RECORD_SIZE);
  const view = new DataView(bytes.buffer);

  records.forEach((record, index) => {
    const offset = index * ENRICHMENT_RECORD_SIZE;
    view.setUint32(offset, record.eventId, true);
    view.setInt32(offset + 4, record.latE7, true);
    view.setInt32(offset + 8, record.lonE7, true);
    view.setInt32(offset + 12, record.altCm, true);
    view.setUint16(offset + 16, record.accuracyDm, true);
    view.setUint32(offset + 18, record.epochUtc, true);
    view.setUint8(offset + 22, record.flags);

    const tagBytes = utf8ToBytes(record.tag);
    const tagLength = Math.min(tagBytes.length, 23);
    bytes.set(tagBytes.subarray(0, tagLength), offset + 23);
    bytes[offset + 46] = 0;
  });

  return bytesToBase64(bytes);
}

export function parseStatusBlob(blob: string): Record<string, string> {
  return blob
    .split(';')
    .map(part => part.trim())
    .filter(Boolean)
    .reduce<Record<string, string>>((acc, part) => {
      const [key, value] = part.split('=');
      if (key) {
        acc[key] = value ?? '';
      }
      return acc;
    }, {});
}

export function isoFromEpoch(epochUtc: number): string {
  return new Date(epochUtc * 1000).toISOString();
}

// ── Phone command/control codecs (slice #2) ─────────────────────────────────

function clampU8(value: number) {
  return value & 0xff;
}

function clampU16(value: number) {
  return value & 0xffff;
}

function asTransportKind(raw: number): PhoneTransportKind {
  if (raw === 1) return 1;
  if (raw === 2) return 2;
  return 0;
}

export function encodePhoneCommandRequest(request: PhoneCommandRequestV1): string {
  const payload = request.payload ?? new Uint8Array(0);
  if (payload.length > PHONE_COMMAND_PAYLOAD_MAX) {
    throw new Error(
      `Command request payload exceeds ${PHONE_COMMAND_PAYLOAD_MAX} bytes (got ${payload.length}).`,
    );
  }

  const bytes = new Uint8Array(PHONE_COMMAND_REQ_HEADER_SIZE + payload.length);
  const view = new DataView(bytes.buffer);
  view.setUint8(0, clampU8(request.version ?? PHONE_COMMAND_VERSION));
  view.setUint8(1, clampU8(request.opcode));
  view.setUint16(2, clampU16(request.requestId), true);
  bytes.set(payload, PHONE_COMMAND_REQ_HEADER_SIZE);
  return bytesToBase64(bytes);
}

export function decodePhoneCommandResponse(
  base64Value: string,
): PhoneCommandResponseV1 {
  const bytes = base64ToBytes(base64Value);
  if (bytes.length < PHONE_COMMAND_RESP_HEADER_SIZE) {
    throw new Error(
      `Command response shorter than ${PHONE_COMMAND_RESP_HEADER_SIZE} byte header (got ${bytes.length}).`,
    );
  }

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const version = view.getUint8(0);
  const opcode = view.getUint8(1);
  const requestId = view.getUint16(2, true);
  const status = view.getUint8(4);
  // byte 5 is reserved
  const payloadLen = view.getUint16(6, true);

  if (PHONE_COMMAND_RESP_HEADER_SIZE + payloadLen > bytes.length) {
    throw new Error(
      `Command response truncated: header says payloadLen=${payloadLen}, total bytes=${bytes.length}.`,
    );
  }
  if (payloadLen > PHONE_COMMAND_PAYLOAD_MAX) {
    throw new Error(
      `Command response payload exceeds ${PHONE_COMMAND_PAYLOAD_MAX} bytes (got ${payloadLen}).`,
    );
  }

  const payload = bytes.slice(
    PHONE_COMMAND_RESP_HEADER_SIZE,
    PHONE_COMMAND_RESP_HEADER_SIZE + payloadLen,
  );

  return {version, opcode, requestId, status, payload};
}

export function decodeCmdStatusResponse(payload: Uint8Array): CmdStatusResponseV1 {
  if (payload.length < CMD_STATUS_RESPONSE_SIZE) {
    throw new Error(
      `CMD_GET_STATUS payload must be ${CMD_STATUS_RESPONSE_SIZE} bytes (got ${payload.length}).`,
    );
  }
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  return {
    uptimeMs: view.getUint32(0, true),
    missionProfile: view.getUint8(4),
    screenEnum: view.getUint8(5),
    radioOwner: view.getUint8(6),
    transportKind: asTransportKind(view.getUint8(7)),
    bootMs: view.getUint32(8, true),
  };
}

export function decodeCmdHealthResponse(payload: Uint8Array): CmdHealthResponseV1 {
  if (payload.length < CMD_HEALTH_RESPONSE_SIZE) {
    throw new Error(
      `CMD_GET_HEALTH payload must be ${CMD_HEALTH_RESPONSE_SIZE} bytes (got ${payload.length}).`,
    );
  }
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  return {
    batteryPct: view.getUint8(0),
    charging: view.getUint8(1) !== 0,
    batteryMv: view.getUint16(2, true),
    freeHeap: view.getUint32(4, true),
    minFreeHeap: view.getUint32(8, true),
    uptimeMs: view.getUint32(12, true),
  };
}

export function decodeCmdStorageResponse(payload: Uint8Array): PhoneStorageFrameV1 {
  if (payload.length < PHONE_STORAGE_FRAME_SIZE) {
    throw new Error(
      `CMD_GET_STORAGE payload must be ${PHONE_STORAGE_FRAME_SIZE} bytes (got ${payload.length}).`,
    );
  }
  // decodePhoneStorageFrame is base64-oriented; reuse via a re-encode would be
  // wasteful, so do the inline DataView walk here mirroring its layout.
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const flags = view.getUint8(1);
  return {
    version: view.getUint8(0),
    flags,
    storageValid: !!(flags & 0x01),
    uploadActive: !!(flags & 0x02),
    storageNearlyFull: !!(flags & 0x04),
    storageFull: !!(flags & 0x08),
    storageOverrun: !!(flags & 0x10),
    storageMode: view.getUint8(2),
    retentionPolicy: view.getUint8(3),
    usedPct: view.getUint16(4, true),
    freeBytes: view.getUint32(8, true),
    missionTotal: view.getUint32(12, true),
    noiseTotal: view.getUint32(16, true),
    p0Total: view.getUint32(20, true),
    p1Total: view.getUint32(24, true),
    p2Total: view.getUint32(28, true),
    p3Total: view.getUint32(32, true),
    pendingUploadMission: view.getUint32(36, true),
    pendingUploadNoise: view.getUint32(40, true),
    pendingEnrichMission: view.getUint32(44, true),
    pendingEnrichNoise: view.getUint32(48, true),
    enrichmentDeltas: view.getUint32(52, true),
    firstEventId: view.getUint32(56, true),
    lastEventId: view.getUint32(60, true),
    updatedMs: view.getUint32(64, true),
  };
}

export function decodeCmdDashboardResponse(
  payload: Uint8Array,
): CmdDashboardSnapshotV1 {
  if (payload.length < CMD_DASHBOARD_RESPONSE_SIZE) {
    throw new Error(
      `CMD_GET_DASHBOARD payload must be ${CMD_DASHBOARD_RESPONSE_SIZE} bytes (got ${payload.length}).`,
    );
  }
  // Layout (offsets in bytes — must match CmdDashboardSnapshotV1 in
  // CompanionProtocol.h):
  //   0  uptimeMs(u32)
  //   4  missionProfile, screenEnum, radioOwner, transportKind  (4×u8)
  //   8  companionEnabled, companionPhone, companionWork, bleConnected
  //  12  wifiConnected, reserved1, wifiNetworkCount(u16), probePacketCount(u32), pmkidCaptured(u32)
  //  24  loraReady, reserved2, subGhzNodeCount(u16), loraRssi(i16), loraSnr(i16), loraPacketCount(u32)
  //  36  uploadActive, reserved3, uploadPercentBp(u16), uploadPublished(u32), uploadTotal(u32)
  //  48  sessionNetworks/Devices/Probes/PMKIDs/Drones (5×u32)
  //  68  droneCount(u16), droneAlert(u8), reserved4(u8)
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const percentBp = view.getUint16(38, true);
  return {
    uptimeMs: view.getUint32(0, true),
    missionProfile: view.getUint8(4),
    screenEnum: view.getUint8(5),
    radioOwner: view.getUint8(6),
    transportKind: asTransportKind(view.getUint8(7)),

    companionEnabled: view.getUint8(8) !== 0,
    companionPhone: view.getUint8(9),
    companionWork: view.getUint8(10),
    bleConnected: view.getUint8(11) !== 0,

    wifiConnected: view.getUint8(12) !== 0,
    wifiNetworkCount: view.getUint16(14, true),
    probePacketCount: view.getUint32(16, true),
    pmkidCaptured: view.getUint32(20, true),

    loraReady: view.getUint8(24) !== 0,
    subGhzNodeCount: view.getUint16(26, true),
    loraRssi: view.getInt16(28, true),
    loraSnr: view.getInt16(30, true),
    loraPacketCount: view.getUint32(32, true),

    uploadActive: view.getUint8(36) !== 0,
    uploadPercent: Math.round(percentBp / 100),
    uploadPublished: view.getUint32(40, true),
    uploadTotal: view.getUint32(44, true),

    sessionNetworks: view.getUint32(48, true),
    sessionDevices: view.getUint32(52, true),
    sessionProbes: view.getUint32(56, true),
    sessionPMKIDs: view.getUint32(60, true),
    sessionDrones: view.getUint32(64, true),

    droneCount: view.getUint16(68, true),
    droneAlert: view.getUint8(70) !== 0,
  };
}

export function decodeCmdWioStatusResponse(
  payload: Uint8Array,
): CmdWioStatusResponseV1 {
  if (payload.length < CMD_WIO_STATUS_RESPONSE_SIZE) {
    throw new Error(
      `CMD_GET_WIO_STATUS payload must be ${CMD_WIO_STATUS_RESPONSE_SIZE} bytes (got ${payload.length}).`,
    );
  }
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  return {
    transportKind: asTransportKind(view.getUint8(0)),
    previousKind: asTransportKind(view.getUint8(1)),
    // bytes 2-3 reserved
    lastChangeAgeMs: view.getUint32(4, true),
    transitions: view.getUint32(8, true),
    wioLastSeenAgeMs: view.getUint32(12, true),
    phoneRssi: view.getInt8(16),
    phoneConnected: view.getUint8(17) !== 0,
    bleProxy: view.getUint8(18) !== 0,
    sx1262Present: view.getUint8(19) !== 0,
  };
}

export function decodeCmdLogTailResponse(payload: Uint8Array): CmdLogTailResponseV1 {
  if (payload.length < CMD_LOG_TAIL_RESPONSE_HEADER_SIZE) {
    throw new Error(
      `CMD_GET_LOG_TAIL payload must be at least ${CMD_LOG_TAIL_RESPONSE_HEADER_SIZE} bytes (got ${payload.length}).`,
    );
  }
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const lineCount = view.getUint16(0, true);
  const totalBytes = view.getUint16(2, true);

  if (CMD_LOG_TAIL_RESPONSE_HEADER_SIZE + totalBytes > payload.length) {
    throw new Error(
      `CMD_GET_LOG_TAIL truncated: header says totalBytes=${totalBytes} but payload is ${payload.length}.`,
    );
  }

  const linesBuf = payload.subarray(
    CMD_LOG_TAIL_RESPONSE_HEADER_SIZE,
    CMD_LOG_TAIL_RESPONSE_HEADER_SIZE + totalBytes,
  );
  const lines: string[] = [];
  let cursor = 0;
  while (cursor < linesBuf.length && lines.length < lineCount) {
    let end = cursor;
    while (end < linesBuf.length && linesBuf[end] !== 0) {
      end++;
    }
    lines.push(bytesToUtf8(linesBuf.subarray(cursor, end)));
    cursor = end + 1; // skip the null
  }

  return {lineCount, totalBytes, lines};
}

// ── Slice #3 — log stream codecs ────────────────────────────────────────────

function unpackNullTerminatedLines(buf: Uint8Array, maxLines: number): string[] {
  const lines: string[] = [];
  let cursor = 0;
  while (cursor < buf.length && lines.length < maxLines) {
    let end = cursor;
    while (end < buf.length && buf[end] !== 0) {
      end++;
    }
    lines.push(bytesToUtf8(buf.subarray(cursor, end)));
    cursor = end + 1;
  }
  return lines;
}

export function encodeCmdStartLogStreamRequest(
  request: CmdStartLogStreamRequestV1,
): Uint8Array {
  const bytes = new Uint8Array(CMD_START_LOG_STREAM_REQ_SIZE);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, request.leaseDurationMs >>> 0, true);
  view.setUint16(4, request.lineCap & 0xffff, true);
  return bytes;
}

export function decodeCmdStartLogStreamResponse(
  payload: Uint8Array,
): CmdStartLogStreamResponseV1 {
  if (payload.length < CMD_START_LOG_STREAM_RESP_SIZE) {
    throw new Error(
      `CMD_START_LOG_STREAM response must be ${CMD_START_LOG_STREAM_RESP_SIZE} bytes (got ${payload.length}).`,
    );
  }
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  return {
    streamId: view.getUint8(0),
    // byte 1 reserved
    grantedLineCap: view.getUint16(2, true),
    grantedDurationMs: view.getUint32(4, true),
  };
}

export function encodeCmdStopLogStreamRequest(
  request: CmdStopLogStreamRequestV1,
): Uint8Array {
  const bytes = new Uint8Array(CMD_STOP_LOG_STREAM_REQ_SIZE);
  bytes[0] = request.streamId & 0xff;
  // byte 1 reserved
  return bytes;
}

export function encodeCmdStartDashboardStreamRequest(
  request: CmdStartDashboardStreamRequestV1,
): Uint8Array {
  const bytes = new Uint8Array(CMD_START_DASHBOARD_STREAM_REQ_SIZE);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, request.leaseDurationMs >>> 0, true);
  view.setUint32(4, request.intervalMs >>> 0, true);
  return bytes;
}

export function decodeCmdStartDashboardStreamResponse(
  payload: Uint8Array,
): CmdStartDashboardStreamResponseV1 {
  if (payload.length < CMD_START_DASHBOARD_STREAM_RESP_SIZE) {
    throw new Error(
      `CMD_START_DASHBOARD_STREAM response must be ${CMD_START_DASHBOARD_STREAM_RESP_SIZE} bytes (got ${payload.length}).`,
    );
  }
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  return {
    streamId: view.getUint8(0),
    // bytes 1-3 reserved
    grantedIntervalMs: view.getUint32(4, true),
    grantedDurationMs: view.getUint32(8, true),
  };
}

export function encodeCmdStopDashboardStreamRequest(
  request: CmdStopDashboardStreamRequestV1,
): Uint8Array {
  const bytes = new Uint8Array(CMD_STOP_DASHBOARD_STREAM_REQ_SIZE);
  bytes[0] = request.streamId & 0xff;
  return bytes;
}

export function decodeDashboardStreamChunk(base64Value: string): DashboardStreamChunkV1 {
  const bytes = base64ToBytes(base64Value);
  if (bytes.length < DASHBOARD_STREAM_CHUNK_HEADER_SIZE + CMD_DASHBOARD_RESPONSE_SIZE) {
    throw new Error(
      `Dashboard stream chunk too short (got ${bytes.length}).`,
    );
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const version = view.getUint8(0);
  const streamId = view.getUint8(1);
  const seq = view.getUint16(2, true);
  const flags = view.getUint8(4);
  // byte 5 reserved

  const snapshot = decodeCmdDashboardResponse(
    bytes.subarray(
      DASHBOARD_STREAM_CHUNK_HEADER_SIZE,
      DASHBOARD_STREAM_CHUNK_HEADER_SIZE + CMD_DASHBOARD_RESPONSE_SIZE,
    ),
  );

  return {
    version,
    streamId,
    seq,
    ending: (flags & DASHBOARD_STREAM_FLAG_END) !== 0,
    snapshot,
  };
}

// ── Slice #5 — safe write commands ──────────────────────────────────────────

export function encodeCmdTagPayload(tag: string): Uint8Array {
  const tagBytes = utf8ToBytes(tag);
  if (tagBytes.length === 0) {
    throw new Error('Tag cannot be empty.');
  }
  if (tagBytes.length > CMD_TAG_PAYLOAD_MAX_LEN) {
    throw new Error(
      `Tag too long (${tagBytes.length} bytes, max ${CMD_TAG_PAYLOAD_MAX_LEN}).`,
    );
  }
  const out = new Uint8Array(1 + tagBytes.length);
  out[0] = tagBytes.length;
  out.set(tagBytes, 1);
  return out;
}

export function encodeCmdScreenChangeRequest(targetScreen: number): Uint8Array {
  return new Uint8Array([targetScreen & 0xff]);
}

// ── Slice #7 — phone notifications ──────────────────────────────────────────

export function decodePhoneNotification(base64Value: string): PhoneNotificationV1 {
  const bytes = base64ToBytes(base64Value);
  if (bytes.length < PHONE_NOTIFICATION_HEADER_SIZE) {
    throw new Error(
      `Notification too short (${bytes.length} bytes, need ${PHONE_NOTIFICATION_HEADER_SIZE}).`,
    );
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const version = view.getUint8(0);
  const type = view.getUint8(1);
  const severity = view.getUint8(2);
  const flags = view.getUint8(3);
  const seq = view.getUint16(4, true);
  const collapsedCount = view.getUint16(6, true);
  const deviceUptimeMs = view.getUint32(8, true);
  const textLen = view.getUint8(12);

  if (PHONE_NOTIFICATION_HEADER_SIZE + textLen > bytes.length) {
    throw new Error(
      `Notification truncated: header says textLen=${textLen} but frame is ${bytes.length}.`,
    );
  }

  const text = textLen > 0
    ? bytesToUtf8(bytes.subarray(
        PHONE_NOTIFICATION_HEADER_SIZE,
        PHONE_NOTIFICATION_HEADER_SIZE + textLen,
      ))
    : '';

  return {
    version,
    type,
    severity,
    collapsed: (flags & PHONE_NOTIF_FLAG_COLLAPSED) !== 0,
    seq,
    collapsedCount,
    deviceUptimeMs,
    text,
  };
}

export function decodeLogStreamChunk(base64Value: string): LogStreamChunkV1 {
  const bytes = base64ToBytes(base64Value);
  if (bytes.length < LOG_STREAM_CHUNK_HEADER_SIZE) {
    throw new Error(
      `Log stream chunk shorter than ${LOG_STREAM_CHUNK_HEADER_SIZE} byte header (got ${bytes.length}).`,
    );
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const version = view.getUint8(0);
  const streamId = view.getUint8(1);
  const seq = view.getUint16(2, true);
  const dropped = view.getUint16(4, true);
  const lineCount = view.getUint16(6, true);
  const totalBytes = view.getUint16(8, true);
  const flags = view.getUint8(10);
  // byte 11 reserved

  if (LOG_STREAM_CHUNK_HEADER_SIZE + totalBytes > bytes.length) {
    throw new Error(
      `Log stream chunk truncated: header says totalBytes=${totalBytes} but frame is ${bytes.length}.`,
    );
  }

  const lines = unpackNullTerminatedLines(
    bytes.subarray(
      LOG_STREAM_CHUNK_HEADER_SIZE,
      LOG_STREAM_CHUNK_HEADER_SIZE + totalBytes,
    ),
    lineCount,
  );

  return {
    version,
    streamId,
    seq,
    dropped,
    ending: (flags & LOG_STREAM_FLAG_END) !== 0,
    lineCount,
    totalBytes,
    lines,
  };
}
