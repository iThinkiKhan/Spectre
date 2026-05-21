import {
  NativeEventEmitter,
  NativeModules,
  Platform,
  type EmitterSubscription,
} from 'react-native';
import {base64ToBytes} from '../../protocol/base64';
import {decodePhoneStorageFrame} from '../../protocol/binary';
import type {PhoneStorageFrameV1} from '../../protocol/types';

type NativePeripheralModule = {
  startServer(config: {
    metadata: string;
    gpsBase64: string;
    controlBase64: string;
    enrichmentBase64: string;
    advertiseMode?: PeripheralAdvertiseMode;
    useDeviceLocation?: boolean;
  }): Promise<PeripheralState>;
  stopServer(): Promise<PeripheralState>;
  updateMetadata(metadata: string): Promise<void>;
  updateGpsValue(gpsBase64: string): Promise<void>;
  updateControlValue(controlBase64: string): Promise<void>;
  updateEnrichmentValue(enrichmentBase64: string, notify: boolean): Promise<void>;
  // Slice #2 command channel.  The phone hosts the request char; updating its
  // value and notifying delivers the encrypted request envelope to the device.
  // Responses arrive as device writes to the response char and surface via
  // the SpectrePeripheralCommandResponse event.
  updateCommandRequestValue(requestBase64: string): Promise<void>;
  getLastKnownLocation(): Promise<LocationFix | null>;
  getAuthPublicKey(): Promise<string>;
  addListener(eventName: string): void;
  removeListeners(count: number): void;
};

export type PeripheralAdvertiseMode =
  | 'nameOnly'
  | 'uuidOnly'
  | 'service'
  | 'shortName';

export type PeripheralState = {
  running: boolean;
  advertising: boolean;
  connectedDevices: number;
  secureSessionReady?: boolean;
  advertiseMode?: PeripheralAdvertiseMode | string | null;
  advertiseStartConfirmed?: boolean;
  watchdogActive?: boolean;
  totalAdvertiseRestarts?: number;
  lastAdvertiseStartedAt?: number | null;
  lastAdvertiseFailureCode?: number | null;
  lastConnectedAt?: number | null;
  lastDisconnectedAt?: number | null;
  lastConnectedPeer?: string | null;
  lastDisconnectedPeer?: string | null;
  lastBatchReceivedAt?: number | null;
  lastBatchPeer?: string | null;
  lastBatchBytes?: number;
  lastBatchRecords?: number;
  lastStorageReceivedAt?: number | null;
  lastStoragePeer?: string | null;
  storageBase64?: string | null;
  totalBatchesReceived?: number;
  totalBatchBytes?: number;
  totalBatchRecords?: number;
  error?: string | null;
  moduleAvailable?: boolean;
};

export type PeripheralEventBatch = {
  base64: string;
  length: number;
  receivedAt: number;
  mock?: boolean;
};

export type PeripheralStorageSnapshot = PhoneStorageFrameV1 & {
  receivedAt: number;
};

export type PeripheralCommandResponseEvent = {
  base64: string;
  receivedAt: number;
};

export type PeripheralLogStreamEvent = {
  base64: string;
  receivedAt: number;
};

export type PeripheralDashboardStreamEvent = {
  base64: string;
  receivedAt: number;
};

export type PeripheralNotificationEvent = {
  base64: string;
  receivedAt: number;
};

export type LocationFix = {
  lat: number;
  lon: number;
  alt: number | null;
  accuracy: number | null;
  timestamp: number;
  provider: string | null;
};

export type SpectrePeripheralListener = {
  onStateChange?: (state: PeripheralState) => void;
  onEventBatchReceived?: (event: PeripheralEventBatch) => void;
  onStorageSnapshotReceived?: (snapshot: PeripheralStorageSnapshot) => void;
  onCommandResponseReceived?: (event: PeripheralCommandResponseEvent) => void;
  onLogStreamChunkReceived?: (event: PeripheralLogStreamEvent) => void;
  onDashboardStreamChunkReceived?: (event: PeripheralDashboardStreamEvent) => void;
  onNotificationReceived?: (event: PeripheralNotificationEvent) => void;
  onLog?: (message: string) => void;
};

const nativeModule =
  Platform.OS === 'android'
    ? (NativeModules.SpectrePeripheral as NativePeripheralModule | undefined)
    : undefined;

const eventEmitter = nativeModule
  ? new NativeEventEmitter(nativeModule)
  : null;

export class SpectrePeripheralBridge {
  private listener: SpectrePeripheralListener | null = null;
  private subscriptions: EmitterSubscription[] = [];

  setListener(listener: SpectrePeripheralListener | null) {
    this.listener = listener;
    this.attachNativeListeners();
  }

  isAvailable() {
    return !!nativeModule;
  }

  async start(config: {
    metadata: string;
    gpsBase64: string;
    controlBase64: string;
    enrichmentBase64: string;
    advertiseMode?: PeripheralAdvertiseMode;
    useDeviceLocation?: boolean;
  }): Promise<PeripheralState> {
    if (!nativeModule) {
      const fallback = {
        running: false,
        advertising: false,
        connectedDevices: 0,
        error: 'Native Android peripheral bridge unavailable',
        moduleAvailable: false,
      };
      this.listener?.onStateChange?.(fallback);
      return fallback;
    }

    const state = await nativeModule.startServer(config);
    this.listener?.onStateChange?.({
      ...state,
      moduleAvailable: true,
    });
    return {
      ...state,
      moduleAvailable: true,
    };
  }

  async stop(): Promise<PeripheralState> {
    if (!nativeModule) {
      return {
        running: false,
        advertising: false,
        connectedDevices: 0,
        error: 'Native Android peripheral bridge unavailable',
        moduleAvailable: false,
      };
    }

    const state = await nativeModule.stopServer();
    this.listener?.onStateChange?.({
      ...state,
      moduleAvailable: true,
    });
    return {
      ...state,
      moduleAvailable: true,
    };
  }

  async updateMetadata(metadata: string) {
    if (!nativeModule) {
      return;
    }

    await nativeModule.updateMetadata(metadata);
  }

  async updateGpsValue(gpsBase64: string) {
    if (!nativeModule) {
      return;
    }

    await nativeModule.updateGpsValue(gpsBase64);
  }

  async updateControlValue(controlBase64: string) {
    if (!nativeModule) {
      return;
    }

    await nativeModule.updateControlValue(controlBase64);
  }

  async updateEnrichmentValue(enrichmentBase64: string, notify = true) {
    if (!nativeModule) {
      return;
    }

    await nativeModule.updateEnrichmentValue(enrichmentBase64, notify);
  }

  // Slice #2 — phone-issued command request.  `requestBase64` is the
  // PLAINTEXT PhoneCommandRequestV1 envelope (version | opcode | requestId |
  // payload).  The native module handles secure-session encryption on
  // PHONE_SECURE_CHANNEL_COMMAND, writes the encrypted envelope to the
  // COMMAND_REQ characteristic, and notifies the device.  The corresponding
  // response is decrypted in-native and surfaces as plaintext via
  // SpectrePeripheralCommandResponse.
  async sendCommandRequest(requestBase64: string) {
    if (!nativeModule) {
      throw new Error('Native Android peripheral bridge unavailable');
    }

    await nativeModule.updateCommandRequestValue(requestBase64);
  }

  isCommandChannelAvailable() {
    return !!nativeModule && typeof nativeModule.updateCommandRequestValue === 'function';
  }

  async getLastKnownLocation() {
    if (!nativeModule) {
      return null;
    }

    return nativeModule.getLastKnownLocation();
  }

  async getAuthPublicKey(): Promise<string | null> {
    if (!nativeModule) {
      return null;
    }

    return nativeModule.getAuthPublicKey();
  }

  emitMockEventBatch(base64: string) {
    this.listener?.onEventBatchReceived?.({
      base64,
      length: base64ToBytes(base64).length,
      receivedAt: Date.now(),
      mock: true,
    });
  }

  destroy() {
    this.subscriptions.forEach(subscription => {
      try {
        subscription.remove();
      } catch {}
    });
    this.subscriptions = [];
  }

  private attachNativeListeners() {
    if (!eventEmitter || this.subscriptions.length > 0) {
      return;
    }

    this.subscriptions.push(
      eventEmitter.addListener('SpectrePeripheralState', payload => {
        this.listener?.onStateChange?.({
          ...payload,
          moduleAvailable: true,
        });
      }),
      eventEmitter.addListener('SpectrePeripheralEventBatch', payload => {
        this.listener?.onEventBatchReceived?.(payload);
      }),
      eventEmitter.addListener('SpectrePeripheralStorage', payload => {
        try {
          const snapshot = decodePhoneStorageFrame(payload.base64);
          this.listener?.onStorageSnapshotReceived?.({
            ...snapshot,
            receivedAt: payload.receivedAt ?? Date.now(),
          });
        } catch (error: any) {
          this.listener?.onLog?.(
            error?.message || 'Storage snapshot decode failed',
          );
        }
      }),
      eventEmitter.addListener('SpectrePeripheralLog', payload => {
        const message =
          typeof payload === 'string' ? payload : payload?.message ?? '';
        if (message) {
          this.listener?.onLog?.(message);
        }
      }),
      eventEmitter.addListener('SpectrePeripheralCommandResponse', payload => {
        if (!payload || typeof payload.base64 !== 'string') {
          return;
        }
        this.listener?.onCommandResponseReceived?.({
          base64: payload.base64,
          receivedAt:
            typeof payload.receivedAt === 'number'
              ? payload.receivedAt
              : Date.now(),
        });
      }),
      eventEmitter.addListener('SpectrePeripheralLogStream', payload => {
        if (!payload || typeof payload.base64 !== 'string') {
          return;
        }
        this.listener?.onLogStreamChunkReceived?.({
          base64: payload.base64,
          receivedAt:
            typeof payload.receivedAt === 'number'
              ? payload.receivedAt
              : Date.now(),
        });
      }),
      eventEmitter.addListener('SpectrePeripheralDashboardStream', payload => {
        if (!payload || typeof payload.base64 !== 'string') {
          return;
        }
        this.listener?.onDashboardStreamChunkReceived?.({
          base64: payload.base64,
          receivedAt:
            typeof payload.receivedAt === 'number'
              ? payload.receivedAt
              : Date.now(),
        });
      }),
      eventEmitter.addListener('SpectrePeripheralNotification', payload => {
        if (!payload || typeof payload.base64 !== 'string') {
          return;
        }
        this.listener?.onNotificationReceived?.({
          base64: payload.base64,
          receivedAt:
            typeof payload.receivedAt === 'number'
              ? payload.receivedAt
              : Date.now(),
        });
      }),
    );
  }
}
