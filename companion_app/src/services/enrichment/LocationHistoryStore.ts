import {Platform} from 'react-native';
import Geolocation from 'react-native-geolocation-service';
import {
  getStoredString,
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

const STORAGE_KEY = '@spectre/location-history-v1';

export const LOCATION_HISTORY_MAX_SAMPLES = 20_000;
export const LOCATION_HISTORY_MAX_AGE_MS = 72 * 60 * 60 * 1000;
export const LOCATION_BOOTSTRAP_MAX_DRIFT_MS = 5 * 60 * 1000;

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

export function rememberLocationSample(
  previous: LocationHistoryFix[],
  sample: LocationHistoryFix,
  referenceTime = Date.now(),
): LocationHistoryFix[] {
  const sanitizedSample = sanitizeFix(sample);
  if (!sanitizedSample) {
    return pruneLocationHistory(previous, referenceTime);
  }

  const next = pruneLocationHistory(previous, referenceTime);
  const previousMarker = [...next].reverse().find(entry => entry.source === sample.source);

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
    }
    return next;
  }

  next.push(sanitizedSample);
  next.sort((a, b) => a.timestamp - b.timestamp);
  return next.slice(-LOCATION_HISTORY_MAX_SAMPLES);
}

export function locationForUnixMs(
  eventUnixMs: number,
  activeLocation: LocationHistoryFix | null,
  history: LocationHistoryFix[],
): LocationHistoryFix | null {
  if (activeLocation?.source === 'manual') {
    return activeLocation;
  }

  if (eventUnixMs < Date.now() - LOCATION_HISTORY_MAX_AGE_MS) {
    return null;
  }

  const candidates = pruneLocationHistory(
    activeLocation ? [...history, activeLocation] : history,
  ).filter(candidate => candidate.source === 'device');

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

export async function loadLocationHistory(): Promise<LocationHistoryFix[]> {
  try {
    const raw = await getStoredString(STORAGE_KEY);
    if (!raw) {
      return [];
    }
    const parsed = JSON.parse(raw);
    return pruneLocationHistory(Array.isArray(parsed) ? parsed : []);
  } catch {
    return [];
  }
}

export async function persistLocationHistory(history: LocationHistoryFix[]) {
  await setStoredString(
    STORAGE_KEY,
    JSON.stringify(pruneLocationHistory(history)),
  );
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
