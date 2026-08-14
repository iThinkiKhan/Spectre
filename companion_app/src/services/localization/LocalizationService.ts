import type {RelayArchiveRecord} from '../relay/SpectreRelayService';

export type TargetKind =
  | 'access-point'
  | 'wifi-device'
  | 'ble'
  | 'subghz'
  | 'drone'
  | 'unknown';

export type LocalizationObservation = {
  eventId: number;
  lat: number;
  lon: number;
  accuracyM: number;
  rssi: number;
  timestamp: number;
  sensorId: string;
};

export type LocalizationEstimate = {
  lat: number;
  lon: number;
  uncertaintyM: number;
  geometrySpreadM: number;
  sampleCount: number;
  confidence: 'insufficient' | 'low' | 'medium' | 'high';
  method: 'single-fix' | 'signal-weighted';
};

export type LocalizationTarget = {
  id: string;
  identity: string;
  label: string;
  kind: TargetKind;
  lastSeen: number;
  lastRssi: number;
  strongestRssi: number;
  totalRecords: number;
  observations: LocalizationObservation[];
  estimate: LocalizationEstimate | null;
};

export type LocalizationSnapshot = {
  targets: LocalizationTarget[];
  recordCount: number;
  locatedRecordCount: number;
  updatedAt: number;
};

type Payload = Record<string, unknown>;

type LocationFields = {
  lat: number;
  lon: number;
  accuracyM: number;
  gpsTimestamp: number | null;
};

const EARTH_RADIUS_M = 6_371_000;

function finiteNumber(value: unknown): number | null {
  const number = typeof value === 'number' ? value : Number(value);
  return Number.isFinite(number) ? number : null;
}

function text(value: unknown): string {
  return typeof value === 'string' ? value.trim() : '';
}

function topicKind(topic: string): TargetKind {
  const suffix = topic.split('/').pop()?.toLowerCase();
  if (suffix === 'network') return 'access-point';
  if (suffix === 'probe' || suffix === 'device') return 'wifi-device';
  if (suffix === 'ble') return 'ble';
  if (suffix === 'subghz') return 'subghz';
  if (suffix === 'drone') return 'drone';
  return 'unknown';
}

function targetIdentity(kind: TargetKind, payload: Payload): string {
  if (kind === 'access-point') return text(payload.bssid);
  if (kind === 'wifi-device') {
    return (
      text(payload.track_id) ||
      text(payload.ie_fingerprint) ||
      text(payload.mac)
    );
  }
  if (kind === 'ble') return text(payload.address) || text(payload.mac);
  if (kind === 'subghz') {
    return text(payload.track_id) || String(payload.source_addr ?? '');
  }
  if (kind === 'drone') {
    return text(payload.drone_id) || text(payload.id) || text(payload.mac);
  }
  return text(payload.track_id) || text(payload.mac) || text(payload.bssid);
}

function targetLabel(kind: TargetKind, identity: string, payload: Payload) {
  if (kind === 'access-point') return text(payload.ssid) || identity;
  if (kind === 'drone') return text(payload.drone_id) || identity;
  if (kind === 'ble') return text(payload.name) || identity;
  if (kind === 'subghz') return `Sub-GHz ${identity}`;
  const vendor = text(payload.vendor);
  return vendor || identity;
}

function locationFields(payload: Payload): LocationFields | null {
  const nestedGps =
    payload.gps && typeof payload.gps === 'object'
      ? (payload.gps as Payload)
      : null;
  const lat = finiteNumber(payload.lat) ?? finiteNumber(nestedGps?.lat);
  const lon = finiteNumber(payload.lon) ?? finiteNumber(nestedGps?.lon);
  const accuracy =
    finiteNumber(payload.acc) ??
    finiteNumber(payload.accuracy) ??
    finiteNumber(nestedGps?.acc) ??
    finiteNumber(nestedGps?.accuracy) ??
    25;
  if (
    lat === null ||
    lon === null ||
    lat < -90 || lat > 90 ||
    lon < -180 || lon > 180 ||
    (lat === 0 && lon === 0) ||
    payload.enrich_no_data === true
  ) {
    return null;
  }
  return {
    lat,
    lon,
    accuracyM: Math.max(1, accuracy),
    gpsTimestamp: finiteNumber(payload.gps_ts),
  };
}

function eventKey(sessionId: string, eventId: number) {
  return `${sessionId}:${eventId}`;
}

export function distanceMeters(
  a: {lat: number; lon: number},
  b: {lat: number; lon: number},
) {
  const lat1 = (a.lat * Math.PI) / 180;
  const lat2 = (b.lat * Math.PI) / 180;
  const dLat = lat2 - lat1;
  const dLon = ((b.lon - a.lon) * Math.PI) / 180;
  const h =
    Math.sin(dLat / 2) ** 2 +
    Math.cos(lat1) * Math.cos(lat2) * Math.sin(dLon / 2) ** 2;
  return 2 * EARTH_RADIUS_M * Math.asin(Math.min(1, Math.sqrt(h)));
}

function spatiallyDistinct(observations: LocalizationObservation[]) {
  const selected: LocalizationObservation[] = [];
  [...observations]
    .sort((a, b) => b.rssi - a.rssi)
    .forEach(observation => {
      const duplicate = selected.some(
        existing => distanceMeters(existing, observation) < 7,
      );
      if (!duplicate) selected.push(observation);
    });
  return selected.slice(0, 64);
}

export function estimateTarget(
  observations: LocalizationObservation[],
): LocalizationEstimate | null {
  if (observations.length === 0) return null;
  const samples = spatiallyDistinct(observations);
  if (samples.length === 1) {
    return {
      lat: samples[0].lat,
      lon: samples[0].lon,
      uncertaintyM: Math.max(75, samples[0].accuracyM * 2),
      geometrySpreadM: 0,
      sampleCount: 1,
      confidence: 'insufficient',
      method: 'single-fix',
    };
  }

  const strongest = Math.max(...samples.map(sample => sample.rssi));
  let weightTotal = 0;
  let latTotal = 0;
  let lonTotal = 0;
  for (const sample of samples) {
    // Relative received power is more honest than assuming a universal radio
    // transmit power. GPS accuracy prevents a poor fix dominating the result.
    const signalWeight = Math.pow(10, (sample.rssi - strongest) / 12);
    const accuracyWeight = 1 / Math.max(5, sample.accuracyM);
    const weight = signalWeight * accuracyWeight;
    weightTotal += weight;
    latTotal += sample.lat * weight;
    lonTotal += sample.lon * weight;
  }
  const lat = latTotal / weightTotal;
  const lon = lonTotal / weightTotal;

  let weightedVariance = 0;
  let geometrySpreadM = 0;
  for (const sample of samples) {
    const signalWeight = Math.pow(10, (sample.rssi - strongest) / 12);
    const accuracyWeight = 1 / Math.max(5, sample.accuracyM);
    const weight = signalWeight * accuracyWeight;
    const distance = distanceMeters({lat, lon}, sample);
    weightedVariance += weight * distance * distance;
    geometrySpreadM = Math.max(geometrySpreadM, distance);
  }

  const rms = Math.sqrt(weightedVariance / weightTotal);
  const medianAccuracy = [...samples]
    .map(sample => sample.accuracyM)
    .sort((a, b) => a - b)[Math.floor(samples.length / 2)];
  const uncertaintyM = Math.max(20, rms + medianAccuracy);
  const confidence =
    samples.length >= 6 && geometrySpreadM >= 60 && uncertaintyM <= 100
      ? 'high'
      : samples.length >= 3 && geometrySpreadM >= 25
        ? 'medium'
        : 'low';

  return {
    lat,
    lon,
    uncertaintyM,
    geometrySpreadM,
    sampleCount: samples.length,
    confidence,
    method: 'signal-weighted',
  };
}

function recordTimestamp(record: RelayArchiveRecord, payload: Payload) {
  const gpsSeconds = finiteNumber(payload.gps_ts);
  if (gpsSeconds && gpsSeconds > 1_500_000_000) return gpsSeconds * 1000;
  const iso = text(payload.ts);
  const parsed = iso ? Date.parse(iso) : NaN;
  return Number.isFinite(parsed) ? parsed : record.receivedAt;
}

export function buildLocalizationSnapshot(
  records: RelayArchiveRecord[],
): LocalizationSnapshot {
  const targets = new Map<string, LocalizationTarget>();
  const enrichmentByEvent = new Map<string, LocationFields>();
  let locatedRecordCount = 0;

  // Spectre persists fixes as enrichment deltas keyed to the original event.
  // Resolved upload readers normally join them before offload, but older phone
  // archives can contain the two records separately. Recover those fixes here
  // so a software upgrade makes the existing field archive useful immediately.
  for (const record of records) {
    let payload: Payload;
    try {
      payload = JSON.parse(record.payload) as Payload;
    } catch {
      continue;
    }
    const targetEventId = finiteNumber(payload.event_id);
    const location = locationFields(payload);
    if (targetEventId && targetEventId > 0 && location) {
      enrichmentByEvent.set(
        eventKey(record.sessionId || text(payload.session), targetEventId),
        location,
      );
    }
  }

  for (const record of records) {
    let payload: Payload;
    try {
      payload = JSON.parse(record.payload) as Payload;
    } catch {
      continue;
    }
    const kind = topicKind(record.topic);
    const identity = targetIdentity(kind, payload);
    if (!identity || kind === 'unknown') continue;
    const id = `${kind}:${identity.toUpperCase()}`;
    const joinedLocation =
      locationFields(payload) ??
      enrichmentByEvent.get(
        eventKey(record.sessionId || text(payload.session), record.eventId),
      ) ??
      null;
    const timestamp =
      joinedLocation?.gpsTimestamp && joinedLocation.gpsTimestamp > 1_500_000_000
        ? joinedLocation.gpsTimestamp * 1000
        : recordTimestamp(record, payload);
    const rssi = finiteNumber(payload.rssi) ?? -127;
    const existing = targets.get(id) ?? {
      id,
      identity,
      label: targetLabel(kind, identity, payload),
      kind,
      lastSeen: timestamp,
      lastRssi: rssi,
      strongestRssi: rssi,
      totalRecords: 0,
      observations: [],
      estimate: null,
    };
    existing.totalRecords += 1;
    existing.strongestRssi = Math.max(existing.strongestRssi, rssi);
    if (timestamp >= existing.lastSeen) {
      existing.lastSeen = timestamp;
      existing.lastRssi = rssi;
      existing.label = targetLabel(kind, identity, payload);
    }

    if (
      joinedLocation &&
      rssi < 0 && rssi >= -127
    ) {
      locatedRecordCount += 1;
      existing.observations.push({
        eventId: record.eventId,
        lat: joinedLocation.lat,
        lon: joinedLocation.lon,
        accuracyM: joinedLocation.accuracyM,
        rssi,
        timestamp,
        sensorId: text(payload.sensor) || 'spectre',
      });
    }
    targets.set(id, existing);
  }

  const targetList = [...targets.values()]
    .map(target => ({
      ...target,
      observations: target.observations.sort((a, b) => b.timestamp - a.timestamp),
      estimate: estimateTarget(target.observations),
    }))
    .sort((a, b) => {
      const aLocated = a.estimate ? 1 : 0;
      const bLocated = b.estimate ? 1 : 0;
      return bLocated - aLocated || b.lastSeen - a.lastSeen;
    });

  return {
    targets: targetList,
    recordCount: records.length,
    locatedRecordCount,
    updatedAt: Date.now(),
  };
}

export const EMPTY_LOCALIZATION_SNAPSHOT: LocalizationSnapshot = {
  targets: [],
  recordCount: 0,
  locatedRecordCount: 0,
  updatedAt: 0,
};
