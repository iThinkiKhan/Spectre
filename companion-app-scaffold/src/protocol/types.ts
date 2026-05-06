export type SpectreEventType = 0 | 1 | 2 | 3;
export type SpectreEventStatus = 0 | 1 | 2;

export interface PhoneGpsFrameV1 {
  version: 1;
  latE7: number;
  lonE7: number;
  altCm: number;
  accuracyDm: number;
  epochUtc: number;
  flags: number;
}

export interface PhoneControlFrameV1 {
  version: 1;
  flags: number;
  counter: number;
}

export interface EventBatchRecord {
  eventId: number;
  timestampMs: number;
  type: SpectreEventType;
  status: SpectreEventStatus;
}

export interface EnrichmentRecord {
  eventId: number;
  latE7: number;
  lonE7: number;
  altCm: number;
  accuracyDm: number;
  epochUtc: number;
  flags: number;
  tag: string;
}

export interface GpsFix {
  ts: number;
  lat: number;
  lon: number;
  alt: number | null;
  accuracy: number | null;
}

export interface KnownLocation {
  id?: number;
  tag: string;
  lat: number;
  lon: number;
  radiusM: number;
}

export interface StoredEvent {
  id?: number;
  spectreEventId: number;
  sessionId: string | null;
  ts: number;
  type: 'probe' | 'device' | 'drone' | 'pmkid' | 'event';
  payload: string;
  lat: number | null;
  lon: number | null;
  alt: number | null;
  accuracy: number | null;
  tag: string | null;
  uploaded: 0 | 1;
  uploadTs: number | null;
  phoneConfirmed: 0 | 1;
  phoneConfirmTs: number | null;
}

export interface UploadCandidate extends StoredEvent {
  uploaded: 0;
}

export interface BrokerSettings {
  host: string;
  port: number;
  username: string;
  password: string;
}

export interface CompanionStats {
  gpsSamples: number;
  pendingEvents: number;
  uploadedEvents: number;
  oldestPendingAgeMs: number | null;
}
