import {NativeModules, Platform} from 'react-native';
import {bytesToBase64} from '../../protocol/base64';

export type RelayQueueStatus = {
  pending: number;
  published: number;
  pendingBytes: number;
  running: boolean;
  publishedThisPass: number;
  endpoint: string;
};

export type PhoneNetworkStatus = {
  vpnActive: boolean;
  vpnValidated: boolean;
  vpnInterface: string;
  cellularAvailable: boolean;
};

export type DurableOffloadRecord = {
  sessionId: string;
  eventId: number;
  lane: number;
  topic: string;
  payload: Uint8Array;
};

export type RelayArchiveRecord = {
  sessionId: string;
  eventId: number;
  lane: number;
  topic: string;
  payload: string;
  receivedAt: number;
  publishedAt: number | null;
};

export type BulkReceiverEndpoint = {
  ssid: string;
  password: string;
  port: number;
  tokenBase64: string;
};

export type BulkReceiverStatus = {
  phase: 'idle' | 'starting' | 'waiting' | 'receiving' | 'complete' | 'error';
  error: string;
  copied: number;
  bytes: number;
  elapsedMs: number;
};

type RelayNativeModule = {
  enqueueRecord(record: {
    sessionId: string;
    eventId: number;
    lane: number;
    topic: string;
    payloadBase64: string;
  }): Promise<{inserted: boolean; digest: string}>;
  getStatus(): Promise<RelayQueueStatus>;
  getNetworkStatus(): Promise<PhoneNetworkStatus>;
  getRecentRecords(limit: number): Promise<RelayArchiveRecord[]>;
  relayPending(options?: {host?: string; port?: number}): Promise<RelayQueueStatus>;
  startBulkReceiver(): Promise<BulkReceiverEndpoint>;
  getBulkStatus(): Promise<BulkReceiverStatus>;
  stopBulkReceiver(): Promise<void>;
};

const nativeModule =
  Platform.OS === 'android'
    ? (NativeModules.SpectreRelay as RelayNativeModule | undefined)
    : undefined;

export class SpectreRelayService {
  isAvailable() {
    return !!nativeModule;
  }

  async enqueue(record: DurableOffloadRecord) {
    if (!nativeModule) {
      throw new Error('Native durable relay is unavailable');
    }
    return nativeModule.enqueueRecord({
      sessionId: record.sessionId,
      eventId: record.eventId,
      lane: record.lane,
      topic: record.topic,
      payloadBase64: bytesToBase64(record.payload),
    });
  }

  async status(): Promise<RelayQueueStatus> {
    if (!nativeModule) {
      return {
        pending: 0,
        published: 0,
        pendingBytes: 0,
        running: false,
        publishedThisPass: 0,
        endpoint: 'unavailable',
      };
    }
    return nativeModule.getStatus();
  }

  async networkStatus(): Promise<PhoneNetworkStatus> {
    if (!nativeModule) {
      return {
        vpnActive: false,
        vpnValidated: false,
        vpnInterface: '',
        cellularAvailable: false,
      };
    }
    return nativeModule.getNetworkStatus();
  }

  async recentRecords(limit = 5000): Promise<RelayArchiveRecord[]> {
    if (!nativeModule || typeof nativeModule.getRecentRecords !== 'function') {
      return [];
    }
    return nativeModule.getRecentRecords(Math.max(1, Math.min(limit, 5000)));
  }

  async relayHome(): Promise<RelayQueueStatus> {
    if (!nativeModule) {
      throw new Error('Native durable relay is unavailable');
    }
    // Matches Spectre's existing field deployment endpoint. No device or
    // broker settings are modified; Android routes this private address over
    // the active WireGuard tunnel.
    let publishedThisPass = 0;
    let status = await nativeModule.relayPending({host: '192.168.0.11', port: 1883});
    publishedThisPass += status.publishedThisPass;
    while (status.pending > 0 && status.publishedThisPass > 0) {
      status = await nativeModule.relayPending({host: '192.168.0.11', port: 1883});
      publishedThisPass += status.publishedThisPass;
    }
    return {...status, publishedThisPass};
  }

  async startBulkReceiver(): Promise<BulkReceiverEndpoint> {
    if (!nativeModule?.startBulkReceiver) throw new Error('Native bulk receiver is unavailable');
    return nativeModule.startBulkReceiver();
  }

  async bulkStatus(): Promise<BulkReceiverStatus> {
    if (!nativeModule?.getBulkStatus) {
      return {phase: 'error', error: 'Native bulk receiver is unavailable', copied: 0, bytes: 0, elapsedMs: 0};
    }
    return nativeModule.getBulkStatus();
  }

  async stopBulkReceiver(): Promise<void> {
    await nativeModule?.stopBulkReceiver?.();
  }
}
