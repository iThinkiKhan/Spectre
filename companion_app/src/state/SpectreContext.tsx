/* eslint-disable no-bitwise */

import React, {createContext, useContext, useEffect, useRef, useState} from 'react';
import {Platform} from 'react-native';

import {
  encodeEnrichmentRecords,
  encodeEventBatchRecords,
  encodePhoneControlFrame,
  encodePhoneGpsFrame,
  PHONE_CONTROL_FLAG_BATCH_RECEIVED,
  PHONE_CONTROL_FLAG_CANCEL,
  PHONE_CONTROL_FLAG_DUMP_REQUEST,
  PHONE_CONTROL_FLAG_WG_ACTIVE,
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
import type {EnrichmentRecord, EventBatchRecord} from '../protocol/types';
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
import {
  EnrichmentService,
  type FinalizedEventBatch,
} from '../services/enrichment/EnrichmentService';

type TabKey = 'link' | 'enrich' | 'ops';
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

export type ActiveLocationFix = {
  lat: number;
  lon: number;
  alt: number;
  accuracy: number;
  timestamp: number;
  source: 'device' | 'manual';
  provider: string | null;
};

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
  locationMode: LocationMode;
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
  setWireGuardActive: (value: boolean) => void;
  sendBatchNow: (batchId: string) => Promise<void>;
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

function nowId(prefix: string) {
  return `${prefix}-${Date.now()}-${Math.round(Math.random() * 1000)}`;
}

function swallowPromise(task: Promise<unknown> | null | undefined) {
  task?.catch(() => {});
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

function buildGpsBase64(location: ActiveLocationFix | null) {
  if (!location) {
    return encodePhoneGpsFrame({
      version: 1,
      latE7: 0,
      lonE7: 0,
      altCm: 0,
      accuracyDm: 0,
      epochUtc: Math.floor(Date.now() / 1000),
      flags: 0,
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
  location: ActiveLocationFix,
  tag: string,
): {
  records: EnrichmentRecord[];
  normalizedTag: ReturnType<typeof normalizeEnrichmentTag>;
} {
  const normalizedTag = normalizeEnrichmentTag(tag);
  return {
    records: events.map(event => ({
      eventId: event.eventId,
      latE7: Math.round(location.lat * 10_000_000),
      lonE7: Math.round(location.lon * 10_000_000),
      altCm: Math.round(location.alt * 100),
      accuracyDm: Math.max(0, Math.round(location.accuracy * 10)),
      epochUtc: Math.max(0, Math.floor(location.timestamp / 1000)),
      flags: normalizedTag.value.length ? 0x01 : 0,
      tag: normalizedTag.value,
    })),
    normalizedTag,
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
  const enrichmentRef = useRef<EnrichmentService | null>(null);
  const activeTagRef = useRef('FIELD');
  const autoApplyRef = useRef(true);
  const activeLocationRef = useRef<ActiveLocationFix | null>(null);
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

  const [activeTab, setActiveTab] = useState<TabKey>('link');
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
  const [locationMode, setLocationMode] = useState<LocationMode>('device');
  const [deviceLocation, setDeviceLocation] =
    useState<ActiveLocationFix | null>(null);
  const [manualLocation, setManualLocation] =
    useState<ActiveLocationFix | null>(null);
  const [manualLocationDraft, setManualLocationDraft] =
    useState<ManualLocationDraft>(EMPTY_MANUAL_LOCATION);
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

  const activeLocation =
    locationMode === 'manual'
      ? manualLocation
      : locationMode === 'device'
        ? deviceLocation
        : null;

  const controlBase64 = buildControlBase64(
    wireGuardActive,
    controlPulses,
    pulseClock,
  );
  const gpsBase64 = buildGpsBase64(activeLocation);
  const metadata = buildMetadata(locationMode, autoApplyEnrichment);
  const statusSummary = summarizeStatus(promptState);

  const missingPermissionLabels = permissions.missing.map(friendlyPermissionName);

  const appendLog = (message: string, level: LogLevel = 'info') => {
    setLogs(previous => appendUniqueLog(previous, message, level));
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

    if (!bridge) {
      appendLog('Peripheral bridge unavailable for enrichment send', 'error');
      return;
    }

    if (!location) {
      setEventBatches(previous =>
        previous.map(batch =>
          batch.id === batchId
            ? {...batch, status: 'waiting', note: 'Waiting for a usable location'}
            : batch,
        ),
      );
      appendLog('Enrichment held until a location fix is available', 'warn');
      return;
    }

    const {records, normalizedTag} = buildEnrichmentRecords(
      events,
      location,
      activeTagRef.current,
    );
    const payload = encodeEnrichmentRecords(records);

    try {
      await bridge.updateEnrichmentValue(payload, true);
      const publishedAt = Date.now();

      setEventBatches(previous =>
        previous.map(batch =>
          batch.id === batchId
            ? {
                ...batch,
                status: 'sent',
                tag: normalizedTag.value,
                location,
                lastPublishedAt: publishedAt,
                note:
                  source === 'mock'
                    ? 'Mock enrichment published to phone peripheral'
                    : 'Enrichment payload published for Spectre pickup',
              }
            : batch,
        ),
      );
      setLastPublishedBatch({
        records: records.length,
        bytes: records.length * ENRICHMENT_RECORD_SIZE,
        source,
        tag: normalizedTag.value,
        sentAt: publishedAt,
      });
      appendLog(`Published enrichment batch (${records.length} records)`);
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
    const nextStatus: BatchStatus =
      activeLocationRef.current && autoApplyRef.current ? 'ready' : 'waiting';

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

    if (autoApplyRef.current && activeLocationRef.current) {
      setTimeout(() => {
        swallowPromise(publishBatch(batchId, payload.events, payload.source));
      }, 0);
    } else if (payload.source === 'spectre' && !activeLocationRef.current) {
      queueControlPulse('cancel');
      appendLog(
        'No phone location available; canceling Spectre enrichment request',
        'warn',
      );
    }
  };

  const refreshDeviceLocation = async () => {
    if (locationMode !== 'device') {
      return;
    }

    const fix = await peripheralRef.current?.getLastKnownLocation();
    if (!fix) {
      appendLog('No recent Android location fix available', 'warn');
      return;
    }

    const nextLocation = locationFromNativeFix(fix);
    setDeviceLocation(nextLocation);
    appendLog(
      `Location updated from phone (${nextLocation.lat.toFixed(5)}, ${nextLocation.lon.toFixed(5)})`,
    );
  };

  const startFieldMode = async () => {
    if (!permissions.allGranted) {
      appendLog('Grant Android Bluetooth permissions before Field Mode', 'warn');
      return;
    }

    const bridge = peripheralRef.current;
    if (!bridge) {
      appendLog('Peripheral bridge unavailable for Field Mode', 'error');
      return;
    }

    try {
      const state = await bridge.start({
        metadata: startupConfigRef.current.metadata,
        gpsBase64: startupConfigRef.current.gpsBase64,
        controlBase64: startupConfigRef.current.controlBase64,
        enrichmentBase64: '',
        advertiseMode: 'uuidOnly',
        useDeviceLocation: locationMode === 'device',
      });
      setPeripheralState(state);
      appendLog('Field Mode foreground service started');
    } catch (error: any) {
      appendLog(error?.message || 'Failed to start Field Mode', 'error');
      throw error;
    }
  };

  const stopFieldMode = async () => {
    const bridge = peripheralRef.current;
    if (!bridge) {
      return;
    }

    try {
      const state = await bridge.stop();
      setPeripheralState(state);
      appendLog('Field Mode stopped');
    } catch (error: any) {
      appendLog(error?.message || 'Failed to stop Field Mode', 'error');
      throw error;
    }
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
    activeTagRef.current = activeTag;
  }, [activeTag]);

  useEffect(() => {
    autoApplyRef.current = autoApplyEnrichment;
  }, [autoApplyEnrichment]);

  useEffect(() => {
    activeLocationRef.current = activeLocation;
  }, [activeLocation]);

  useEffect(() => {
    const timer = setInterval(() => {
      setPulseClock(Date.now());
      setControlPulses(previous =>
        previous.filter(pulse => pulse.expiresAt > Date.now()),
      );
    }, 1000);

    return () => {
      clearInterval(timer);
    };
  }, []);

  useEffect(() => {
    swallowPromise(peripheralRef.current?.updateControlValue(controlBase64));
  }, [controlBase64]);

  useEffect(() => {
    swallowPromise(peripheralRef.current?.updateGpsValue(gpsBase64));
  }, [gpsBase64]);

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
      onLog: message => {
        appendLog(message);
      },
    });

    swallowPromise(bleRef.current.initialize());
    swallowPromise(refreshPermissions());

    return () => {
      const ble = bleRef.current;
      const peripheral = peripheralRef.current;
      const enrichment = enrichmentRef.current;

      bleRef.current = null;
      peripheralRef.current = null;
      enrichmentRef.current = null;

      ble?.destroy();
      enrichment?.destroy();
      peripheral?.destroy();
      swallowPromise(peripheral?.stop());
    };
  }, []);

  useEffect(() => {
    if (!permissions.allGranted || locationMode !== 'device') {
      return;
    }

    swallowPromise(refreshDeviceLocationRef.current?.());
    const timer = setInterval(() => {
      swallowPromise(refreshDeviceLocationRef.current?.());
    }, 20_000);

    return () => {
      clearInterval(timer);
    };
  }, [permissions.allGranted, locationMode]);

  useEffect(() => {
    if (!autoApplyEnrichment || !activeLocation) {
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
  }, [activeLocation, autoApplyEnrichment, eventBatches]);

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
    locationMode,
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
        appendLog('Bluetooth, advertise, and location permissions granted');
      }
    },
    startFieldMode,
    stopFieldMode,
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
    setWireGuardActive: nextValue => {
      setWireGuardActiveState(nextValue);
    },
    sendBatchNow: async batchId => {
      const batch = eventBatches.find(entry => entry.id === batchId);
      if (!batch) {
        return;
      }
      await publishBatch(batch.id, batch.events, batch.source);
    },
    injectMockBatch: () => {
      const payload = encodeEventBatchRecords(demoBatch());
      peripheralRef.current?.emitMockEventBatch(payload);
    },
  };

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
