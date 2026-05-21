export type SpectreEventType = 0 | 1 | 2 | 3 | 4 | 5;
export type SpectreEventStatus = 0 | 1 | 2;
export type SpectreStorageLane = 0 | 1;
export type SpectreStoragePriority = 0 | 1 | 2 | 3;

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

export interface PhoneStorageFrameV1 {
  version: number;
  flags: number;
  storageValid: boolean;
  uploadActive: boolean;
  storageNearlyFull: boolean;
  storageFull: boolean;
  storageOverrun: boolean;
  storageMode: number;
  retentionPolicy: number;
  usedPct: number;
  freeBytes: number;
  missionTotal: number;
  noiseTotal: number;
  p0Total: number;
  p1Total: number;
  p2Total: number;
  p3Total: number;
  pendingUploadMission: number;
  pendingUploadNoise: number;
  pendingEnrichMission: number;
  pendingEnrichNoise: number;
  enrichmentDeltas: number;
  firstEventId: number;
  lastEventId: number;
  updatedMs: number;
}

export interface EventBatchRecord {
  eventId: number;
  // Wire field is 32-bit: epoch seconds when Spectre has trusted time,
  // otherwise legacy monotonic millis.
  timestampMs: number;
  type: SpectreEventType;
  status: SpectreEventStatus;
  lane: SpectreStorageLane;
  priority: SpectreStoragePriority;
}

export interface EnrichmentRecord {
  // eventId 0 is an explicit no-match placeholder; Spectre keeps that
  // original event pending for a later GPS-correlated enrichment pass.
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

// Phone command/control wire types — slice #2.  Mirrors
// PhoneCommandRequestV1 / PhoneCommandResponseV1 in CompanionProtocol.h.
export interface PhoneCommandRequestV1 {
  version: number;
  opcode: number;
  requestId: number;
  payload: Uint8Array; // empty for read-only opcodes
}

export interface PhoneCommandResponseV1 {
  version: number;
  opcode: number;
  requestId: number;
  status: number;
  payload: Uint8Array;
}

// Mirrors PhoneTransportKind on the device — keep ordering identical.
export type PhoneTransportKind = 0 | 1 | 2; // None | WioBle | InternalBle

export interface CmdStatusResponseV1 {
  uptimeMs: number;
  missionProfile: number;
  screenEnum: number;
  radioOwner: number;
  transportKind: PhoneTransportKind;
  bootMs: number;
}

export interface CmdHealthResponseV1 {
  batteryPct: number;
  charging: boolean;
  batteryMv: number;
  freeHeap: number;
  minFreeHeap: number;
  uptimeMs: number;
}

// CmdStorageResponseV1 reuses PhoneStorageFrameV1 — same wire shape, just
// served from a request instead of a notification.

export interface CmdDashboardSnapshotV1 {
  uptimeMs: number;
  missionProfile: number;
  screenEnum: number;
  radioOwner: number;
  transportKind: PhoneTransportKind;

  companionEnabled: boolean;
  companionPhone: number;   // 0 unknown, 1 available, 2 unavailable
  companionWork: number;    // 0 idle, 1 probing, 2 enriching
  bleConnected: boolean;

  wifiConnected: boolean;
  wifiNetworkCount: number;
  probePacketCount: number;
  pmkidCaptured: number;

  loraReady: boolean;
  subGhzNodeCount: number;
  loraRssi: number;         // signed
  loraSnr: number;          // signed
  loraPacketCount: number;

  uploadActive: boolean;
  uploadPercent: number;    // 0..100 (rounded from on-wire basis points)
  uploadPublished: number;
  uploadTotal: number;

  sessionNetworks: number;
  sessionDevices: number;
  sessionProbes: number;
  sessionPMKIDs: number;
  sessionDrones: number;

  droneCount: number;
  droneAlert: boolean;
}

export interface CmdWioStatusResponseV1 {
  transportKind: PhoneTransportKind;
  previousKind: PhoneTransportKind;
  lastChangeAgeMs: number;
  transitions: number;
  wioLastSeenAgeMs: number;
  phoneRssi: number;       // signed int8
  phoneConnected: boolean;
  bleProxy: boolean;
  sx1262Present: boolean;
}

export interface CmdLogTailResponseV1 {
  lineCount: number;
  totalBytes: number;
  lines: string[]; // null-terminated boundaries already split for callers
}

// Slice #3 — log streaming lease.
export interface CmdStartLogStreamRequestV1 {
  leaseDurationMs: number;
  lineCap: number; // 0 → device default
}

export interface CmdStartLogStreamResponseV1 {
  streamId: number;
  grantedLineCap: number;
  grantedDurationMs: number;
}

export interface CmdStopLogStreamRequestV1 {
  streamId: number;
}

// LogStreamChunkV1 — header + packed null-terminated lines.
export interface LogStreamChunkV1 {
  version: number;
  streamId: number;
  seq: number;
  dropped: number;        // lines lost since the previous chunk
  ending: boolean;        // LOG_STREAM_FLAG_END set
  lineCount: number;
  totalBytes: number;
  lines: string[];
}

// Slice #4 — dashboard streaming lease.
export interface CmdStartDashboardStreamRequestV1 {
  leaseDurationMs: number;
  intervalMs: number;       // 0 → device default
}

export interface CmdStartDashboardStreamResponseV1 {
  streamId: number;
  grantedIntervalMs: number;
  grantedDurationMs: number;
}

export interface CmdStopDashboardStreamRequestV1 {
  streamId: number;
}

// One chunk = header + a full CmdDashboardSnapshotV1 payload.
export interface DashboardStreamChunkV1 {
  version: number;
  streamId: number;
  seq: number;
  ending: boolean;
  snapshot: CmdDashboardSnapshotV1;
}

// Slice #7 — throttled phone notifications pushed from device.
export interface PhoneNotificationV1 {
  version: number;
  type: number;            // PHONE_NOTIF_TYPE_*
  severity: number;        // PHONE_NOTIF_SEVERITY_*
  collapsed: boolean;      // PHONE_NOTIF_FLAG_COLLAPSED set
  seq: number;
  collapsedCount: number;  // duplicates folded during the throttle window
  deviceUptimeMs: number;
  text: string;
}
