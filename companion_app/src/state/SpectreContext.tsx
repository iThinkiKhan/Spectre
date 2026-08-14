/* eslint-disable no-bitwise */

import React, {createContext, useContext, useEffect, useMemo, useRef, useState} from 'react';
import {AppState, type AppStateStatus, Platform} from 'react-native';

import {
  encodeEnrichmentRecords,
  encodeEventBatchRecords,
  encodePhoneControlFrame,
  encodePhoneGpsFrame,
  eventTimestampToUnixMs,
  PHONE_CONTROL_FLAG_BATCH_RECEIVED,
  PHONE_CONTROL_FLAG_CANCEL,
  PHONE_CONTROL_FLAG_DUMP_REQUEST,
  PHONE_CONTROL_FLAG_WG_ACTIVE,
  PHONE_ENRICH_FLAG_NO_DATA,
  PHONE_ENRICH_FLAG_TAG_PRESENT,
  PHONE_GPS_FLAG_TRUSTED_TIME,
  PHONE_GPS_FLAG_VALID,
} from '../protocol/binary';
import {
  buildCompanionMetadata,
  ENRICHMENT_RECORD_SIZE,
  normalizeEnrichmentTag,
} from '../protocol/contracts';
import type {BadUsbScriptDraft, BadUsbUploadState} from '../protocol/badusb';
import {emptyBadUsbUploadState} from '../protocol/badusb';
import type {
  EnrichmentRecord,
  EventBatchRecord,
  PhoneNotificationV1,
} from '../protocol/types';
import {decodePhoneNotification} from '../protocol/binary';
import {BleClientService} from '../services/ble/BleClientService';
import type {
  SpectreConnectionState,
  SpectreDeviceSummary,
  SpectrePromptState,
} from '../services/ble/bleTypes';
import {
  friendlyPermissionName,
  readAndroidBlePermissions,
  requestAndroidBlePermissions,
  type AndroidBlePermissionState,
} from '../services/permissions/androidBlePermissions';
import {
  SpectrePeripheralBridge,
  type LocationFix,
  type PeripheralState,
  type PeripheralStorageSnapshot,
} from '../services/peripheral/SpectrePeripheralBridge';
import {SpectreCommandService} from '../services/peripheral/SpectreCommandService';
import {
  SpectreRelayService,
  type PhoneNetworkStatus,
  type RelayQueueStatus,
} from '../services/relay/SpectreRelayService';
import {
  buildLocalizationSnapshot,
  EMPTY_LOCALIZATION_SNAPSHOT,
  type LocalizationSnapshot,
} from '../services/localization/LocalizationService';
import {
  EnrichmentService,
  type FinalizedEventBatch,
} from '../services/enrichment/EnrichmentService';
import {
  buildLocationCandidates,
  flushLocationHistoryPersist,
  getCurrentDeviceLocationFix,
  loadLocationHistory,
  locationForUnixMsFromCandidates,
  nearestLocationDriftFromCandidates,
  rememberLocationSample,
  scheduleLocationHistoryPersist,
  type LocationCandidateSet,
  type LocationHistoryFix,
} from '../services/enrichment/LocationHistoryStore';
import {
  SPECTRE_LOCATION_RECORDER_AVAILABLE,
  startSpectreLocationRecorder,
  stopSpectreLocationRecorder,
  subscribeToSpectreLocationFixes,
} from '../services/enrichment/SpectreLocationRecorder';
import {
  getStoredString,
  setStoredString,
} from '../services/storage/NativeKeyValueStore';

export type TabKey = 'mission' | 'targets' | 'map';
type LocationMode = 'device' | 'manual' | 'off';
type LogLevel = 'info' | 'warn' | 'error';
type BatchStatus = 'waiting' | 'ready' | 'sent';
type ControlPulseKind = 'dump' | 'cancel' | 'batchAck';

export type LogEntry = {
  id: string;
  message: string;
  level: LogLevel;
  timestamp: number;
};

export type ActiveLocationFix = LocationHistoryFix;

export type ManualLocationDraft = {
  lat: string;
  lon: string;
  alt: string;
  accuracy: string;
};

export type EventBatchView = {
  id: string;
  events: EventBatchRecord[];
  receivedAt: number;
  source: 'spectre' | 'mock';
  status: BatchStatus;
  tag: string;
  note: string;
  location: ActiveLocationFix | null;
  lastPublishedAt: number | null;
};

export type BatchTransferSummary = {
  records: number;
  bytes: number;
  source: 'spectre' | 'mock';
  tag: string;
  sentAt: number;
};

type ControlPulse = {
  id: string;
  kind: ControlPulseKind;
  counter: number;
  expiresAt: number;
};

export type SpectreStatusSummary = {
  sessionId: string | null;
  backhaulStateLabel: string;
  gpsValid: boolean;
  inputState: string | null;
  wireGuardState: string | null;
  token: number | null;
};

export type FieldTransferState = {
  phase: 'idle' | 'copying' | 'relaying' | 'complete' | 'error';
  message: string;
  copied: number;
};

type SpectreContextValue = {
  activeTab: TabKey;
  setActiveTab: (tab: TabKey) => void;
  permissions: AndroidBlePermissionState;
  missingPermissionLabels: string[];
  adapterState: string;
  connectionState: SpectreConnectionState;
  connectionDetail: string;
  connectedDevice: SpectreDeviceSummary | null;
  discoveredDevices: SpectreDeviceSummary[];
  promptState: SpectrePromptState;
  statusSummary: SpectreStatusSummary;
  peripheralState: PeripheralState;
  storageSnapshot: PeripheralStorageSnapshot | null;
  relayStatus: RelayQueueStatus;
  networkStatus: PhoneNetworkStatus;
  fieldTransfer: FieldTransferState;
  localization: LocalizationSnapshot;
  selectedTargetId: string | null;
  setSelectedTargetId: (targetId: string | null) => void;
  refreshLocalization: () => Promise<void>;
  commandService: SpectreCommandService | null;
  notifications: PhoneNotificationV1[];
  clearNotifications: () => void;
  locationMode: LocationMode;
  nativeRecorderActive: boolean;
  gpsRecording: boolean;
  activeLocation: ActiveLocationFix | null;
  deviceLocation: ActiveLocationFix | null;
  manualLocationDraft: ManualLocationDraft;
  eventBatches: EventBatchView[];
  lastPublishedBatch: BatchTransferSummary | null;
  activeTag: string;
  autoApplyEnrichment: boolean;
  wireGuardActive: boolean;
  badUsbUploadState: BadUsbUploadState;
  logs: LogEntry[];
  requestPermissions: () => Promise<void>;
  startFieldMode: () => Promise<void>;
  stopFieldMode: () => Promise<void>;
  startBleLink: () => Promise<void>;
  stopBleLink: () => Promise<void>;
  ensureBleLink: () => Promise<void>;
  startGpsRecording: () => Promise<void>;
  stopGpsRecording: () => Promise<void>;
  scanForDevices: () => Promise<void>;
  connectToDevice: (deviceId: string) => Promise<void>;
  disconnect: () => Promise<void>;
  submitPromptReply: (text: string) => Promise<void>;
  uploadBadUsbScript: (draft: BadUsbScriptDraft) => Promise<void>;
  cancelBadUsbUpload: () => Promise<void>;
  setActiveTag: (value: string) => void;
  setAutoApplyEnrichment: (value: boolean) => void;
  setLocationMode: (mode: LocationMode) => void;
  updateManualLocationDraft: (
    patch: Partial<ManualLocationDraft>,
  ) => void;
  applyManualLocation: () => void;
  clearManualLocation: () => void;
  refreshDeviceLocation: () => Promise<void>;
  queueDumpRequest: () => void;
  queueCancelRequest: () => void;
  sendBatchNow: (batchId: string) => Promise<void>;
  offloadToPhone: (maxRecords?: number) => Promise<void>;
  relayHome: () => Promise<void>;
  injectMockBatch: () => void;
};

const SpectreContext = createContext<SpectreContextValue | null>(null);

const EMPTY_PERMISSIONS: AndroidBlePermissionState = {
  checked: Platform.OS !== 'android',
  allGranted: Platform.OS !== 'android',
  missing: [],
  statuses: {},
};

const EMPTY_PERIPHERAL_STATE: PeripheralState = {
  running: false,
  advertising: false,
  connectedDevices: 0,
  secureSessionReady: false,
  advertiseMode: null,
  advertiseStartConfirmed: false,
  watchdogActive: false,
  totalAdvertiseRestarts: 0,
  lastAdvertiseStartedAt: null,
  lastAdvertiseFailureCode: null,
  lastConnectedAt: null,
  lastDisconnectedAt: null,
  lastConnectedPeer: null,
  lastDisconnectedPeer: null,
  lastBatchReceivedAt: null,
  lastBatchPeer: null,
  lastBatchBytes: 0,
  lastBatchRecords: 0,
  lastStorageReceivedAt: null,
  lastStoragePeer: null,
  storageBase64: null,
  totalBatchesReceived: 0,
  totalBatchBytes: 0,
  totalBatchRecords: 0,
  error: null,
  moduleAvailable: Platform.OS === 'android',
};

const EMPTY_MANUAL_LOCATION: ManualLocationDraft = {
  lat: '',
  lon: '',
  alt: '0',
  accuracy: '10',
};

const EMPTY_RELAY_STATUS: RelayQueueStatus = {
  pending: 0,
  published: 0,
  pendingBytes: 0,
  running: false,
  publishedThisPass: 0,
  endpoint: '192.168.0.11:1883',
};

const EMPTY_NETWORK_STATUS: PhoneNetworkStatus = {
  vpnActive: false,
  vpnValidated: false,
  vpnInterface: '',
  cellularAvailable: false,
};

const MISSION_INTENT_KEY = 'spectre.mission.enabled';
const WIFI_BULK_ENRICH_THRESHOLD = 128;
// A Wi-Fi handoff includes a deliberate Spectre radio reboot. Below one full
// bulk batch, the authenticated BLE link is faster and avoids making a couple
// of newly captured observations bounce the device between radio modes.
const WIFI_BULK_OFFLOAD_THRESHOLD = 64;
const WIFI_BULK_TIMEOUT_MS = 10 * 60_000;

function nowId(prefix: string) {
  return `${prefix}-${Date.now()}-${Math.round(Math.random() * 1000)}`;
}

function swallowPromise(task: Promise<unknown> | null | undefined) {
  task?.catch(() => {});
}

function pause(ms: number) {
  return new Promise<void>(resolve => setTimeout(resolve, ms));
}

function appendUniqueLog(
  previous: LogEntry[],
  message: string,
  level: LogLevel,
): LogEntry[] {
  const nextEntry = {
    id: nowId('log'),
    message,
    level,
    timestamp: Date.now(),
  };
  return [nextEntry, ...previous].slice(0, 80);
}

function summarizeStatus(promptState: SpectrePromptState): SpectreStatusSummary {
  const stateLabel =
    promptState.parsedStatus.stateLabel ||
    promptState.parsedStatus.state ||
    'UNKNOWN';

  return {
    sessionId: promptState.parsedStatus.sess || null,
    backhaulStateLabel: stateLabel,
    gpsValid: promptState.parsedStatus.gps === '1',
    inputState: promptState.parsedStatus.input || null,
    wireGuardState: promptState.parsedStatus.wg || null,
    token: promptState.token,
  };
}

function locationFromNativeFix(fix: LocationFix): ActiveLocationFix {
  return {
    lat: fix.lat,
    lon: fix.lon,
    alt: fix.alt ?? 0,
    accuracy: fix.accuracy ?? 0,
    timestamp: fix.timestamp,
    source: 'device',
    provider: fix.provider ?? null,
  };
}

function locationForEvent(
  event: EventBatchRecord,
  candidates: LocationCandidateSet,
): ActiveLocationFix | null {
  if (candidates.manualOverride) {
    return candidates.manualOverride;
  }

  const eventUnixMs = eventTimestampToUnixMs(event.timestampMs);
  if (!eventUnixMs) {
    return null;
  }

  return locationForUnixMsFromCandidates(eventUnixMs, candidates);
}

function buildGpsBase64(location: ActiveLocationFix | null) {
  if (!location) {
    // Time-only frames keep capture timestamps enrichable without GPS lock.
    return encodePhoneGpsFrame({
      version: 1,
      latE7: 0,
      lonE7: 0,
      altCm: 0,
      accuracyDm: 0,
      epochUtc: Math.floor(Date.now() / 1000),
      flags: PHONE_GPS_FLAG_TRUSTED_TIME,
    });
  }

  return encodePhoneGpsFrame({
    version: 1,
    latE7: Math.round(location.lat * 10_000_000),
    lonE7: Math.round(location.lon * 10_000_000),
    altCm: Math.round(location.alt * 100),
    accuracyDm: Math.max(0, Math.round(location.accuracy * 10)),
    epochUtc: Math.max(0, Math.floor(location.timestamp / 1000)),
    flags: PHONE_GPS_FLAG_VALID | PHONE_GPS_FLAG_TRUSTED_TIME,
  });
}

function buildEnrichmentRecords(
  events: EventBatchRecord[],
  location: ActiveLocationFix | null,
  tag: string,
  locationHistory: ActiveLocationFix[],
): {
  records: EnrichmentRecord[];
  wireRecords: EnrichmentRecord[];
  normalizedTag: ReturnType<typeof normalizeEnrichmentTag>;
  skipped: number;
} {
  const normalizedTag = normalizeEnrichmentTag(tag);
  const records: EnrichmentRecord[] = [];
  const wireRecords: EnrichmentRecord[] = [];
  const candidates = buildLocationCandidates(location, locationHistory);

  events.forEach(event => {
    const eventLocation = locationForEvent(event, candidates);
    if (!eventLocation) {
      wireRecords.push({
        eventId: event.eventId,
        latE7: 0,
        lonE7: 0,
        altCm: 0,
        accuracyDm: 0,
        epochUtc: 0,
        flags: PHONE_ENRICH_FLAG_NO_DATA,
        tag: '',
      });
      return;
    }

    const record = {
      eventId: event.eventId,
      latE7: Math.round(eventLocation.lat * 10_000_000),
      lonE7: Math.round(eventLocation.lon * 10_000_000),
      altCm: Math.round(eventLocation.alt * 100),
      accuracyDm: Math.max(0, Math.round(eventLocation.accuracy * 10)),
      epochUtc: Math.max(0, Math.floor(eventLocation.timestamp / 1000)),
      flags: normalizedTag.value.length ? PHONE_ENRICH_FLAG_TAG_PRESENT : 0,
      tag: normalizedTag.value,
    };

    records.push(record);
    wireRecords.push(record);
  });

  return {
    records,
    wireRecords,
    normalizedTag,
    skipped: events.length - records.length,
  };
}

function controlPulseFlag(kind: ControlPulseKind) {
  switch (kind) {
    case 'dump':
      return PHONE_CONTROL_FLAG_DUMP_REQUEST;
    case 'cancel':
      return PHONE_CONTROL_FLAG_CANCEL;
    case 'batchAck':
      return PHONE_CONTROL_FLAG_BATCH_RECEIVED;
    default:
      return 0;
  }
}

function buildControlBase64(
  wireGuardActive: boolean,
  pulses: ControlPulse[],
  referenceTime: number,
) {
  let flags = wireGuardActive ? PHONE_CONTROL_FLAG_WG_ACTIVE : 0;
  let counter = 0;

  pulses.forEach(pulse => {
    if (pulse.expiresAt <= referenceTime) {
      return;
    }
    flags |= controlPulseFlag(pulse.kind);
    if (pulse.counter > counter) {
      counter = pulse.counter;
    }
  });

  return encodePhoneControlFrame({
    version: 1,
    flags,
    counter,
  });
}

function buildMetadata(
  locationMode: LocationMode,
  autoApplyEnrichment: boolean,
) {
  return buildCompanionMetadata({
    gpsMode: locationMode,
    autoApply: autoApplyEnrichment,
  }).value;
}

function demoBatch(): EventBatchRecord[] {
  const timestampMs = Date.now();
  return [
    {
      eventId: 4101,
      timestampMs,
      type: 1,
      status: 0,
      lane: 0,
      priority: 1,
    },
    {
      eventId: 4102,
      timestampMs: timestampMs - 12_000,
      type: 4,
      status: 0,
      lane: 0,
      priority: 1,
    },
    {
      eventId: 4103,
      timestampMs: timestampMs - 28_000,
      type: 3,
      status: 0,
      lane: 1,
      priority: 2,
    },
  ];
}

export function SpectreProvider({children}: {children: React.ReactNode}) {
  const bleRef = useRef<BleClientService | null>(null);
  const peripheralRef = useRef<SpectrePeripheralBridge | null>(null);
  const commandServiceRef = useRef<SpectreCommandService | null>(null);
  const relayRef = useRef<SpectreRelayService | null>(null);
  const enrichmentRef = useRef<EnrichmentService | null>(null);
  const activeTagRef = useRef('FIELD');
  const autoApplyRef = useRef(true);
  const activeLocationRef = useRef<ActiveLocationFix | null>(null);
  const locationHistoryRef = useRef<ActiveLocationFix[]>([]);
  const nextControlCounterRef = useRef(1);
  const startupConfigRef = useRef({
    metadata: '',
    gpsBase64: '',
    controlBase64: '',
  });
  const publishBatchRef = useRef<
    ((batchId: string, events: EventBatchRecord[], source: 'spectre' | 'mock') => Promise<void>) | null
  >(null);
  const handleIncomingBatchRef = useRef<
    ((payload: FinalizedEventBatch) => Promise<void>) | null
  >(null);
  const refreshDeviceLocationRef = useRef<(() => Promise<void>) | null>(null);
  const offloadToPhoneRef = useRef<((maxRecords?: number) => Promise<void>) | null>(null);
  const automaticEnrichKeyRef = useRef('');
  const automaticOffloadKeyRef = useRef('');
  const automaticRelayKeyRef = useRef('');
  const fieldStartInFlightRef = useRef(false);

  const [activeTab, setActiveTab] = useState<TabKey>('mission');
  const [permissions, setPermissions] =
    useState<AndroidBlePermissionState>(EMPTY_PERMISSIONS);
  const [adapterState, setAdapterState] = useState('Unknown');
  const [connectionState, setConnectionState] =
    useState<SpectreConnectionState>('idle');
  const [connectionDetail, setConnectionDetail] = useState('');
  const [connectedDevice, setConnectedDevice] =
    useState<SpectreDeviceSummary | null>(null);
  const [discoveredDevices, setDiscoveredDevices] = useState<
    SpectreDeviceSummary[]
  >([]);
  const [promptState, setPromptState] = useState<SpectrePromptState>({
    promptText: null,
    pending: false,
    awaitingReply: false,
    submittedReply: false,
    receipt: null,
    rawStatus: null,
    parsedStatus: {},
    token: null,
    promptKind: 'none',
    replyError: null,
    updatedAt: null,
  });
  const [peripheralState, setPeripheralState] = useState<PeripheralState>(
    EMPTY_PERIPHERAL_STATE,
  );
  const [storageSnapshot, setStorageSnapshot] =
    useState<PeripheralStorageSnapshot | null>(null);
  const [relayStatus, setRelayStatus] = useState(EMPTY_RELAY_STATUS);
  const [networkStatus, setNetworkStatus] = useState(EMPTY_NETWORK_STATUS);
  const [localization, setLocalization] = useState(
    EMPTY_LOCALIZATION_SNAPSHOT,
  );
  const [selectedTargetId, setSelectedTargetId] = useState<string | null>(null);
  const [fieldTransfer, setFieldTransfer] = useState<FieldTransferState>({
    phase: 'idle',
    message: 'Ready',
    copied: 0,
  });
  const [notifications, setNotifications] = useState<PhoneNotificationV1[]>([]);
  const [appState, setAppState] = useState<AppStateStatus>(
    AppState.currentState,
  );
  const [locationMode, setLocationMode] = useState<LocationMode>('device');
  const [deviceLocation, setDeviceLocation] =
    useState<ActiveLocationFix | null>(null);
  const [manualLocation, setManualLocation] =
    useState<ActiveLocationFix | null>(null);
  const [manualLocationDraft, setManualLocationDraft] =
    useState<ManualLocationDraft>(EMPTY_MANUAL_LOCATION);
  const [locationHistoryVersion, setLocationHistoryVersion] = useState(0);
  const [historyLoaded, setHistoryLoaded] = useState(false);
  const historyLoadedRef = useRef(false);
  // Native recorder owns phone GPS and bucket persistence while active.
  const [nativeRecorderActive, setNativeRecorderActive] = useState(false);
  const nativeRecorderActiveRef = useRef(false);
  // GPS recording intent is independent of the BLE link.
  const [gpsRecording, setGpsRecording] = useState(false);
  const gpsRecordingRef = useRef(false);
  const [missionIntentLoaded, setMissionIntentLoaded] = useState(false);
  const [missionDesired, setMissionDesired] = useState(false);
  const [eventBatches, setEventBatches] = useState<EventBatchView[]>([]);
  const [lastPublishedBatch, setLastPublishedBatch] =
    useState<BatchTransferSummary | null>(null);
  const [activeTag, setActiveTagState] = useState('FIELD');
  const [autoApplyEnrichment, setAutoApplyEnrichmentState] = useState(true);
  const [wireGuardActive, setWireGuardActiveState] = useState(false);
  const [badUsbUploadState, setBadUsbUploadState] =
    useState<BadUsbUploadState>(emptyBadUsbUploadState());
  const [controlPulses, setControlPulses] = useState<ControlPulse[]>([]);
  const [pulseClock, setPulseClock] = useState(Date.now());
  const [logs, setLogs] = useState<LogEntry[]>([]);

  const activeLocation = useMemo(
    () =>
      locationMode === 'manual'
        ? manualLocation
        : locationMode === 'device'
          ? deviceLocation
          : null,
    [locationMode, manualLocation, deviceLocation],
  );

  const controlBase64 = useMemo(
    () => buildControlBase64(wireGuardActive, controlPulses, pulseClock),
    [wireGuardActive, controlPulses, pulseClock],
  );
  const gpsBase64 = useMemo(() => buildGpsBase64(activeLocation), [activeLocation]);
  const metadata = useMemo(
    () => buildMetadata(locationMode, autoApplyEnrichment),
    [locationMode, autoApplyEnrichment],
  );
  const statusSummary = summarizeStatus(promptState);

  const missingPermissionLabels = permissions.missing.map(friendlyPermissionName);

  const appendLog = (message: string, level: LogLevel = 'info') => {
    setLogs(previous => appendUniqueLog(previous, message, level));
  };

  const refreshLocalization = async () => {
    const relay = relayRef.current;
    if (!relay) return;
    try {
      const records = await relay.recentRecords();
      const next = buildLocalizationSnapshot(records);
      setLocalization(next);
      setSelectedTargetId(previous =>
        previous && next.targets.some(target => target.id === previous)
          ? previous
          : next.targets[0]?.id ?? null,
      );
    } catch (error: any) {
      appendLog(error?.message || 'Could not read the phone target archive', 'warn');
    }
  };

  const refreshPermissions = async () => {
    const next = await readAndroidBlePermissions();
    setPermissions(next);
  };

  const queueControlPulse = (kind: ControlPulseKind) => {
    const counter = nextControlCounterRef.current;
    nextControlCounterRef.current += 1;
    setControlPulses(previous => [
      ...previous.filter(pulse => pulse.expiresAt > Date.now()),
      {
        id: nowId(kind),
        kind,
        counter,
        expiresAt:
          Date.now() +
          (kind === 'batchAck' ? 6000 : kind === 'cancel' ? 5000 : 8000),
      },
    ]);
  };

  const publishBatch = async (
    batchId: string,
    events: EventBatchRecord[],
    source: 'spectre' | 'mock',
  ) => {
    const bridge = peripheralRef.current;
    const location = activeLocationRef.current;
    let locationHistory = locationHistoryRef.current;

    if (!bridge) {
      appendLog('Peripheral bridge unavailable for enrichment send', 'error');
      return;
    }

    if (source === 'spectre') {
      try {
        const persistedHistory = await loadLocationHistory();
        if (persistedHistory.length > 0) {
          locationHistoryRef.current = persistedHistory;
          locationHistory = persistedHistory;
          setLocationHistoryVersion(previous => previous + 1);
        }
      } catch (error: any) {
        console.warn(
          `[SpectreEnrich] history refresh failed message=${error?.message ?? 'unknown'}`,
        );
      }
    }

    const {records, wireRecords, normalizedTag, skipped} = buildEnrichmentRecords(
      events,
      location,
      activeTagRef.current,
      locationHistory,
    );

    if (source === 'spectre') {
      const eventTimes = events
        .map(event => eventTimestampToUnixMs(event.timestampMs))
        .filter((value): value is number => !!value);
      const candidates = buildLocationCandidates(location, locationHistory);
      const driftSummaries = eventTimes
        .map(eventTime => nearestLocationDriftFromCandidates(eventTime, candidates))
        .filter((value): value is NonNullable<typeof value> => !!value);
      const nearestDrifts = driftSummaries.map(summary => summary.nearestDriftMs);
      const historyTimes = locationHistory
        .map(fix => fix.timestamp)
        .filter(value => Number.isFinite(value) && value > 0);
      const firstEvent = eventTimes.length > 0 ? Math.min(...eventTimes) : 0;
      const lastEvent = eventTimes.length > 0 ? Math.max(...eventTimes) : 0;
      const oldestHistory = historyTimes.length > 0 ? Math.min(...historyTimes) : 0;
      const newestHistory = historyTimes.length > 0 ? Math.max(...historyTimes) : 0;
      const nearestDriftMin =
        nearestDrifts.length > 0 ? Math.min(...nearestDrifts) : -1;
      const nearestDriftMax =
        nearestDrifts.length > 0 ? Math.max(...nearestDrifts) : -1;
      const nearestFixFirst =
        driftSummaries.length > 0 ? driftSummaries[0].nearestTimestamp : 0;
      console.info(
        `[SpectreEnrich] batch events=${events.length} matched=${records.length} skipped=${skipped} history=${locationHistory.length} firstEvent=${firstEvent} lastEvent=${lastEvent} oldestHistory=${oldestHistory} newestHistory=${newestHistory} nearestDriftMin=${nearestDriftMin} nearestDriftMax=${nearestDriftMax} nearestFixFirst=${nearestFixFirst} nativeRecorder=${nativeRecorderActiveRef.current ? 1 : 0}`,
      );
    }

    const payload = encodeEnrichmentRecords(wireRecords);

    try {
      await bridge.updateEnrichmentValue(payload, true);
      const publishedAt = Date.now();
      const publishedNote =
        source === 'mock'
          ? 'Mock enrichment published to phone peripheral'
          : records.length === 0
            ? 'No UTC-correlated GPS samples; records marked no-data'
            : skipped > 0
              ? `Enrichment payload published; ${skipped} events need a closer GPS sample`
              : 'Enrichment payload published for Spectre pickup';

      setEventBatches(previous =>
        previous.map(batch =>
          batch.id === batchId
            ? {
                ...batch,
                status: 'sent',
                tag: normalizedTag.value,
                location,
                lastPublishedAt: publishedAt,
                note: publishedNote,
              }
            : batch,
        ),
      );
      setLastPublishedBatch({
        records: records.length,
        bytes: wireRecords.length * ENRICHMENT_RECORD_SIZE,
        source,
        tag: normalizedTag.value,
        sentAt: publishedAt,
      });
      appendLog(
        records.length > 0
          ? `Published enrichment batch (${records.length} records)`
          : 'Published terminal enrichment no-data markers',
      );
      if (skipped > 0) {
        appendLog(
          `Skipped ${skipped} events without a nearby phone GPS sample`,
          'warn',
        );
      }
      if (normalizedTag.truncated) {
        appendLog('Enrichment tag was trimmed to fit the device contract', 'warn');
      }
    } catch (error: any) {
      if (source === 'spectre') {
        queueControlPulse('cancel');
      }
      setEventBatches(previous =>
        previous.map(batch =>
          batch.id === batchId
            ? {
                ...batch,
                status: 'ready',
                note: error?.message || 'Failed to publish enrichment payload',
              }
            : batch,
        ),
      );
      appendLog(error?.message || 'Failed to publish enrichment payload', 'error');
    }
  };

  const handleIncomingBatch = async (
    payload: FinalizedEventBatch,
  ) => {
    const batchId = nowId(payload.source === 'mock' ? 'mock-batch' : 'batch');
    const nextStatus: BatchStatus = autoApplyRef.current ? 'ready' : 'waiting';

    setEventBatches(previous => [
      {
        id: batchId,
        events: payload.events,
        receivedAt: payload.receivedAt,
        source: payload.source,
        status: nextStatus,
        tag: normalizeEnrichmentTag(activeTagRef.current).value,
        note:
          payload.source === 'mock'
            ? 'Mock batch injected for workflow testing'
            : 'Batch received from Spectre peripheral link',
        location: activeLocationRef.current,
        lastPublishedAt: null,
      },
      ...previous.filter(batch =>
        payload.source !== 'spectre'
          ? true
          : !(batch.source === 'spectre' && batch.status !== 'sent'),
      ),
    ].slice(0, 12));

    queueControlPulse('batchAck');
    appendLog(
      `${payload.source === 'mock' ? 'Mock' : 'Spectre'} batch received (${payload.events.length} records)`,
    );

    if (autoApplyRef.current) {
      swallowPromise(publishBatch(batchId, payload.events, payload.source));
    }
  };

  const refreshDeviceLocation = async () => {
    if (locationMode !== 'device') {
      return;
    }

    if (!gpsRecordingRef.current) {
      return;
    }

    // Persisted history must be loaded before we accept any new samples; a
    // sample taken against the empty initial ref would, on the next flush,
    // make the chunked store delete every migrated day-bucket.
    if (!historyLoaded) {
      return;
    }

    const fix =
      (await peripheralRef.current?.getLastKnownLocation()) ??
      (await getCurrentDeviceLocationFix());
    if (!fix) {
      appendLog('No recent Android location fix available', 'warn');
      return;
    }

    const nextLocation = locationFromNativeFix(fix);
    locationHistoryRef.current = rememberLocationSample(
      locationHistoryRef.current,
      nextLocation,
    );
    setLocationHistoryVersion(previous => previous + 1);
    if (!nativeRecorderActiveRef.current) {
      scheduleLocationHistoryPersist(locationHistoryRef.current);
    }
    setDeviceLocation(nextLocation);
    appendLog(
      `Location updated from phone (${nextLocation.lat.toFixed(5)}, ${nextLocation.lon.toFixed(5)})`,
    );
  };

  const ensureFieldPermissions = async (): Promise<boolean> => {
    const currentPermissions = permissions.allGranted
      ? permissions
      : await readAndroidBlePermissions();

    if (!permissions.allGranted) {
      setPermissions(currentPermissions);
    }

    if (!currentPermissions.allGranted) {
      const missingLabels = currentPermissions.missing.map(friendlyPermissionName);
      appendLog(
        missingLabels.length
          ? `Grant Android permissions first: ${missingLabels.join(', ')}`
          : 'Grant Android Bluetooth/location permissions first',
        'warn',
      );
      return false;
    }
    return true;
  };

  const startBleLink = async () => {
    if (!(await ensureFieldPermissions())) {
      return;
    }

    const bridge = peripheralRef.current;
    if (!bridge) {
      appendLog('Peripheral bridge unavailable for BLE link', 'error');
      return;
    }

    try {
      const state = await bridge.start({
        metadata: startupConfigRef.current.metadata,
        gpsBase64: startupConfigRef.current.gpsBase64,
        controlBase64: startupConfigRef.current.controlBase64,
        enrichmentBase64: '',
        advertiseMode: 'uuidOnly',
        useDeviceLocation: gpsRecordingRef.current,
      });
      setPeripheralState(state);
      appendLog('BLE link started: advertising active');
    } catch (error: any) {
      appendLog(error?.message || 'Failed to start BLE link', 'error');
      throw error;
    }
  };

  const stopBleLink = async () => {
    const bridge = peripheralRef.current;
    if (!bridge) {
      return;
    }

    try {
      const state = await bridge.stop();
      setPeripheralState(state);
      appendLog('BLE link stopped');
    } catch (error: any) {
      appendLog(error?.message || 'Failed to stop BLE link', 'error');
      throw error;
    }
  };

  const ensureBleLink = async () => {
    if (peripheralState.running) {
      return;
    }
    await startBleLink();
  };

  const startGpsRecording = async () => {
    if (!(await ensureFieldPermissions())) {
      return;
    }
    if (locationMode !== 'device') {
      setLocationMode('device');
    }
    setGpsRecording(true);
  };

  const stopGpsRecording = async () => {
    setGpsRecording(false);
  };

  const startFieldMode = async () => {
    setMissionDesired(true);
    await setStoredString(MISSION_INTENT_KEY, '1');
    await startGpsRecording();
    await startBleLink();
  };

  const stopFieldMode = async () => {
    setMissionDesired(false);
    await setStoredString(MISSION_INTENT_KEY, '0');
    await stopGpsRecording();
    await stopBleLink();
  };

  startupConfigRef.current = {
    metadata,
    gpsBase64,
    controlBase64,
  };
  publishBatchRef.current = publishBatch;
  handleIncomingBatchRef.current = handleIncomingBatch;
  refreshDeviceLocationRef.current = refreshDeviceLocation;

  useEffect(() => {
    gpsRecordingRef.current = gpsRecording;
  }, [gpsRecording]);

  useEffect(() => {
    activeTagRef.current = activeTag;
  }, [activeTag]);

  useEffect(() => {
    autoApplyRef.current = autoApplyEnrichment;
  }, [autoApplyEnrichment]);

  useEffect(() => {
    activeLocationRef.current = activeLocation;
  }, [activeLocation]);

  useEffect(() => {
    let cancelled = false;
    swallowPromise(
      loadLocationHistory().then(history => {
        if (cancelled) {
          return;
        }
        locationHistoryRef.current = history;
        setLocationHistoryVersion(previous => previous + 1);
        historyLoadedRef.current = true;
        setHistoryLoaded(true);
        if (history.length > 0) {
          appendLog(`Loaded ${history.length} phone GPS history markers`);
        }
      }),
    );

    return () => {
      cancelled = true;
    };
  }, []);

  useEffect(() => {
    let cancelled = false;
    swallowPromise(
      getStoredString(MISSION_INTENT_KEY).then(stored => {
        if (cancelled) {
          return;
        }
        // Spectre is a field instrument: on first launch, collection is on.
        // An explicit Stop Mission is persisted and always wins thereafter.
        setMissionDesired(stored !== '0');
        setMissionIntentLoaded(true);
      }),
    );
    return () => {
      cancelled = true;
    };
  }, []);

  useEffect(() => {
    if (!SPECTRE_LOCATION_RECORDER_AVAILABLE) {
      return;
    }
    const unsubscribe = subscribeToSpectreLocationFixes(fix => {
      if (!nativeRecorderActiveRef.current) {
        return;
      }
      locationHistoryRef.current = rememberLocationSample(
        locationHistoryRef.current,
        fix,
      );
      setLocationHistoryVersion(previous => previous + 1);
      setDeviceLocation(fix);
    });
    return unsubscribe;
  }, []);

  useEffect(() => {
    if (!SPECTRE_LOCATION_RECORDER_AVAILABLE) {
      return;
    }
    const shouldRun = gpsRecording && locationMode === 'device';
    if (nativeRecorderActiveRef.current === shouldRun) {
      return;
    }

    const applyRecorderState = (active: boolean) => {
      nativeRecorderActiveRef.current = active;
      setNativeRecorderActive(active);
    };

    swallowPromise(
      (shouldRun
        ? startSpectreLocationRecorder()
        : stopSpectreLocationRecorder()
      ).then(updated => {
        if (!updated && shouldRun) {
          appendLog('Phone GPS recorder could not start', 'warn');
          return;
        }
        applyRecorderState(shouldRun);
        appendLog(
          shouldRun
            ? 'Phone GPS recorder active'
            : 'Phone GPS recorder stopped',
        );
        if (shouldRun) {
          swallowPromise(refreshDeviceLocationRef.current?.());
        }
      }),
    );
  }, [gpsRecording, locationMode]);

  // Tick only while a control pulse is in flight.
  useEffect(() => {
    if (controlPulses.length === 0) {
      return;
    }
    const timer = setInterval(() => {
      setPulseClock(Date.now());
      setControlPulses(previous =>
        previous.filter(pulse => pulse.expiresAt > Date.now()),
      );
    }, 1000);

    return () => {
      clearInterval(timer);
    };
  }, [controlPulses.length]);

  useEffect(() => {
    swallowPromise(peripheralRef.current?.updateControlValue(controlBase64));
  }, [controlBase64]);

  useEffect(() => {
    if (!peripheralState.running) {
      return;
    }
    swallowPromise(peripheralRef.current?.updateGpsValue(gpsBase64));
  }, [gpsBase64, peripheralState.running]);

  // Keep time-only GPS frames fresh while the phone lacks a live fix.
  useEffect(() => {
    if (!peripheralState.running || activeLocation) {
      return;
    }
    const timer = setInterval(() => {
      swallowPromise(peripheralRef.current?.updateGpsValue(buildGpsBase64(null)));
    }, 15000);
    return () => clearInterval(timer);
  }, [peripheralState.running, activeLocation]);

  // Refresh UTC immediately when a brief enrich probe becomes secure.
  useEffect(() => {
    if (!peripheralState.secureSessionReady) {
      return;
    }
    swallowPromise(peripheralRef.current?.updateGpsValue(buildGpsBase64(activeLocation)));
  }, [peripheralState.secureSessionReady, activeLocation]);

  useEffect(() => {
    swallowPromise(peripheralRef.current?.updateMetadata(metadata));
  }, [metadata]);

  useEffect(() => {
    bleRef.current = new BleClientService({
      onAdapterState: state => {
        setAdapterState(state);
      },
      onScanUpdate: devices => {
        setDiscoveredDevices(devices);
      },
      onConnectionChange: (state, detail, device) => {
        setConnectionState(state);
        setConnectionDetail(detail || '');
        setConnectedDevice(device ?? null);
        if (state === 'connected') {
          swallowPromise(
            peripheralRef.current?.kickAdvertising('text_link_connected'),
          );
        }
        if (detail) {
          appendLog(detail, state === 'error' ? 'error' : 'info');
        }
      },
      onPromptChange: nextPrompt => {
        setPromptState(nextPrompt);
      },
      onBadUsbUploadChange: nextUploadState => {
        setBadUsbUploadState(nextUploadState);
      },
      onLog: message => {
        appendLog(message);
      },
    });

    enrichmentRef.current = new EnrichmentService({
      onBatchReady: batch => {
        swallowPromise(handleIncomingBatchRef.current?.(batch));
      },
      onLog: (message, level = 'warn') => {
        appendLog(message, level);
      },
    });

    peripheralRef.current = new SpectrePeripheralBridge();
    commandServiceRef.current = new SpectreCommandService(peripheralRef.current);
    relayRef.current = new SpectreRelayService();
    peripheralRef.current.setListener({
      onStateChange: state => {
        setPeripheralState(state);
      },
      onEventBatchReceived: payload => {
        const source = payload.mock ? 'mock' : 'spectre';
        enrichmentRef.current?.ingest(payload, source);
      },
      onStorageSnapshotReceived: snapshot => {
        setStorageSnapshot(snapshot);
      },
      onCommandResponseReceived: event => {
        commandServiceRef.current?.handleResponseEvent(event);
      },
      onLogStreamChunkReceived: event => {
        commandServiceRef.current?.handleLogStreamEvent(event);
      },
      onDashboardStreamChunkReceived: event => {
        commandServiceRef.current?.handleDashboardStreamEvent(event);
      },
      onNotificationReceived: event => {
        try {
          const notif = decodePhoneNotification(event.base64);
          setNotifications(prev => {
            if (
              prev.some(
                existing => existing.seq === notif.seq && existing.type === notif.type,
              )
            ) {
              return prev;
            }
            const next = [notif, ...prev];
            return next.length > 50 ? next.slice(0, 50) : next;
          });
        } catch (error: any) {
          appendLog(`Notification decode failed: ${error?.message ?? error}`, 'warn');
        }
      },
      onLog: message => {
        appendLog(message);
      },
    });

    swallowPromise(bleRef.current.initialize());
    swallowPromise(refreshPermissions());
    swallowPromise(
      relayRef.current.status().then(status => setRelayStatus(status)),
    );
    swallowPromise(
      relayRef.current.networkStatus().then(status => {
        setNetworkStatus(status);
        setWireGuardActiveState(status.vpnValidated);
      }),
    );
    swallowPromise(refreshLocalization());

    return () => {
      const ble = bleRef.current;
      const peripheral = peripheralRef.current;
      const enrichment = enrichmentRef.current;
      const commandService = commandServiceRef.current;

      bleRef.current = null;
      peripheralRef.current = null;
      enrichmentRef.current = null;
      commandServiceRef.current = null;
      relayRef.current = null;

      commandService?.cancelAllPending('Bridge teardown');
      ble?.destroy();
      enrichment?.destroy();
      peripheral?.destroy();
    };
  }, []);

  // Restore the operator's mission intent after a process/UI restart. Native
  // foreground services are intentionally stopped only by Stop Mission.
  useEffect(() => {
    if (
      !missionIntentLoaded ||
      !missionDesired ||
      !permissions.allGranted ||
      !peripheralRef.current ||
      (gpsRecording && peripheralState.running) ||
      fieldStartInFlightRef.current
    ) {
      return;
    }

    fieldStartInFlightRef.current = true;
    swallowPromise(
      (async () => {
        if (!gpsRecording) {
          await startGpsRecording();
        }
        if (!peripheralState.running) {
          await startBleLink();
        }
      })().finally(() => {
        fieldStartInFlightRef.current = false;
      }),
    );
  }, [
    gpsRecording,
    missionDesired,
    missionIntentLoaded,
    peripheralState.running,
    permissions.allGranted,
  ]);

  useEffect(() => {
    const subscription = AppState.addEventListener('change', next => {
      setAppState(next);
      if (
        next !== 'active' &&
        historyLoadedRef.current &&
        !nativeRecorderActiveRef.current
      ) {
        swallowPromise(flushLocationHistoryPersist());
      }
    });
    return () => {
      subscription.remove();
      if (historyLoadedRef.current && !nativeRecorderActiveRef.current) {
        swallowPromise(flushLocationHistoryPersist());
      }
    };
  }, []);

  useEffect(() => {
    if (appState !== 'active') return;
    const refreshNetwork = () => {
      swallowPromise(
        relayRef.current?.networkStatus().then(status => {
          setNetworkStatus(status);
          setWireGuardActiveState(status.vpnValidated);
        }),
      );
    };
    refreshNetwork();
    swallowPromise(refreshLocalization());
    const timer = setInterval(refreshNetwork, 10_000);
    return () => clearInterval(timer);
  }, [appState]);

  // JS GPS polling is foreground fallback only; native service owns field runs.
  useEffect(() => {
    if (nativeRecorderActive) {
      return;
    }

    const sessionActive =
      permissions.allGranted &&
      gpsRecording &&
      locationMode === 'device' &&
      appState === 'active' &&
      historyLoaded;

    if (!sessionActive) {
      if (permissions.allGranted && gpsRecording && locationMode === 'device') {
        const reason =
          appState !== 'active' ? `app ${appState}` : 'idle';
        appendLog(`Phone GPS poll paused (${reason})`);
      }
      return;
    }

    appendLog('Phone GPS poll active (10s cadence, JS fallback)');
    swallowPromise(refreshDeviceLocationRef.current?.());
    const timer = setInterval(() => {
      swallowPromise(refreshDeviceLocationRef.current?.());
    }, 10_000);

    return () => {
      clearInterval(timer);
    };
  }, [
    nativeRecorderActive,
    permissions.allGranted,
    gpsRecording,
    locationMode,
    appState,
    historyLoaded,
  ]);

  useEffect(() => {
    if (!autoApplyEnrichment) {
      return;
    }

    const nextWaitingBatch = eventBatches.find(batch => batch.status === 'waiting');
    if (!nextWaitingBatch) {
      return;
    }

    swallowPromise(
      publishBatchRef.current?.(
        nextWaitingBatch.id,
        nextWaitingBatch.events,
        nextWaitingBatch.source,
      ),
    );
  }, [activeLocation, autoApplyEnrichment, eventBatches, locationHistoryVersion]);

  const value: SpectreContextValue = {
    activeTab,
    setActiveTab,
    permissions,
    missingPermissionLabels,
    adapterState,
    connectionState,
    connectionDetail,
    connectedDevice,
    discoveredDevices,
    promptState,
    statusSummary,
    peripheralState,
    storageSnapshot,
    relayStatus,
    networkStatus,
    fieldTransfer,
    localization,
    selectedTargetId,
    setSelectedTargetId,
    refreshLocalization,
    commandService: commandServiceRef.current,
    notifications,
    clearNotifications: () => setNotifications([]),
    locationMode,
    nativeRecorderActive,
    gpsRecording,
    activeLocation,
    deviceLocation,
    manualLocationDraft,
    eventBatches,
    lastPublishedBatch,
    activeTag,
    autoApplyEnrichment,
    wireGuardActive,
    badUsbUploadState,
    logs,
    requestPermissions: async () => {
      const next = await requestAndroidBlePermissions();
      setPermissions(next);
      if (!next.allGranted) {
        appendLog('Some Android Bluetooth permissions are still missing', 'warn');
      } else {
        appendLog('Bluetooth, advertise, notification, and location permissions granted');
      }
    },
    startFieldMode,
    stopFieldMode,
    startBleLink,
    stopBleLink,
    ensureBleLink,
    startGpsRecording,
    stopGpsRecording,
    scanForDevices: async () => {
      try {
        await bleRef.current?.scanForSpectre();
      } catch (error: any) {
        appendLog(error?.message || 'Spectre scan failed', 'error');
      }
    },
    connectToDevice: async (deviceId: string) => {
      try {
        await bleRef.current?.connectToDevice(deviceId);
      } catch (error: any) {
        appendLog(error?.message || 'Spectre connect failed', 'error');
      }
    },
    disconnect: async () => {
      await bleRef.current?.disconnect();
    },
    submitPromptReply: async (text: string) => {
      try {
        await bleRef.current?.submitPromptReply(text);
        appendLog('Prompt response sent to Spectre');
      } catch (error: any) {
        appendLog(error?.message || 'Prompt response failed', 'error');
        throw error;
      }
    },
    uploadBadUsbScript: async draft => {
      try {
        await bleRef.current?.uploadBadUsbScript(draft);
      } catch (error: any) {
        appendLog(error?.message || 'BadUSB upload failed', 'error');
        throw error;
      }
    },
    cancelBadUsbUpload: async () => {
      try {
        await bleRef.current?.cancelBadUsbUpload();
        appendLog('BadUSB upload cancel sent');
      } catch (error: any) {
        appendLog(error?.message || 'Failed to cancel BadUSB upload', 'error');
        throw error;
      }
    },
    setActiveTag: nextValue => {
      setActiveTagState(nextValue);
    },
    setAutoApplyEnrichment: nextValue => {
      setAutoApplyEnrichmentState(nextValue);
    },
    setLocationMode: mode => {
      setLocationMode(mode);
    },
    updateManualLocationDraft: patch => {
      setManualLocationDraft(previous => ({
        ...previous,
        ...patch,
      }));
    },
    applyManualLocation: () => {
      const lat = Number(manualLocationDraft.lat);
      const lon = Number(manualLocationDraft.lon);
      const alt = Number(manualLocationDraft.alt);
      const accuracy = Number(manualLocationDraft.accuracy);

      if (!Number.isFinite(lat) || !Number.isFinite(lon)) {
        appendLog('Manual location needs valid latitude and longitude', 'error');
        return;
      }

      const nextLocation: ActiveLocationFix = {
        lat,
        lon,
        alt: Number.isFinite(alt) ? alt : 0,
        accuracy: Number.isFinite(accuracy) ? accuracy : 10,
        timestamp: Date.now(),
        source: 'manual',
        provider: 'manual',
      };

      setManualLocation(nextLocation);
      setLocationMode('manual');
      appendLog(
        `Manual field fix armed (${lat.toFixed(5)}, ${lon.toFixed(5)})`,
      );
    },
    clearManualLocation: () => {
      setManualLocation(null);
      appendLog('Manual location cleared');
    },
    refreshDeviceLocation,
    queueDumpRequest: () => {
      queueControlPulse('dump');
      appendLog('Queued WireGuard dump request');
    },
    queueCancelRequest: () => {
      queueControlPulse('cancel');
      appendLog('Queued companion cancel pulse');
    },
    sendBatchNow: async batchId => {
      const batch = eventBatches.find(entry => entry.id === batchId);
      if (!batch) {
        return;
      }
      await publishBatch(batch.id, batch.events, batch.source);
    },
    offloadToPhone: async maxRecords => {
      const command = commandServiceRef.current;
      const relay = relayRef.current;
      if (!command || !relay || !peripheralState.secureSessionReady) {
        const message = 'Secure Spectre companion session is not ready';
        setFieldTransfer({phase: 'error', message, copied: 0});
        appendLog(message, 'warn');
        throw new Error(message);
      }
      setFieldTransfer({
        phase: 'copying',
        message: maxRecords
          ? `Copying up to ${maxRecords} records into the phone durable queue`
          : 'Copying all available records into the phone durable queue',
        copied: 0,
      });
      try {
        let copied = 0;
        let pendingAfter = Number.MAX_SAFE_INTEGER;
        let indexTruncated = false;
        let usedWifiBulk = false;

        const pendingForTransport = storageSnapshot
          ? storageSnapshot.pendingUploadMission + storageSnapshot.pendingUploadNoise
          : 0;
        if (
          !maxRecords &&
          Platform.OS === 'android' &&
          pendingForTransport >= WIFI_BULK_OFFLOAD_THRESHOLD
        ) {
          let receiverStarted = false;
          let deviceAccepted = false;
          try {
            const endpoint = await relay.startBulkReceiver();
            receiverStarted = true;
            const begin = await command.startWifiOffload(endpoint);
            deviceAccepted = begin.started;
            pendingAfter = begin.pendingAtStart;
            if (!begin.started) {
              usedWifiBulk = true;
              pendingAfter = 0;
            }
            const deadline = Date.now() + WIFI_BULK_TIMEOUT_MS;
            let lastProgressAt = 0;
            while (begin.started && Date.now() < deadline) {
              const bulk = await relay.bulkStatus();
              if (bulk.phase === 'error') {
                throw new Error(bulk.error || 'Phone Wi-Fi receiver failed');
              }
              copied = bulk.copied;
              pendingAfter = Math.max(0, begin.pendingAtStart - copied);
              const now = Date.now();
              if (now - lastProgressAt >= 500 || bulk.phase === 'complete') {
                const seconds = Math.max(0.001, bulk.elapsedMs / 1000);
                const rate = copied / seconds;
                setFieldTransfer({
                  phase: 'copying',
                  message: `Wi-Fi copied ${copied}; ${pendingAfter} remain on Spectre (${rate.toFixed(0)}/s)`,
                  copied,
                });
                lastProgressAt = now;
              }
              if (bulk.phase === 'complete') {
                usedWifiBulk = true;
                indexTruncated = pendingAfter > 0;
                break;
              }
              await pause(250);
            }
            if (!usedWifiBulk) {
              throw new Error('Phone Wi-Fi handoff timed out');
            }
          } catch (bulkError: any) {
            // Once Spectre accepts the endpoint it releases BLE and owns the
            // Wi-Fi radio. Preserve its records and surface that failure; a
            // BLE fallback is only safe before the device changes transports.
            if (deviceAccepted) throw bulkError;
            appendLog(
              `Fast Wi-Fi handoff unavailable; using BLE (${bulkError?.message ?? 'startup failed'})`,
              'warn',
            );
          } finally {
            if (receiverStarted) {
              await relay.stopBulkReceiver().catch(() => {});
            }
          }
        }

        if (!usedWifiBulk) {
          do {
            const summary = await command.offloadToPhone({
              commit: record => relay.enqueue(record),
              maxRecords,
              onProgress: (passCopied, passPending) => {
                setFieldTransfer({
                  phase: 'copying',
                  message: `Drained ${copied + passCopied}; ${passPending} remain on Spectre`,
                  copied: copied + passCopied,
                });
              },
            });
            copied += summary.copied;
            pendingAfter = summary.pendingAfter;
            indexTruncated = summary.indexTruncated;
          } while (!maxRecords && indexTruncated && pendingAfter > 0);
        }

        let status = await relay.status();
        setRelayStatus(status);
        const network = await relay.networkStatus();
        setNetworkStatus(network);
        setWireGuardActiveState(network.vpnValidated);
        let message = maxRecords && indexTruncated && pendingAfter > 0
          ? `Diagnostic copy drained ${copied}; ${pendingAfter} remain on Spectre`
          : pendingAfter > 0
            ? `Fast handoff saved ${copied} records; ${pendingAfter} remain for the next automatic window`
            : `Drained ${copied} records safely to the phone`;

        if (!maxRecords) {
          // The phone relay no longer needs BLE. Release the persistent device
          // link immediately so Spectre can resume radio capture while Android
          // forwards its durable queue over cellular/WireGuard.
          queueControlPulse('cancel');
          appendLog('Spectre drain committed; releasing the transfer link for capture');
        }

        if (!maxRecords && network.vpnValidated && status.pending > 0) {
          setFieldTransfer({
            phase: 'relaying',
            message: `${pendingAfter > 0 ? 'Handoff window saved' : 'Spectre is clear'}; relaying ${status.pending} phone records home`,
            copied,
          });
          try {
            status = await relay.relayHome();
            setRelayStatus(status);
            message = `Drained ${copied}; home acknowledged ${status.publishedThisPass}; ${status.pending} remain on phone`;
            if (pendingAfter > 0) message += `; ${pendingAfter} remain on Spectre`;
          } catch (relayError: any) {
            status = await relay.status();
            setRelayStatus(status);
            message = `${pendingAfter > 0 ? 'Handoff window is saved' : 'Spectre is clear'}; ${status.pending} records remain safely on phone (${relayError?.message ?? 'home relay unavailable'})`;
            appendLog(message, 'warn');
          }
        } else if (!maxRecords && status.pending > 0) {
          message += `; ${status.pending} await a phone VPN route`;
        }

        setFieldTransfer({phase: 'complete', message, copied});
        appendLog(message);
        await refreshLocalization();
      } catch (error: any) {
        const message = error?.message || 'Phone offload failed';
        setFieldTransfer({phase: 'error', message, copied: 0});
        appendLog(message, 'error');
        throw error;
      }
    },
    relayHome: async () => {
      const relay = relayRef.current;
      if (!relay) throw new Error('Native relay is unavailable');
      setFieldTransfer({
        phase: 'relaying',
        message: 'Relaying durable queue to home over the active phone route',
        copied: fieldTransfer.copied,
      });
      try {
        const status = await relay.relayHome();
        setRelayStatus(status);
        const message = `Home acknowledged ${status.publishedThisPass}; ${status.pending} remain queued`;
        setFieldTransfer({phase: 'complete', message, copied: fieldTransfer.copied});
        appendLog(message);
      } catch (error: any) {
        const message = error?.message || 'Home relay failed';
        setFieldTransfer({phase: 'error', message, copied: fieldTransfer.copied});
        appendLog(message, 'error');
        throw error;
      }
    },
    injectMockBatch: () => {
      const payload = encodeEventBatchRecords(demoBatch());
      peripheralRef.current?.emitMockEventBatch(payload);
    },
  };

  offloadToPhoneRef.current = value.offloadToPhone;

  // A secure visit must resolve GPS correlation before any observation is
  // acknowledged off Spectre. Once an event crosses the device upload
  // watermark, a later enrichment delta cannot be emitted as a normal replay,
  // so "save first, locate later" silently creates permanently unmapped data.
  // Request one enrichment pass per authenticated visit; the normal batch
  // listener above publishes GPS matches, and the refreshed storage snapshot
  // then unlocks automatic offload below.
  useEffect(() => {
    const pendingEnrichment = storageSnapshot
      ? storageSnapshot.pendingEnrichMission + storageSnapshot.pendingEnrichNoise
      : 0;
    const visitMarker =
      peripheralState.lastConnectedAt ?? storageSnapshot?.receivedAt ?? 0;
    const key = `${visitMarker}:enrich`;
    if (
      !gpsRecording ||
      !peripheralState.secureSessionReady ||
      pendingEnrichment <= 0 ||
      pendingEnrichment >= WIFI_BULK_ENRICH_THRESHOLD ||
      fieldTransfer.phase === 'copying' ||
      fieldTransfer.phase === 'relaying' ||
      automaticEnrichKeyRef.current === key
    ) {
      return;
    }

    const command = commandServiceRef.current;
    if (!command) return;

    automaticEnrichKeyRef.current = key;
    setFieldTransfer(previous => ({
      phase: 'idle',
      message: `Locating ${pendingEnrichment} Spectre observations before safe handoff`,
      copied: previous.copied,
    }));
    swallowPromise(
      command.enrichNow().then(
        () => appendLog(`Automatic localization requested for ${pendingEnrichment} observations`),
        (error: any) => {
          appendLog(error?.message || 'Automatic localization request failed', 'warn');
        },
      ),
    );
  }, [
    fieldTransfer.phase,
    gpsRecording,
    peripheralState.lastConnectedAt,
    peripheralState.secureSessionReady,
    storageSnapshot,
  ]);

  // A secure Spectre visit is a handoff opportunity, not an engineering task.
  // Drain automatically only after the storage snapshot proves GPS correlation
  // has reached a terminal state for every pending observation. The key
  // prevents a failed attempt from spinning until connection/backlog state
  // materially changes.
  useEffect(() => {
    const pending = storageSnapshot
      ? storageSnapshot.pendingUploadMission + storageSnapshot.pendingUploadNoise
      : 0;
    const pendingEnrichment = storageSnapshot
      ? storageSnapshot.pendingEnrichMission + storageSnapshot.pendingEnrichNoise
      : 0;
    // A fresh storage frame is a stronger attempt boundary than the Android
    // connection timestamp and changes on every authenticated device visit.
    const connectionMarker = storageSnapshot?.receivedAt ?? 0;
    const key = `${connectionMarker}:${pending}`;
    if (
      !gpsRecording ||
      !peripheralState.secureSessionReady ||
      pending <= 0 ||
      (pendingEnrichment > 0 && pending < WIFI_BULK_ENRICH_THRESHOLD) ||
      fieldTransfer.phase === 'copying' ||
      fieldTransfer.phase === 'relaying' ||
      automaticOffloadKeyRef.current === key
    ) {
      return;
    }
    automaticOffloadKeyRef.current = key;
    swallowPromise(offloadToPhoneRef.current?.());
  }, [
    fieldTransfer.phase,
    gpsRecording,
    peripheralState.secureSessionReady,
    storageSnapshot,
  ]);

  // Likewise, a validated home route should empty the already-durable phone
  // queue without making the operator press a transport-specific button.
  useEffect(() => {
    const key = `${relayStatus.pending}:${networkStatus.vpnInterface}`;
    if (
      !networkStatus.vpnValidated ||
      relayStatus.pending <= 0 ||
      fieldTransfer.phase === 'copying' ||
      fieldTransfer.phase === 'relaying' ||
      automaticRelayKeyRef.current === key
    ) {
      return;
    }
    automaticRelayKeyRef.current = key;
    swallowPromise(value.relayHome());
  }, [
    fieldTransfer.phase,
    networkStatus.vpnInterface,
    networkStatus.vpnValidated,
    relayStatus.pending,
  ]);

  return (
    <SpectreContext.Provider value={value}>{children}</SpectreContext.Provider>
  );
}

export function useSpectre() {
  const context = useContext(SpectreContext);
  if (!context) {
    throw new Error('useSpectre must be used inside SpectreProvider');
  }
  return context;
}
