import {NativeEventEmitter, NativeModules, Platform} from 'react-native';

import type {LocationHistoryFix} from './LocationHistoryStore';

type SpectreLocationRecorderModule = {
  start: () => Promise<boolean>;
  stop: () => Promise<boolean>;
};

const nativeModule =
  Platform.OS === 'android'
    ? (NativeModules.SpectreLocationRecorder as
        | SpectreLocationRecorderModule
        | undefined)
    : undefined;

const emitter = nativeModule
  ? new NativeEventEmitter(NativeModules.SpectreLocationRecorder)
  : null;

export const SPECTRE_LOCATION_RECORDER_AVAILABLE = !!nativeModule;

export async function startSpectreLocationRecorder(): Promise<boolean> {
  if (!nativeModule) return false;
  try {
    await nativeModule.start();
    return true;
  } catch {
    return false;
  }
}

export async function stopSpectreLocationRecorder(): Promise<boolean> {
  if (!nativeModule) return false;
  try {
    await nativeModule.stop();
    return true;
  } catch {
    return false;
  }
}

export type SpectreLocationFixListener = (fix: LocationHistoryFix) => void;

export function subscribeToSpectreLocationFixes(
  listener: SpectreLocationFixListener,
): () => void {
  if (!emitter) return () => undefined;
  const subscription = emitter.addListener(
    'SpectreLocationFix',
    (payload: any) => {
      if (!payload || typeof payload !== 'object') return;
      const lat = Number(payload.lat);
      const lon = Number(payload.lon);
      const timestamp = Number(payload.timestamp);
      if (!Number.isFinite(lat) || !Number.isFinite(lon) || !(timestamp > 0)) {
        return;
      }
      listener({
        lat,
        lon,
        alt: Number(payload.alt) || 0,
        accuracy: Math.max(0, Number(payload.accuracy) || 0),
        timestamp,
        source: 'device',
        provider:
          typeof payload.provider === 'string' ? payload.provider : 'fused',
      });
    },
  );
  return () => subscription.remove();
}
