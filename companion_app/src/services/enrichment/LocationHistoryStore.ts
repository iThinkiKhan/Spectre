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
const LOCATION_STATIONARY_HEARTBEAT_MS = 60 * 1000;
// Stationary fixes fold into an accuracy-weighted mean; cap the improvement so
// we never claim sub-meter certainty from phone GPS.
const STATIONARY_ACCURACY_FLOOR_M = 2.5;
const STATIONARY_AVG_SAMPLE_CAP = 16;
// Gate new fixes against the cluster centroid, not the last jittery sample.
const STATIONARY_MOTION_K = 2.5;

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

function distanceMetersLL(latA: number, lonA: number, latB: number, lonB: number) {
  const radiusM = 6_371_000;
  const lat1 = (latA * Math.PI) / 180;
  const lat2 = (latB * Math.PI) / 180;
  const deltaLat = ((latB - latA) * Math.PI) / 180;
  const deltaLon = ((lonB - lonA) * Math.PI) / 180;
  const sinLat = Math.sin(deltaLat / 2);
  const sinLon = Math.sin(deltaLon / 2);
  const h =
    sinLat * sinLat +
    Math.cos(lat1) * Math.cos(lat2) * sinLon * sinLon;
  return 2 * radiusM * Math.atan2(Math.sqrt(h), Math.sqrt(1 - h));
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

// Small in-memory cushion between debounced persistence passes.
const LOCATION_HISTORY_INMEMORY_HEADROOM = 256;

// Mirrored by Android SpectreLocationService; change both together.
type StationaryAccumulator = {
  source: LocationHistoryFix['source'];
  anchorTimestamp: number; // identity of the marker this cluster is folding into
  count: number;
  sumWeight: number;
  sumWeightLat: number;
  sumWeightLon: number;
  sumWeightAlt: number;
  rawAccuracy: number; // worst raw single-fix accuracy seen — drives the motion gate
  bestAccuracy: number; // best raw single-fix accuracy — base for the √N reduction
};

let stationaryAccumulator: StationaryAccumulator | null = null;

function accumWeight(accuracy: number): number {
  const a = Math.max(accuracy, STATIONARY_ACCURACY_FLOOR_M);
  return 1 / (a * a);
}

function seedAccumulator(marker: LocationHistoryFix): StationaryAccumulator {
  const w = accumWeight(marker.accuracy);
  return {
    source: marker.source,
    anchorTimestamp: marker.timestamp,
    count: 1,
    sumWeight: w,
    sumWeightLat: w * marker.lat,
    sumWeightLon: w * marker.lon,
    sumWeightAlt: w * marker.alt,
    rawAccuracy: marker.accuracy,
    bestAccuracy: marker.accuracy > 0 ? marker.accuracy : STATIONARY_ACCURACY_FLOOR_M,
  };
}

function foldAccumulator(acc: StationaryAccumulator, fix: LocationHistoryFix) {
  const w = accumWeight(fix.accuracy);
  acc.count += 1;
  acc.sumWeight += w;
  acc.sumWeightLat += w * fix.lat;
  acc.sumWeightLon += w * fix.lon;
  acc.sumWeightAlt += w * fix.alt;
  if (fix.accuracy > acc.rawAccuracy) {
    acc.rawAccuracy = fix.accuracy;
  }
  if (fix.accuracy > 0 && fix.accuracy < acc.bestAccuracy) {
    acc.bestAccuracy = fix.accuracy;
  }
}

function accumulatedMean(acc: StationaryAccumulator) {
  const n = Math.min(acc.count, STATIONARY_AVG_SAMPLE_CAP);
  return {
    lat: acc.sumWeightLat / acc.sumWeight,
    lon: acc.sumWeightLon / acc.sumWeight,
    alt: acc.sumWeightAlt / acc.sumWeight,
    accuracy: Math.max(acc.bestAccuracy / Math.sqrt(n), STATIONARY_ACCURACY_FLOOR_M),
  };
}

function withinMotionRadius(
  refLat: number,
  refLon: number,
  refRawAccuracy: number,
  sample: LocationHistoryFix,
) {
  const thresholdM =
    STATIONARY_MOTION_K *
    Math.max(refRawAccuracy, sample.accuracy, STATIONARY_ACCURACY_FLOOR_M);
  return distanceMetersLL(refLat, refLon, sample.lat, sample.lon) <= thresholdM;
}

function insertFixSorted(
  history: LocationHistoryFix[],
  fix: LocationHistoryFix,
): LocationHistoryFix[] {
  const next = history.slice();
  let insertAt = next.length;
  for (let i = next.length - 1; i >= 0; i -= 1) {
    if (next[i].timestamp <= fix.timestamp) {
      insertAt = i + 1;
      break;
    }
    if (i === 0) {
      insertAt = 0;
    }
  }
  next.splice(insertAt, 0, fix);
  pendingDirtyBuckets.add(bucketIdFor(fix.timestamp));

  if (next.length > LOCATION_HISTORY_MAX_SAMPLES + LOCATION_HISTORY_INMEMORY_HEADROOM) {
    return next.slice(next.length - LOCATION_HISTORY_MAX_SAMPLES);
  }
  return next;
}

export function rememberLocationSample(
  previous: LocationHistoryFix[],
  sample: LocationHistoryFix,
  _referenceTime = Date.now(),
): LocationHistoryFix[] {
  const sanitizedSample = sanitizeFix(sample);
  if (!sanitizedSample) {
    return previous;
  }

  // Tail scan avoids sorting the whole history on each GPS poll.
  let previousMarker: LocationHistoryFix | null = null;
  for (let i = previous.length - 1; i >= 0; i -= 1) {
    if (previous[i].source === sample.source) {
      previousMarker = previous[i];
      break;
    }
  }

  const accMatches =
    !!stationaryAccumulator &&
    !!previousMarker &&
    stationaryAccumulator.source === sanitizedSample.source &&
    stationaryAccumulator.anchorTimestamp === previousMarker.timestamp;

  // Use raw cluster accuracy so refining the mean never tightens the gate.
  let refLat = 0;
  let refLon = 0;
  let refRawAccuracy = 0;
  if (accMatches) {
    const centroid = accumulatedMean(stationaryAccumulator!);
    refLat = centroid.lat;
    refLon = centroid.lon;
    refRawAccuracy = stationaryAccumulator!.rawAccuracy;
  } else if (previousMarker) {
    refLat = previousMarker.lat;
    refLon = previousMarker.lon;
    refRawAccuracy = previousMarker.accuracy;
  }

  if (
    previousMarker &&
    withinMotionRadius(refLat, refLon, refRawAccuracy, sanitizedSample)
  ) {
    // Stationary: fold this fix into the cluster's running mean.
    if (!accMatches) {
      stationaryAccumulator = seedAccumulator(previousMarker);
    }
    foldAccumulator(stationaryAccumulator!, sanitizedSample);
    const mean = accumulatedMean(stationaryAccumulator!);

    if (
      sanitizedSample.timestamp - previousMarker.timestamp >=
      LOCATION_STATIONARY_HEARTBEAT_MS
    ) {
      // Heartbeat preserves temporal coverage while keeping the averaged fix.
      const marker: LocationHistoryFix = {
        lat: mean.lat,
        lon: mean.lon,
        alt: mean.alt,
        accuracy: mean.accuracy,
        timestamp: sanitizedSample.timestamp,
        source: sanitizedSample.source,
        provider: sanitizedSample.provider,
      };
      const next = insertFixSorted(previous, marker);
      stationaryAccumulator!.anchorTimestamp = marker.timestamp;
      return next;
    }

    // Sub-heartbeat: refine the marker in place.
    previousMarker.lat = mean.lat;
    previousMarker.lon = mean.lon;
    previousMarker.alt = mean.alt;
    previousMarker.accuracy = mean.accuracy;
    previousMarker.provider = sanitizedSample.provider;
    pendingDirtyBuckets.add(bucketIdFor(previousMarker.timestamp));
    return previous;
  }

  // Moved (or no prior marker): start a fresh cluster.
  stationaryAccumulator = null;
  return insertFixSorted(previous, sanitizedSample);
}

// Pre-pruned snapshot for batch enrichment.
export type LocationCandidateSet = {
  manualOverride: LocationHistoryFix | null;
  deviceCandidates: LocationHistoryFix[];
};

export type LocationDriftSummary = {
  nearestTimestamp: number;
  nearestDriftMs: number;
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

// Metres per degree of latitude. Longitude is scaled by cos(lat) at use.
const METERS_PER_DEG_LAT = 111_320;

/**
 * Position of the observer AT the event, not at the nearest GPS fix.
 *
 * Fixes arrive every ~20 s, so snapping an event to the nearest one leaves up
 * to ~10 s of unmodelled motion: about 14 m walking, 150 m in a vehicle. That
 * error lands directly on the observer position a trilateration solve is
 * anchored to, and it dwarfs the RF-side error budget.
 *
 * When the event falls between two fixes we interpolate along the segment
 * instead. The residual is then only the deviation from a straight line over
 * one sampling interval, not the whole distance travelled.
 *
 * `accuracy` is widened to carry what is actually unknown: the GPS accuracy of
 * the bracketing fixes combined with the motion-derived uncertainty. Consumers
 * already weight by accuracy, so the uncertainty propagates without a format
 * change - and a stale or badly bracketed match now says so instead of
 * reporting a confident-looking fix.
 */
function interpolateFix(
  before: LocationHistoryFix,
  after: LocationHistoryFix,
  eventUnixMs: number,
): LocationHistoryFix {
  const span = after.timestamp - before.timestamp;
  if (span <= 0) {
    return before;
  }
  const t = Math.min(1, Math.max(0, (eventUnixMs - before.timestamp) / span));

  const lat = before.lat + (after.lat - before.lat) * t;
  const lon = before.lon + (after.lon - before.lon) * t;
  const alt = before.alt + (after.alt - before.alt) * t;

  // Straight-line distance covered across the bracketing segment.
  const latScale = METERS_PER_DEG_LAT;
  const lonScale = METERS_PER_DEG_LAT * Math.cos((lat * Math.PI) / 180);
  const dx = (after.lon - before.lon) * lonScale;
  const dy = (after.lat - before.lat) * latScale;
  const segmentM = Math.sqrt(dx * dx + dy * dy);

  // The path between two fixes is not necessarily straight. Charge a quarter
  // of the segment length as path-shape uncertainty - generous for a walk,
  // honest for a turn taken between samples.
  const pathUncertaintyM = segmentM * 0.25;
  const gpsAccuracyM = Math.max(before.accuracy, after.accuracy);

  return {
    lat,
    lon,
    alt,
    accuracy: Math.sqrt(
      gpsAccuracyM * gpsAccuracyM + pathUncertaintyM * pathUncertaintyM,
    ),
    // Report the EVENT time: this position is an estimate for that instant,
    // not an observation made at either bracketing fix.
    timestamp: eventUnixMs,
    source: before.source,
    provider: before.provider,
  };
}

/** Nearest-fix fallback, with the unmodelled motion folded into accuracy. */
function widenForDrift(
  fix: LocationHistoryFix,
  eventUnixMs: number,
  neighbourForSpeed: LocationHistoryFix | null,
): LocationHistoryFix {
  const driftMs = Math.abs(fix.timestamp - eventUnixMs);
  if (driftMs <= 0) {
    return fix;
  }

  // Estimate speed from the nearest neighbouring fix when there is one;
  // otherwise assume a walking pace rather than pretending drift is free.
  let speedMps = 1.4;
  if (neighbourForSpeed) {
    const dtMs = Math.abs(neighbourForSpeed.timestamp - fix.timestamp);
    if (dtMs > 0) {
      const latScale = METERS_PER_DEG_LAT;
      const lonScale = METERS_PER_DEG_LAT * Math.cos((fix.lat * Math.PI) / 180);
      const dx = (neighbourForSpeed.lon - fix.lon) * lonScale;
      const dy = (neighbourForSpeed.lat - fix.lat) * latScale;
      speedMps = Math.sqrt(dx * dx + dy * dy) / (dtMs / 1000);
    }
  }

  const motionM = speedMps * (driftMs / 1000);
  return {
    ...fix,
    accuracy: Math.sqrt(fix.accuracy * fix.accuracy + motionM * motionM),
    timestamp: eventUnixMs,
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

  // Bracketing pair, if the event falls inside the recorded track.
  {
    let before: LocationHistoryFix | null = null;
    let after: LocationHistoryFix | null = null;
    for (const candidate of candidates) {
      if (candidate.timestamp <= eventUnixMs) {
        if (!before || candidate.timestamp > before.timestamp) {
          before = candidate;
        }
      } else if (!after || candidate.timestamp < after.timestamp) {
        after = candidate;
      }
    }
    if (
      before &&
      after &&
      eventUnixMs - before.timestamp <= LOCATION_BOOTSTRAP_MAX_DRIFT_MS &&
      after.timestamp - eventUnixMs <= LOCATION_BOOTSTRAP_MAX_DRIFT_MS
    ) {
      return interpolateFix(before, after, eventUnixMs);
    }
  }

  let nearest: LocationHistoryFix | null = null;
  let nearestDrift = Number.MAX_SAFE_INTEGER;
  for (const candidate of candidates) {
    const drift = Math.abs(candidate.timestamp - eventUnixMs);
    if (drift < nearestDrift) {
      nearest = candidate;
      nearestDrift = drift;
    }
    if (candidate.timestamp > eventUnixMs && drift > nearestDrift) {
      break;
    }
  }

  if (!nearest || nearestDrift > LOCATION_BOOTSTRAP_MAX_DRIFT_MS) {
    return null;
  }

  // Only one side of the event has a fix (start or end of a track, or a GPS
  // outage). Keep it, but say how uncertain it is rather than reporting the
  // fix's own accuracy as if it applied at the event.
  let neighbour: LocationHistoryFix | null = null;
  for (const candidate of candidates) {
    if (candidate === nearest) continue;
    if (
      !neighbour ||
      Math.abs(candidate.timestamp - nearest.timestamp) <
        Math.abs(neighbour.timestamp - nearest.timestamp)
    ) {
      neighbour = candidate;
    }
  }
  return widenForDrift(nearest, eventUnixMs, neighbour);
}

export function nearestLocationDriftFromCandidates(
  eventUnixMs: number,
  set: LocationCandidateSet,
): LocationDriftSummary | null {
  if (set.manualOverride) {
    return {
      nearestTimestamp: set.manualOverride.timestamp,
      nearestDriftMs: 0,
    };
  }

  const candidates = set.deviceCandidates;
  let nearest: LocationHistoryFix | null = null;
  let nearestDrift = Number.MAX_SAFE_INTEGER;
  for (const candidate of candidates) {
    const drift = Math.abs(candidate.timestamp - eventUnixMs);
    if (drift < nearestDrift) {
      nearest = candidate;
      nearestDrift = drift;
    }
    if (candidate.timestamp > eventUnixMs && drift > nearestDrift) {
      break;
    }
  }

  return nearest
    ? {nearestTimestamp: nearest.timestamp, nearestDriftMs: nearestDrift}
    : null;
}

export function locationForUnixMs(
  eventUnixMs: number,
  activeLocation: LocationHistoryFix | null,
  history: LocationHistoryFix[],
): LocationHistoryFix | null {
  // Single-event convenience path; batches should reuse a candidate set.
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

  // Boundary buckets may be partially trimmed; force-rewrite them.
  const cutoffBucket = bucketIdFor(Date.now() - LOCATION_HISTORY_MAX_AGE_MS);
  if (liveBuckets.has(cutoffBucket)) {
    pendingDirtyBuckets.add(cutoffBucket);
  }
  const sortedLiveIds = [...liveBuckets.keys()].sort((a, b) => a - b);
  if (sortedLiveIds.length > 0) {
    pendingDirtyBuckets.add(sortedLiveIds[0]);
  }

  const priorIndex = await loadBucketIndex();
  // indexChanged assumes both arrays are sorted.
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

  // Buckets first, then index: a crash can waste bytes but not lose indexed data.
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
        // Another sample landed while we were writing.
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
  // The native foreground recorder is the canonical location source and owns
  // the same chunked history. Avoid react-native-geolocation-service here: its
  // bundled client targets the pre-interface Play Services API and crashes on
  // current Android 16 Play Services before its error callback can run.
  const history = await loadLocationHistory();
  const latest = history.length > 0 ? history[history.length - 1] : null;
  return latest && Date.now() - latest.timestamp <= 2 * 60_000 ? latest : null;
}
