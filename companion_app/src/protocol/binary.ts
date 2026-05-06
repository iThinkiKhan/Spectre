/* eslint-disable no-bitwise */

import type {
  EnrichmentRecord,
  EventBatchRecord,
  PhoneControlFrameV1,
  PhoneGpsFrameV1,
  PhoneStorageFrameV1,
} from './types';
import {base64ToBytes, bytesToBase64, utf8ToBytes} from './base64';
import {
  ENRICHMENT_RECORD_SIZE,
  EVENT_BATCH_RECORD_SIZE,
  PHONE_CONTROL_FRAME_SIZE,
  PHONE_GPS_FRAME_SIZE,
  PHONE_STORAGE_FRAME_SIZE,
} from './contracts';

export const PHONE_GPS_FLAG_VALID = 0x01;
export const PHONE_GPS_FLAG_TRUSTED_TIME = 0x02;

export const PHONE_CONTROL_FLAG_WG_ACTIVE = 0x01;
export const PHONE_CONTROL_FLAG_DUMP_REQUEST = 0x02;
export const PHONE_CONTROL_FLAG_CANCEL = 0x04;
export const PHONE_CONTROL_FLAG_BATCH_RECEIVED = 0x08;

export const PHONE_STORAGE_FLAG_VALID = 0x01;
export const PHONE_STORAGE_FLAG_UPLOAD_ACTIVE = 0x02;
export const PHONE_STORAGE_FLAG_NEARLY_FULL = 0x04;
export const PHONE_STORAGE_FLAG_FULL = 0x08;
export const PHONE_STORAGE_FLAG_OVERRUN = 0x10;

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
