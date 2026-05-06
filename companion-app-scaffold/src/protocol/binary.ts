import {Buffer} from 'buffer';

import type {
  EnrichmentRecord,
  EventBatchRecord,
  PhoneControlFrameV1,
  PhoneGpsFrameV1,
} from './types';

const GPS_FRAME_SIZE = 20;
const CONTROL_FRAME_SIZE = 4;
const EVENT_BATCH_RECORD_SIZE = 10;
const ENRICHMENT_RECORD_SIZE = 47;

export const PHONE_GPS_FLAG_VALID = 0x01;
export const PHONE_GPS_FLAG_TRUSTED_TIME = 0x02;

export const PHONE_CONTROL_FLAG_WG_ACTIVE = 0x01;
export const PHONE_CONTROL_FLAG_DUMP_REQUEST = 0x02;
export const PHONE_CONTROL_FLAG_CANCEL = 0x04;
export const PHONE_CONTROL_FLAG_BATCH_RECEIVED = 0x08;

export function encodePhoneGpsFrame(frame: PhoneGpsFrameV1): string {
  const buffer = Buffer.alloc(GPS_FRAME_SIZE);
  buffer.writeUInt8(frame.version, 0);
  buffer.writeInt32LE(frame.latE7, 1);
  buffer.writeInt32LE(frame.lonE7, 5);
  buffer.writeInt32LE(frame.altCm, 9);
  buffer.writeUInt16LE(frame.accuracyDm, 13);
  buffer.writeUInt32LE(frame.epochUtc, 15);
  buffer.writeUInt8(frame.flags, 19);
  return buffer.toString('base64');
}

export function encodePhoneControlFrame(frame: PhoneControlFrameV1): string {
  const buffer = Buffer.alloc(CONTROL_FRAME_SIZE);
  buffer.writeUInt8(frame.version, 0);
  buffer.writeUInt8(frame.flags, 1);
  buffer.writeUInt16LE(frame.counter & 0xffff, 2);
  return buffer.toString('base64');
}

export function decodeEventBatchRecords(base64Value: string): EventBatchRecord[] {
  const buffer = Buffer.from(base64Value, 'base64');
  const records: EventBatchRecord[] = [];

  for (let offset = 0; offset + EVENT_BATCH_RECORD_SIZE <= buffer.length; offset += EVENT_BATCH_RECORD_SIZE) {
    records.push({
      eventId: buffer.readUInt32LE(offset),
      timestampMs: buffer.readUInt32LE(offset + 4),
      type: buffer.readUInt8(offset + 8) as EventBatchRecord['type'],
      status: buffer.readUInt8(offset + 9) as EventBatchRecord['status'],
    });
  }

  return records;
}

export function encodeEnrichmentRecords(records: EnrichmentRecord[]): string {
  const buffer = Buffer.alloc(records.length * ENRICHMENT_RECORD_SIZE);

  records.forEach((record, index) => {
    const offset = index * ENRICHMENT_RECORD_SIZE;
    buffer.writeUInt32LE(record.eventId, offset);
    buffer.writeInt32LE(record.latE7, offset + 4);
    buffer.writeInt32LE(record.lonE7, offset + 8);
    buffer.writeInt32LE(record.altCm, offset + 12);
    buffer.writeUInt16LE(record.accuracyDm, offset + 16);
    buffer.writeUInt32LE(record.epochUtc, offset + 18);
    buffer.writeUInt8(record.flags, offset + 22);

    const tagBytes = Buffer.from(record.tag, 'utf8');
    tagBytes.copy(buffer, offset + 23, 0, Math.min(tagBytes.length, 23));
    buffer.writeUInt8(0, offset + 46);
  });

  return buffer.toString('base64');
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
