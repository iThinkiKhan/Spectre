import {BleManager, Device, Subscription} from 'react-native-ble-plx';

import {base64ToUtf8, utf8ToBase64} from '../../protocol/base64';
import {
  BadUsbScriptDraft,
  BadUsbUploadPhase,
  BadUsbUploadState,
  buildBadUsbChunkFrames,
  encodeBadUsbBeginCommand,
  encodeBadUsbCancelCommand,
  encodeBadUsbCommitCommand,
  emptyBadUsbUploadState,
  normalizeBadUsbScriptDraft,
  parseBadUsbUploadStatus,
} from '../../protocol/badusb';
import {
  BADUSB_UPLOAD_CHUNK_DATA_BYTES,
  BADUSB_UPLOAD_HEADER_BYTES,
  classifyPromptKind,
  isCompletedReceipt,
  isRetryableReceipt,
  validatePromptReply,
} from '../../protocol/contracts';
import {parseStatusBlob} from '../../protocol/binary';
import {
  SPECTRE_BADUSB_COMMAND_UUID,
  SPECTRE_BADUSB_UPLOAD_DATA_UUID,
  SPECTRE_BADUSB_UPLOAD_STATUS_UUID,
  SPECTRE_TEXT_INPUT_UUID,
  SPECTRE_TEXT_PROMPT_UUID,
  SPECTRE_TEXT_RECEIPT_UUID,
  SPECTRE_TEXT_SERVICE_UUID,
  SPECTRE_TEXT_STATUS_UUID,
} from '../../protocol/uuids';
import type {
  BleClientListener,
  SpectreConnectionState,
  SpectreDeviceSummary,
  SpectrePromptState,
} from './bleTypes';

const SCAN_TIMEOUT_MS = 12000;
const RECONNECT_DELAY_MS = 2200;
const BADUSB_READY_TIMEOUT_MS = 5000;
const BADUSB_COMMIT_TIMEOUT_MS = 12000;
const PREFERRED_MTU = 185;
const DEFAULT_MTU = 23;

type UploadWaiter = {
  token: number;
  phases: BadUsbUploadPhase[];
  resolve: (state: BadUsbUploadState) => void;
  reject: (error: Error) => void;
  timer: ReturnType<typeof setTimeout>;
};

function emptyPromptState(): SpectrePromptState {
  return {
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
  };
}

function coerceName(device: Device): string {
  return device.name || device.localName || 'Spectre';
}

function summarizeDevice(device: Device): SpectreDeviceSummary {
  return {
    id: device.id,
    name: coerceName(device),
    localName: device.localName ?? null,
    rssi: typeof device.rssi === 'number' ? device.rssi : null,
    lastSeenMs: Date.now(),
  };
}

function normalizePromptText(value?: string | null) {
  const text = value?.replace(/\0/g, '').trim() ?? '';
  return text.length ? text : null;
}

function normalizeReceipt(value?: string | null) {
  const receipt = value?.trim().toUpperCase() ?? '';
  return receipt.length ? receipt : null;
}

function normalizeInputState(value?: string | null) {
  const inputState = value?.trim().toUpperCase() ?? '';
  return inputState.length ? inputState : null;
}

function linkStateLabel(rawState?: string | null): string {
  switch (rawState) {
    case '0':
      return 'IDLE';
    case '1':
      return 'SCANNING';
    case '2':
      return 'CONNECTING';
    case '3':
      return 'CONNECTED';
    case '4':
      return 'SUBSCRIBED';
    default:
      return rawState || 'UNKNOWN';
  }
}

export class BleClientService {
  private readonly manager = new BleManager();
  private readonly discoveredDevices = new Map<string, Device>();
  private readonly uploadWaiters = new Set<UploadWaiter>();
  private listener: BleClientListener | null = null;
  private device: Device | null = null;
  private adapterSubscription: Subscription | null = null;
  private disconnectSubscription: Subscription | null = null;
  private characteristicSubscriptions: Subscription[] = [];
  private promptState: SpectrePromptState = emptyPromptState();
  private badUsbUploadState: BadUsbUploadState = emptyBadUsbUploadState();
  private scanStopTimer: ReturnType<typeof setTimeout> | null = null;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private scanResolver: ((devices: SpectreDeviceSummary[]) => void) | null = null;
  private lastTargetId: string | null = null;
  private lastTargetName: string | null = null;
  private manualDisconnect = false;
  private disposed = false;
  private isScanning = false;
  private scanGeneration = 0;
  private connectGeneration = 0;
  private submittedPromptToken: number | null = null;
  private localPromptError: string | null = null;
  private nextUploadToken = 1;
  private negotiatedMtu = DEFAULT_MTU;

  constructor(listener?: BleClientListener) {
    this.listener = listener ?? null;
    this.adapterSubscription = this.manager.onStateChange(
      state => {
        this.listener?.onAdapterState?.(state);
      },
      true,
    );
  }

  setListener(listener: BleClientListener | null) {
    this.listener = listener;
    this.listener?.onPromptChange?.(this.promptState);
    this.listener?.onBadUsbUploadChange?.(this.badUsbUploadState);
    this.listener?.onScanUpdate?.(this.getDiscoveredDeviceList());
  }

  async initialize() {
    const state = await this.manager.state();
    this.listener?.onAdapterState?.(state);
  }

  destroy() {
    this.disposed = true;
    this.cancelScan();
    this.clearReconnectTimer();
    this.clearUploadWaiters();
    this.teardownCharacteristicSubscriptions();
    this.disconnectSubscription?.remove();
    this.adapterSubscription?.remove();

    if (this.device) {
      this.manager.cancelDeviceConnection(this.device.id).catch(() => {});
      this.device = null;
    }

    this.manager.destroy();
  }

  getPromptState() {
    return this.promptState;
  }

  getBadUsbUploadState() {
    return this.badUsbUploadState;
  }

  getConnectedDevice() {
    return this.device ? summarizeDevice(this.device) : null;
  }

  async scanForSpectre(timeoutMs = SCAN_TIMEOUT_MS) {
    this.clearReconnectTimer();
    this.cancelScan();
    this.discoveredDevices.clear();
    this.listener?.onScanUpdate?.([]);

    const generation = ++this.scanGeneration;
    this.emitConnection('scanning', 'Scanning for Spectre text service');

    return new Promise<SpectreDeviceSummary[]>(resolve => {
      this.scanResolver = resolve;
      this.isScanning = true;

      this.manager.startDeviceScan(
        [SPECTRE_TEXT_SERVICE_UUID],
        {
          allowDuplicates: false,
        },
        (error, scannedDevice) => {
          if (this.disposed || generation !== this.scanGeneration) {
            return;
          }

          if (error) {
            this.listener?.onLog?.(`Scan error: ${error.message}`);
            this.emitConnection('error', error.message, this.getConnectedDevice());
            this.finishScan(generation);
            return;
          }

          if (!scannedDevice) {
            return;
          }

          const summary = summarizeDevice(scannedDevice);
          this.discoveredDevices.set(scannedDevice.id, scannedDevice);
          this.listener?.onScanUpdate?.(this.getDiscoveredDeviceList());
          this.listener?.onLog?.(
            `Discovered ${summary.name} (${summary.id.slice(0, 8)})`,
          );
        },
      );

      this.scanStopTimer = setTimeout(() => {
        if (generation !== this.scanGeneration) {
          return;
        }
        if (!this.device) {
          this.emitConnection('idle', 'Scan complete');
        }
        this.finishScan(generation);
      }, timeoutMs);
    });
  }

  async connectToDevice(deviceId: string) {
    this.manualDisconnect = false;
    this.clearReconnectTimer();
    this.cancelScan();

    if (this.device?.id === deviceId) {
      const current = summarizeDevice(this.device);
      this.emitConnection('connected', `Connected to ${current.name}`, current);
      return;
    }

    const cached = this.discoveredDevices.get(deviceId);
    const summary = cached ? summarizeDevice(cached) : null;
    const generation = ++this.connectGeneration;

    this.lastTargetId = deviceId;
    this.lastTargetName = summary?.name ?? this.lastTargetName;
    this.emitConnection(
      'connecting',
      summary ? `Connecting to ${summary.name}` : 'Connecting to Spectre',
      summary,
    );

    try {
      if (this.device && this.device.id !== deviceId) {
        const previousDeviceId = this.device.id;
        this.disconnectSubscription?.remove();
        this.disconnectSubscription = null;
        this.teardownCharacteristicSubscriptions();
        this.device = null;
        await this.manager.cancelDeviceConnection(previousDeviceId).catch(() => {});
      }

      let targetDevice = cached;
      if (!targetDevice) {
        const knownDevices = await this.manager.devices([deviceId]);
        targetDevice = knownDevices[0];
      }

      if (!targetDevice) {
        throw new Error('Target device is no longer in range');
      }

      let connected = await targetDevice.connect({
        timeout: 10000,
      });
      await connected.discoverAllServicesAndCharacteristics();

      try {
        connected = await connected.requestMTU(PREFERRED_MTU);
        this.negotiatedMtu =
          typeof connected.mtu === 'number' && connected.mtu > 0
            ? connected.mtu
            : DEFAULT_MTU;
      } catch {
        this.negotiatedMtu = DEFAULT_MTU;
      }

      if (this.disposed || generation !== this.connectGeneration) {
        await this.manager.cancelDeviceConnection(connected.id).catch(() => {});
        return;
      }

      this.device = connected;
      this.lastTargetId = connected.id;
      this.lastTargetName = coerceName(connected);
      this.localPromptError = null;
      this.rebuildPromptState({});
      this.emitBadUsbUploadState({
        ...emptyBadUsbUploadState(),
        available: false,
        updatedAt: Date.now(),
      });

      this.disconnectSubscription?.remove();
      this.disconnectSubscription = this.manager.onDeviceDisconnected(
        connected.id,
        (_error, disconnectedDevice) => {
          if (this.disposed || generation !== this.connectGeneration) {
            return;
          }

          const disconnectedSummary = disconnectedDevice
            ? summarizeDevice(disconnectedDevice)
            : this.getConnectedDevice();

          this.teardownCharacteristicSubscriptions();
          this.device = null;
          this.negotiatedMtu = DEFAULT_MTU;
          this.submittedPromptToken = null;
          this.localPromptError = null;
          this.emitPrompt(emptyPromptState());
          this.failActiveUpload('Spectre link dropped during BadUSB transfer');

          if (this.manualDisconnect) {
            this.emitConnection(
              'disconnected',
              'Disconnected from Spectre',
              disconnectedSummary,
            );
            return;
          }

          this.emitConnection(
            'reconnecting',
            'Spectre link dropped, retrying',
            disconnectedSummary,
          );
          this.scheduleReconnect();
        },
      );

      await this.readInitialTextState(connected);

      if (this.disposed || generation !== this.connectGeneration) {
        return;
      }

      this.attachCharacteristicMonitors(connected, generation);

      this.emitConnection(
        'connected',
        `Connected to ${coerceName(connected)}`,
        summarizeDevice(connected),
      );
      this.listener?.onLog?.(`Connected to ${coerceName(connected)}`);
      this.listener?.onLog?.(`Negotiated BLE MTU ${this.negotiatedMtu}`);
    } catch (error: any) {
      if (this.disposed || generation !== this.connectGeneration) {
        return;
      }

      const message = error?.message || 'BLE connect failed';
      this.listener?.onLog?.(`Connect failed: ${message}`);
      this.emitConnection('error', message, summary);
      this.scheduleReconnect();
      throw error;
    }
  }

  async disconnect() {
    this.manualDisconnect = true;
    this.clearReconnectTimer();
    this.cancelScan();
    this.connectGeneration += 1;
    this.teardownCharacteristicSubscriptions();

    this.submittedPromptToken = null;
    this.localPromptError = null;
    this.emitPrompt(emptyPromptState());
    this.clearUploadWaiters();
    this.emitBadUsbUploadState({
      ...emptyBadUsbUploadState(),
      updatedAt: Date.now(),
    });

    const currentDevice = this.device ? summarizeDevice(this.device) : null;
    const deviceId = this.device?.id;
    this.device = null;
    this.negotiatedMtu = DEFAULT_MTU;
    this.disconnectSubscription?.remove();
    this.disconnectSubscription = null;

    if (deviceId) {
      try {
        await this.manager.cancelDeviceConnection(deviceId);
      } catch {}
    }

    this.emitConnection('disconnected', 'Spectre link closed', currentDevice);
  }

  async submitPromptReply(text: string) {
    if (!this.device) {
      throw new Error('No Spectre device connected');
    }

    if (!this.hasActivePromptSession()) {
      throw new Error('No active Spectre input request');
    }

    const validation = validatePromptReply(text);
    if (!validation.ok) {
      this.localPromptError = validation.reason;
      this.rebuildPromptState({});
      throw new Error(validation.reason);
    }

    try {
      await this.device.writeCharacteristicWithResponseForService(
        SPECTRE_TEXT_SERVICE_UUID,
        SPECTRE_TEXT_INPUT_UUID,
        utf8ToBase64(validation.value),
      );

      if (typeof this.promptState.token === 'number') {
        this.submittedPromptToken = this.promptState.token;
      }
      this.localPromptError = null;
      this.rebuildPromptState({
        receipt: 'PENDING',
        updatedAt: Date.now(),
      });
    } catch (error: any) {
      const message = error?.message || 'Prompt send failed';
      this.localPromptError = message;
      this.rebuildPromptState({});
      throw error;
    }
  }

  async uploadBadUsbScript(draft: BadUsbScriptDraft) {
    if (!this.device) {
      throw new Error('No Spectre device connected');
    }
    if (!this.badUsbUploadState.available) {
      throw new Error(
        'Connected Spectre firmware does not expose the BadUSB vault upload endpoint yet.',
      );
    }

    if (
      this.badUsbUploadState.phase === 'uploading' ||
      this.badUsbUploadState.phase === 'committing'
    ) {
      throw new Error('A BadUSB transfer is already in progress.');
    }

    const chunkSize = this.computeUploadChunkSize();
    const script = normalizeBadUsbScriptDraft(draft, chunkSize);
    const token = this.issueUploadToken();

    this.emitBadUsbUploadState({
      phase: 'ready',
      rawStatus: null,
      token,
      fileName: script.fileName,
      message: 'Preparing vault transfer',
      receivedChunks: 0,
      totalChunks: script.totalChunks,
      receivedBytes: 0,
      totalBytes: script.totalBytes,
      available: true,
      updatedAt: Date.now(),
    });

    try {
      await this.writeBadUsbCommand(encodeBadUsbBeginCommand(token, script));
      await this.waitForUploadPhase(token, ['ready', 'uploading'], BADUSB_READY_TIMEOUT_MS);

      const frames = buildBadUsbChunkFrames(token, script);
      for (let index = 0; index < frames.length; index += 1) {
        await this.writeBadUsbFrame(token, frames[index]);

        const receivedBytes = Math.min(
          script.totalBytes,
          (index + 1) * script.chunkSize,
        );
        this.emitBadUsbUploadState({
          ...this.badUsbUploadState,
          phase: 'uploading',
          token,
          fileName: script.fileName,
          message: `Chunk ${index + 1}/${script.totalChunks}`,
          receivedChunks: index + 1,
          totalChunks: script.totalChunks,
          receivedBytes,
          totalBytes: script.totalBytes,
          available: true,
          updatedAt: Date.now(),
        });
      }

      this.emitBadUsbUploadState({
        ...this.badUsbUploadState,
        phase: 'committing',
        token,
        fileName: script.fileName,
        message: 'Committing to /config/vault/badusb',
        totalChunks: script.totalChunks,
        totalBytes: script.totalBytes,
        available: true,
        updatedAt: Date.now(),
      });

      await this.writeBadUsbCommand(encodeBadUsbCommitCommand(token));
      await this.waitForUploadPhase(token, ['complete'], BADUSB_COMMIT_TIMEOUT_MS);

      this.listener?.onLog?.(
        `BadUSB upload committed (${script.fileName}, ${script.totalBytes} bytes)`,
      );
      script.warnings.forEach(warning => this.listener?.onLog?.(warning));
    } catch (error: any) {
      await this.safeCancelUpload(token);
      const message = error?.message || 'BadUSB upload failed';
      this.emitBadUsbUploadState({
        ...this.badUsbUploadState,
        phase: 'error',
        token,
        fileName: script.fileName,
        message,
        totalChunks: script.totalChunks,
        totalBytes: script.totalBytes,
        available: true,
        updatedAt: Date.now(),
      });
      throw error;
    }
  }

  async cancelBadUsbUpload() {
    const token = this.badUsbUploadState.token;
    if (!this.device || typeof token !== 'number') {
      return;
    }

    await this.writeBadUsbCommand(encodeBadUsbCancelCommand(token));
    try {
      await this.waitForUploadPhase(token, ['cancelled', 'idle'], 4000);
    } catch {}
  }

  private async readInitialTextState(device: Device) {
    const [prompt, receipt, status, uploadStatus] = await Promise.allSettled([
      device.readCharacteristicForService(
        SPECTRE_TEXT_SERVICE_UUID,
        SPECTRE_TEXT_PROMPT_UUID,
      ),
      device.readCharacteristicForService(
        SPECTRE_TEXT_SERVICE_UUID,
        SPECTRE_TEXT_RECEIPT_UUID,
      ),
      device.readCharacteristicForService(
        SPECTRE_TEXT_SERVICE_UUID,
        SPECTRE_TEXT_STATUS_UUID,
      ),
      device.readCharacteristicForService(
        SPECTRE_TEXT_SERVICE_UUID,
        SPECTRE_BADUSB_UPLOAD_STATUS_UUID,
      ),
    ]);

    if (prompt.status === 'fulfilled') {
      this.handlePromptValue(prompt.value.value);
    }
    if (receipt.status === 'fulfilled') {
      this.handleReceiptValue(receipt.value.value);
    }
    if (status.status === 'fulfilled') {
      this.handleStatusValue(status.value.value);
    }
    if (uploadStatus.status === 'fulfilled') {
      this.handleBadUsbStatusValue(uploadStatus.value.value);
    } else {
      this.emitBadUsbUploadState({
        ...emptyBadUsbUploadState(),
        available: false,
        updatedAt: Date.now(),
      });
    }
  }

  private attachCharacteristicMonitors(device: Device, generation: number) {
    this.teardownCharacteristicSubscriptions();

    this.characteristicSubscriptions.push(
      device.monitorCharacteristicForService(
        SPECTRE_TEXT_SERVICE_UUID,
        SPECTRE_TEXT_PROMPT_UUID,
        (error, characteristic) => {
          if (this.disposed || generation !== this.connectGeneration) {
            return;
          }

          if (error) {
            this.listener?.onLog?.(`Prompt monitor error: ${error.message}`);
            return;
          }

          this.handlePromptValue(characteristic?.value);
        },
      ),
      device.monitorCharacteristicForService(
        SPECTRE_TEXT_SERVICE_UUID,
        SPECTRE_TEXT_RECEIPT_UUID,
        (error, characteristic) => {
          if (this.disposed || generation !== this.connectGeneration) {
            return;
          }

          if (error) {
            this.listener?.onLog?.(`Receipt monitor error: ${error.message}`);
            return;
          }

          this.handleReceiptValue(characteristic?.value);
        },
      ),
      device.monitorCharacteristicForService(
        SPECTRE_TEXT_SERVICE_UUID,
        SPECTRE_TEXT_STATUS_UUID,
        (error, characteristic) => {
          if (this.disposed || generation !== this.connectGeneration) {
            return;
          }

          if (error) {
            this.listener?.onLog?.(`Status monitor error: ${error.message}`);
            return;
          }

          this.handleStatusValue(characteristic?.value);
        },
      ),
      device.monitorCharacteristicForService(
        SPECTRE_TEXT_SERVICE_UUID,
        SPECTRE_BADUSB_UPLOAD_STATUS_UUID,
        (error, characteristic) => {
          if (this.disposed || generation !== this.connectGeneration) {
            return;
          }

          if (error) {
            this.listener?.onLog?.(`BadUSB status monitor error: ${error.message}`);
            this.emitBadUsbUploadState({
              ...this.badUsbUploadState,
              available: false,
              updatedAt: Date.now(),
            });
            return;
          }

          this.handleBadUsbStatusValue(characteristic?.value);
        },
      ),
    );
  }

  private handlePromptValue(value?: string | null) {
    this.rebuildPromptState({
      promptText: normalizePromptText(base64ToUtf8(value)),
      updatedAt: Date.now(),
    });
  }

  private handleReceiptValue(value?: string | null) {
    this.rebuildPromptState({
      receipt: normalizeReceipt(base64ToUtf8(value)),
      updatedAt: Date.now(),
    });
  }

  private handleStatusValue(value?: string | null) {
    const rawStatus = normalizePromptText(base64ToUtf8(value));
    const parsedStatus = rawStatus ? parseStatusBlob(rawStatus) : {};
    const normalizedInputState = normalizeInputState(parsedStatus.input);
    const token =
      typeof parsedStatus.tok === 'string' && parsedStatus.tok.length
        ? Number(parsedStatus.tok)
        : null;

    this.rebuildPromptState({
      rawStatus,
      parsedStatus: {
        ...parsedStatus,
        input: normalizedInputState || '',
        stateLabel: linkStateLabel(parsedStatus.state),
      },
      token: Number.isFinite(token) ? token : null,
      updatedAt: Date.now(),
    });
  }

  private handleBadUsbStatusValue(value?: string | null) {
    const rawStatus = normalizePromptText(base64ToUtf8(value));
    if (!rawStatus) {
      this.emitBadUsbUploadState({
        ...this.badUsbUploadState,
        phase: 'idle',
        rawStatus: null,
        message: null,
        available: true,
        updatedAt: Date.now(),
      });
      return;
    }

    this.emitBadUsbUploadState({
      ...this.badUsbUploadState,
      ...parseBadUsbUploadStatus(rawStatus),
      available: true,
      updatedAt: Date.now(),
    });
  }

  private rebuildPromptState(patch: Partial<SpectrePromptState>) {
    const previousToken = this.promptState.token;
    const nextPromptText =
      patch.promptText === undefined
        ? this.promptState.promptText
        : patch.promptText;
    const nextReceipt = normalizeReceipt(
      patch.receipt === undefined ? this.promptState.receipt : patch.receipt,
    );
    const nextParsedStatus =
      patch.parsedStatus === undefined
        ? this.promptState.parsedStatus
        : patch.parsedStatus;
    const nextToken =
      patch.token === undefined ? this.promptState.token : patch.token;
    const nextInputState = normalizeInputState(nextParsedStatus.input);

    if (nextToken !== previousToken) {
      this.submittedPromptToken = null;
      this.localPromptError = null;
    }

    if (isRetryableReceipt(nextReceipt) || nextReceipt === 'IDLE') {
      if (
        typeof nextToken === 'number' &&
        this.submittedPromptToken === nextToken
      ) {
        this.submittedPromptToken = null;
      }
    }

    if (isCompletedReceipt(nextReceipt) && nextReceipt === 'CONSUMED') {
      this.submittedPromptToken = null;
      this.localPromptError = null;
    }

    const submittedReply =
      typeof nextToken === 'number' && this.submittedPromptToken === nextToken;
    const pending =
      !!nextPromptText ||
      nextInputState === 'PENDING' ||
      nextInputState === 'READY';
    const awaitingReply =
      pending && nextInputState !== 'READY' && !submittedReply;

    this.emitPrompt({
      promptText: nextPromptText,
      pending,
      awaitingReply,
      submittedReply,
      receipt: nextReceipt,
      rawStatus:
        patch.rawStatus === undefined ? this.promptState.rawStatus : patch.rawStatus,
      parsedStatus: nextParsedStatus,
      token: typeof nextToken === 'number' ? nextToken : null,
      promptKind: classifyPromptKind(nextPromptText),
      replyError: awaitingReply ? this.localPromptError : null,
      updatedAt:
        patch.updatedAt === undefined ? this.promptState.updatedAt : patch.updatedAt,
    });
  }

  private emitPrompt(nextPromptState: SpectrePromptState) {
    this.promptState = nextPromptState;
    this.listener?.onPromptChange?.(nextPromptState);
  }

  private emitBadUsbUploadState(nextUploadState: BadUsbUploadState) {
    this.badUsbUploadState = nextUploadState;
    this.listener?.onBadUsbUploadChange?.(nextUploadState);
    this.resolveUploadWaiters(nextUploadState);
  }

  private emitConnection(
    state: SpectreConnectionState,
    detail?: string,
    device?: SpectreDeviceSummary | null,
  ) {
    this.listener?.onConnectionChange?.(
      state,
      detail,
      device ?? this.getConnectedDevice(),
    );
  }

  private scheduleReconnect() {
    if (this.manualDisconnect || this.disposed) {
      return;
    }

    this.clearReconnectTimer();
    this.reconnectTimer = setTimeout(() => {
      this.retryLastTarget().catch(() => {});
    }, RECONNECT_DELAY_MS);
  }

  private async retryLastTarget() {
    if (this.manualDisconnect || this.disposed) {
      return;
    }

    if (!this.lastTargetId && !this.lastTargetName) {
      return;
    }

    this.emitConnection('reconnecting', 'Searching for last Spectre device');
    const discovered = await this.scanForSpectre(6000);
    const target =
      discovered.find(device => device.id === this.lastTargetId) ||
      discovered.find(device => device.name === this.lastTargetName) ||
      discovered[0];

    if (!target) {
      this.listener?.onLog?.('Reconnect scan found no Spectre device');
      this.scheduleReconnect();
      return;
    }

    await this.connectToDevice(target.id);
  }

  private finishScan(generation: number) {
    if (generation !== this.scanGeneration) {
      return;
    }

    if (this.scanStopTimer) {
      clearTimeout(this.scanStopTimer);
      this.scanStopTimer = null;
    }

    if (this.isScanning) {
      this.manager.stopDeviceScan();
      this.isScanning = false;
    }

    const resolve = this.scanResolver;
    this.scanResolver = null;
    resolve?.(this.getDiscoveredDeviceList());
  }

  private cancelScan() {
    const generation = this.scanGeneration;
    if (generation === 0 && !this.isScanning && !this.scanResolver) {
      return;
    }

    this.scanGeneration += 1;
    this.finishScan(generation);
  }

  private clearReconnectTimer() {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
  }

  private teardownCharacteristicSubscriptions() {
    this.characteristicSubscriptions.forEach(subscription => {
      try {
        subscription.remove();
      } catch {}
    });
    this.characteristicSubscriptions = [];
  }

  private hasActivePromptSession() {
    return (
      !!this.promptState.promptText ||
      this.promptState.parsedStatus.input === 'PENDING' ||
      this.promptState.parsedStatus.input === 'READY'
    );
  }

  private getDiscoveredDeviceList() {
    return Array.from(this.discoveredDevices.values())
      .map(summarizeDevice)
      .sort((left, right) => {
        const rightRssi = right.rssi ?? -200;
        const leftRssi = left.rssi ?? -200;
        return rightRssi - leftRssi;
      });
  }

  private issueUploadToken() {
    const token = this.nextUploadToken % 0x10000;
    this.nextUploadToken = (token + 1) % 0x10000;
    return token === 0 ? 1 : token;
  }

  private computeUploadChunkSize() {
    const attPayload = Math.max(20, this.negotiatedMtu - 3);
    const dataBudget = Math.max(
      16,
      attPayload - BADUSB_UPLOAD_HEADER_BYTES,
    );
    return Math.min(BADUSB_UPLOAD_CHUNK_DATA_BYTES, dataBudget);
  }

  private async writeBadUsbCommand(command: string) {
    if (!this.device) {
      throw new Error('No Spectre device connected');
    }

    await this.device.writeCharacteristicWithResponseForService(
      SPECTRE_TEXT_SERVICE_UUID,
      SPECTRE_BADUSB_COMMAND_UUID,
      utf8ToBase64(command),
    );
  }

  private async writeBadUsbFrame(token: number, frameBase64: string) {
    if (!this.device) {
      throw new Error('No Spectre device connected');
    }

    try {
      await this.device.writeCharacteristicWithResponseForService(
        SPECTRE_TEXT_SERVICE_UUID,
        SPECTRE_BADUSB_UPLOAD_DATA_UUID,
        frameBase64,
      );
    } catch (error: any) {
      throw new Error(
        error?.message || `Chunk write failed for upload ${token}`,
      );
    }
  }

  private waitForUploadPhase(
    token: number,
    phases: BadUsbUploadPhase[],
    timeoutMs: number,
  ) {
    if (
      this.badUsbUploadState.token === token &&
      phases.includes(this.badUsbUploadState.phase)
    ) {
      return Promise.resolve(this.badUsbUploadState);
    }

    if (
      this.badUsbUploadState.token === token &&
      (this.badUsbUploadState.phase === 'error' ||
        this.badUsbUploadState.phase === 'cancelled')
    ) {
      return Promise.reject(
        new Error(this.badUsbUploadState.message || 'BadUSB transfer failed'),
      );
    }

    return new Promise<BadUsbUploadState>((resolve, reject) => {
      const waiter: UploadWaiter = {
        token,
        phases,
        resolve,
        reject,
        timer: setTimeout(() => {
          this.uploadWaiters.delete(waiter);
          reject(new Error('Timed out waiting for Spectre BadUSB receipt.'));
        }, timeoutMs),
      };

      this.uploadWaiters.add(waiter);
    });
  }

  private resolveUploadWaiters(state: BadUsbUploadState) {
    const completed: UploadWaiter[] = [];

    this.uploadWaiters.forEach(waiter => {
      if (waiter.token !== state.token) {
        return;
      }

      if (state.phase === 'error' || state.phase === 'cancelled') {
        clearTimeout(waiter.timer);
        waiter.reject(new Error(state.message || 'BadUSB transfer failed.'));
        completed.push(waiter);
        return;
      }

      if (waiter.phases.includes(state.phase)) {
        clearTimeout(waiter.timer);
        waiter.resolve(state);
        completed.push(waiter);
      }
    });

    completed.forEach(waiter => {
      this.uploadWaiters.delete(waiter);
    });
  }

  private clearUploadWaiters() {
    this.uploadWaiters.forEach(waiter => {
      clearTimeout(waiter.timer);
      waiter.reject(new Error('BadUSB transfer aborted.'));
    });
    this.uploadWaiters.clear();
  }

  private failActiveUpload(message: string) {
    if (
      this.badUsbUploadState.phase === 'idle' ||
      this.badUsbUploadState.phase === 'complete' ||
      this.badUsbUploadState.phase === 'cancelled' ||
      this.badUsbUploadState.phase === 'error'
    ) {
      return;
    }

    this.emitBadUsbUploadState({
      ...this.badUsbUploadState,
      phase: 'error',
      message,
      updatedAt: Date.now(),
    });
  }

  private async safeCancelUpload(token: number) {
    if (!this.device) {
      return;
    }

    try {
      await this.writeBadUsbCommand(encodeBadUsbCancelCommand(token));
    } catch {}
  }
}
