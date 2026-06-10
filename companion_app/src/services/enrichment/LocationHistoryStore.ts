import {Platform} from 'react-native';
import Geolocation from 'react-native-geolocation-service';
import {
  getStoredString,
  removeStoredString,
  setStoredString,
} from '../storage/NativeKeyValueStore';

export type LocationHistoryFix = {
  lat: number;
  lon: number;
  alt: number;
  accuracy: number;
  timestamp: number;
  source: 'device' | 'manual';
  provider: string | null;
};

const LEGACY_STORAGE_KEY = '@spectre/location-history-v1';
const BUCKET_INDEX_KEY = '@spectre/location-history-v2/index';
const BUCKET_KEY_PREFIX = '@spectre/location-history-v2/day/';
const DAY_MS = 24 * 60 * 60 * 1000;

export const LOCATION_HISTORY_MAX_SAMPLES = 200_000;
export const LOCATION_HISTORY_MAX_AGE_MS = 30 * 24 * 60 * 60 * 1000;
export const LOCATION_BOOTSTRAP_MAX_DRIFT_MS = 5 * 60 * 1000;

function bucketIdFor(timestamp: number): number {
  return Math.floor(timestamp / DAY_MS);
}

function bucketKey(id: number): string {
  return `${BUCKET_KEY_PREFIX}${id}`;
}

const pendingDirtyBuckets = new Set<number>();

function finiteNumber(value: unknown, fallback: number) {
  return typeof value === 'number' && Number.isFinite(value) ? value : fallback;
}

function sanitizeFix(value: unknown): LocationHistoryFix | null {
  if (!value || typeof value !== 'object') {
    return null;
  }

  const raw = value as Partial<LocationHistoryFix>;
  const lat = finiteNumber(raw.lat, Number.NaN);
  const lon = finiteNumber(raw.lon, Number.NaN);
  const timestamp = finiteNumber(raw.timestamp, 0);

  if (
    !Number.isFinite(lat) ||
    !Number.isFinite(lon) ||
    Math.abs(lat) > 90 ||
    Math.abs(lon) > 180 ||
    timestamp <= 0
  ) {
    return null;
  }

  return {
    lat,
    lon,
    alt: finiteNumber(raw.alt, 0),
    accuracy: Math.max(0, finiteNumber(raw.accuracy, 0)),
    timestamp,
    source: raw.source === 'manual' ? 'manual' : 'device',
    provider: typeof raw.provider === 'string' ? raw.provider : null,
  };
}

function distanceMeters(a: LocationHistoryFix, b: LocationHistoryFix) {
  const radiusM = 6_371_000;
  const lat1 = (a.lat * Math.PI) / 180;
  const lat2 = (b.lat * Math.PI) / 180;
  const deltaLat = ((b.lat - a.lat) * Math.PI) / 180;
  const deltaLon = ((b.lon - a.lon) * Math.PI) / 180;
  const sinLat = Math.sin(deltaLat / 2);
  const sinLon = Math.sin(deltaLon / 2);
  const h =
    sinLat * sinLat +
    Math.cos(lat1) * Math.cos(lat2) * sinLon * sinLon;
  return 2 * radiusM * Math.atan2(Math.sqrt(h), Math.sqrt(1 - h));
}

function withinAccuracyRadius(a: LocationHistoryFix, b: LocationHistoryFix) {
  const thresholdM = Math.max(a.accuracy, b.accuracy, 0);
  return distanceMeters(a, b) <= thresholdM;
}

export function pruneLocationHistory(
  history: LocationHistoryFix[],
  referenceTime = Date.now(),
) {
  const cutoff = referenceTime - LOCATION_HISTORY_MAX_AGE_MS;
  const sorted = history
    .map(sanitizeFix)
    .filter((entry): entry is LocationHistoryFix => !!entry)
    .sort((a, b) => a.timestamp - b.timestamp);
  const anchor = [...sorted].reverse().find(entry => entry.timestamp < cutoff);
  const retained = sorted.filter(entry => entry.timestamp >= cutoff);
  const withAnchor = anchor ? [anchor, ...retained] : retained;

  if (withAnchor.length <= LOCATION_HISTORY_MAX_SAMPLES) {
    return withAnchor;
  }

  const recent = withAnchor.slice(-(LOCATION_HISTORY_MAX_SAMPLES - 1));
  return anchor ? [anchor, ...recent.filter(entry => entry !== anchor)] : recent;
}

// Headroom past the cap before we trim in-memory.  The persist path runs the
// full age-based prune every 30s; this just stops unbounded growth between
// flushes.
const LOCATION_HISTORY_INMEMORY_HEADROOM = 256;

export function rememberLocationSample(
  previous: LocationHistoryFix[],
  sample: LocationHistoryFix,
  _referenceTime = Date.now(),
): LocationHistoryFix[] {
  const sanitizedSample = sanitizeFix(sample);
  if (!sanitizedSample) {
    return previous;
  }

  // Tail scan for the most recent same-source marker.  Avoids the previous
  // implementation's full pruneLocationHistory (sanitize + filter + sort over
  // ~200k entries) on every 10s GPS poll.
  let previousMarker: LocationHistoryFix | null = null;
  for (let i = previous.length - 1; i >= 0; i -= 1) {
    if (previous[i].source === sample.source) {
      previousMarker = previous[i];
      break;
    }
  }

  if (previousMarker && withinAccuracyRadius(previousMarker, sanitizedSample)) {
    if (
      sanitizedSample.accuracy > 0 &&
      (previousMarker.accuracy === 0 ||
        sanitizedSample.accuracy < previousMarker.accuracy)
    ) {
      previousMarker.lat = sanitizedSample.lat;
      previousMarker.lon = sanitizedSample.lon;
      previousMarker.alt = sanitizedSample.alt;
      previousMarker.accuracy = sanitizedSample.accuracy;
      previousMarker.provider = sanitizedSample.provider;
      pendingDirtyBuckets.add(bucketIdFor(previousMarker.timestamp));
    }
    return previous;
  }

  // Samples almost always arrive monotonically.  Tail-insert in O(1) instead
  // of a full Array.sort.
  const next = previous.slice();
  let insertAt = next.length;
  for (let i = next.length - 1; i >= 0; i -= 1) {
    if (next[i].timestamp <= sanitizedSample.timestamp) {
      insertAt = i + 1;
      break;
    }
    if (i === 0) {
      insertAt = 0;
    }
  }
  next.splice(insertAt, 0, sanitizedSample);
  pendingDirtyBuckets.add(bucketIdFor(sanitizedSample.timestamp));

  if (next.length > LOCATION_HISTORY_MAX_SAMPLES + LOCATION_HISTORY_INMEMORY_HEADROOM) {
    return next.slice(next.length - LOCATION_HISTORY_MAX_SAMPLES);
  }
  return next;
}

// Pre-pruned snapshot for batch enrichment.  Callers that resolve locations
// for many events in a row should build this once and reuse it instead of
// re-pruning the entire history per event.
export type LocationCandidateSet = {
  manualOverride: LocationHistoryFix | null;
  deviceCandidates: LocationHistoryFix[];
};

export function buildLocationCandidates(
  activeLocation: LocationHistoryFix | null,
  history: LocationHistoryFix[],
): LocationCandidateSet {
  if (activeLocation?.source === 'manual') {
    return {manualOverride: activeLocation, deviceCandidates: []};
  }
  const pruned = pruneLocationHistory(
    activeLocation ? [...history, activeLocation] : history,
  );
  return {
    manualOverride: null,
    deviceCandidates: pruned.filter(candidate => candidate.source === 'device'),
  };
}

export function locationForUnixMsFromCandidates(
  eventUnixMs: number,
  set: LocationCandidateSet,
): LocationHistoryFix | null {
  if (set.manualOverride) {
    return set.manualOverride;
  }

  if (eventUnixMs < Date.now() - LOCATION_HISTORY_MAX_AGE_MS) {
    return null;
  }

  const candidates = set.deviceCandidates;

  let bestBefore: LocationHistoryFix | null = null;
  for (const candidate of candidates) {
    if (candidate.timestamp <= eventUnixMs) {
      bestBefore = candidate;
      continue;
    }
    break;
  }

  if (bestBefore) {
    return bestBefore;
  }

  let nearest: LocationHistoryFix | null = null;
  let nearestDrift = Number.MAX_SAFE_INTEGER;
  for (const candidate of candidates) {
    const drift = Math.abs(candidate.timestamp - eventUnixMs);
    if (drift < nearestDrift) {
      nearest = candidate;
      nearestDrift = drift;
    }
  }

  return nearest && nearestDrift <= LOCATION_BOOTSTRAP_MAX_DRIFT_MS
    ? nearest
    : null;
}

export function locationForUnixMs(
  eventUnixMs: number,
  activeLocation: LocationHistoryFix | null,
  history: LocationHistoryFix[],
): LocationHistoryFix | null {
  // Single-event call site — builds the candidate set, then looks up.  Batch
  // call sites should call buildLocationCandidates once and reuse the result
  // with locationForUnixMsFromCandidates per event.
  return locationForUnixMsFromCandidates(
    eventUnixMs,
    buildLocationCandidates(activeLocation, history),
  );
}

async function loadBucketIndex(): Promise<number[]> {
  try {
    const raw = await getStoredString(BUCKET_INDEX_KEY);
    if (!raw) {
      return [];
    }
    const parsed = JSON.parse(raw);
    if (!Array.isArray(parsed)) {
      return [];
    }
    return parsed.filter(
      (id): id is number => typeof id === 'number' && Number.isFinite(id),
    );
  } catch {
    return [];
  }
}

async function migrateLegacyHistory(): Promise<LocationHistoryFix[] | null> {
  let legacyRaw: string | null;
  try {
    legacyRaw = await getStoredString(LEGACY_STORAGE_KEY);
  } catch {
    return null;
  }
  if (!legacyRaw) {
    return null;
  }

  let pruned: LocationHistoryFix[] = [];
  try {
    const parsed = JSON.parse(legacyRaw);
    pruned = pruneLocationHistory(Array.isArray(parsed) ? parsed : []);
  } catch {
    pruned = [];
  }

  // Stage every surviving fix's bucket as dirty so the chunked write emits it.
  for (const fix of pruned) {
    pendingDirtyBuckets.add(bucketIdFor(fix.timestamp));
  }
  await persistLocationHistory(pruned);
  await removeStoredString(LEGACY_STORAGE_KEY);
  return pruned;
}

export async function loadLocationHistory(): Promise<LocationHistoryFix[]> {
  const migrated = await migrateLegacyHistory();
  if (migrated) {
    return migrated;
  }

  try {
    const index = await loadBucketIndex();
    if (index.length === 0) {
      return [];
    }
    const buckets = await Promise.all(
      index.map(async id => {
        try {
          const raw = await getStoredString(bucketKey(id));
          if (!raw) {
            return [] as LocationHistoryFix[];
          }
          const parsed = JSON.parse(raw);
          return Array.isArray(parsed)
            ? (parsed as LocationHistoryFix[])
            : [];
        } catch {
          return [] as LocationHistoryFix[];
        }
      }),
    );
    const merged: LocationHistoryFix[] = [];
    for (const bucket of buckets) {
      for (const fix of bucket) {
        merged.push(fix);
      }
    }
    return pruneLocationHistory(merged);
  } catch {
    return [];
  }
}

export async function persistLocationHistory(history: LocationHistoryFix[]) {
  const pruned = pruneLocationHistory(history);

  const liveBuckets = new Map<number, LocationHistoryFix[]>();
  for (const fix of pruned) {
    const id = bucketIdFor(fix.timestamp);
    let bucket = liveBuckets.get(id);
    if (!bucket) {
      bucket = [];
      liveBuckets.set(id, bucket);
    }
    bucket.push(fix);
  }

  // The cutoff bucket may have been partially trimmed by the age filter, and
  // the oldest live bucket may have been partially trimmed by the sample cap.
  // Force-rewrite both so storage matches the in-memory model.
  const cutoffBucket = bucketIdFor(Date.now() - LOCATION_HISTORY_MAX_AGE_MS);
  if (liveBuckets.has(cutoffBucket)) {
    pendingDirtyBuckets.add(cutoffBucket);
  }
  const sortedLiveIds = [...liveBuckets.keys()].sort((a, b) => a - b);
  if (sortedLiveIds.length > 0) {
    pendingDirtyBuckets.add(sortedLiveIds[0]);
  }

  const priorIndex = await loadBucketIndex();
  // We always write the index sorted, but sort defensively in case a future
  // schema or external write leaves it unordered — the indexChanged check
  // below assumes both arrays are sorted.
  priorIndex.sort((a, b) => a - b);
  const priorIndexSet = new Set(priorIndex);

  const writes: Promise<unknown>[] = [];

  for (const id of priorIndex) {
    if (!liveBuckets.has(id)) {
      writes.push(removeStoredString(bucketKey(id)));
    }
  }

  for (const id of sortedLiveIds) {
    if (pendingDirtyBuckets.has(id) || !priorIndexSet.has(id)) {
      const samples = liveBuckets.get(id)!;
      writes.push(setStoredString(bucketKey(id), JSON.stringify(samples)));
    }
  }

  const indexChanged =
    priorIndex.length !== sortedLiveIds.length ||
    priorIndex.some((id, i) => id !== sortedLiveIds[i]);

  pendingDirtyBuckets.clear();

  // Buckets first, then index. A crash between the two leaves orphan bucket
  // files (new keys absent from the on-disk index). Without a list-keys API on
  // the native store, the next reconcile pass cannot see or delete them — they
  // just sit unused. The alternative ordering (index first) would instead let
  // a crash strand the index pointing at empty/stale bucket data, which is
  // worse: silent data loss vs. wasted bytes.
  await Promise.all(writes);
  if (indexChanged) {
    await setStoredString(BUCKET_INDEX_KEY, JSON.stringify(sortedLiveIds));
  }
}

export const LOCATION_HISTORY_PERSIST_DEBOUNCE_MS = 30_000;

let pendingPersistHistory: LocationHistoryFix[] | null = null;
let pendingPersistTimer: ReturnType<typeof setTimeout> | null = null;
let inFlightPersist: Promise<void> | null = null;

function runPendingPersist(): Promise<void> {
  if (!pendingPersistHistory) {
    return inFlightPersist ?? Promise.resolve();
  }
  const snapshot = pendingPersistHistory;
  pendingPersistHistory = null;
  const previous = inFlightPersist ?? Promise.resolve();
  inFlightPersist = previous
    .catch(() => undefined)
    .then(() => persistLocationHistory(snapshot))
    .finally(() => {
      inFlightPersist = null;
      if (pendingPersistHistory && !pendingPersistTimer) {
        // Another sample landed while we were writing — schedule the next flush.
        scheduleLocationHistoryPersist(pendingPersistHistory);
      }
    });
  return inFlightPersist;
}

export function scheduleLocationHistoryPersist(history: LocationHistoryFix[]) {
  pendingPersistHistory = history;
  if (pendingPersistTimer) {
    return;
  }
  pendingPersistTimer = setTimeout(() => {
    pendingPersistTimer = null;
    void runPendingPersist();
  }, LOCATION_HISTORY_PERSIST_DEBOUNCE_MS);
}

export async function flushLocationHistoryPersist(): Promise<void> {
  if (pendingPersistTimer) {
    clearTimeout(pendingPersistTimer);
    pendingPersistTimer = null;
  }
  await runPendingPersist();
  if (inFlightPersist) {
    await inFlightPersist;
  }
}

export async function getCurrentDeviceLocationFix(): Promise<LocationHistoryFix | null> {
  if (Platform.OS !== 'android') {
    return null;
  }

  return new Promise(resolve => {
    Geolocation.getCurrentPosition(
      position => {
        resolve({
          lat: position.coords.latitude,
          lon: position.coords.longitude,
          alt: position.coords.altitude ?? 0,
          accuracy: position.coords.accuracy ?? 0,
          timestamp: position.timestamp || Date.now(),
          source: 'device',
          provider: 'fused',
        });
      },
      () => resolve(null),
      {
        enableHighAccuracy: true,
        timeout: 15_000,
        maximumAge: 20_000,
        forceRequestLocation: false,
        showLocationDialog: true,
      },
    );
  });
}
