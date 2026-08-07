import {
  CMD_OP_DEBRIEF_REQUEST,
  CMD_OP_ENRICH_NOW,
  CMD_OP_GET_DASHBOARD,
  CMD_OP_GET_HEALTH,
  CMD_OP_GET_LOG_TAIL,
  CMD_OP_GET_STATUS,
  CMD_OP_GET_STORAGE,
  CMD_OP_GET_WIO_STATUS,
  CMD_OP_SAVE_LOCATION,
  CMD_OP_SCREEN_CHANGE,
  CMD_OP_START_DASHBOARD_STREAM,
  CMD_OP_START_LOG_STREAM,
  CMD_OP_STOP_DASHBOARD_STREAM,
  CMD_OP_STOP_LOG_STREAM,
  CMD_OP_TAG_SESSION,
  CMD_OP_UPLOAD_NOW,
  CMD_STATUS_OK,
  PHONE_COMMAND_VERSION,
  PHONE_DASHBOARD_STREAM_DEFAULT_LEASE_MS,
  PHONE_DASHBOARD_STREAM_INTERVAL_DEFAULT_MS,
  PHONE_LOG_STREAM_DEFAULT_LEASE_MS,
} from '../../protocol/contracts';
import {
  decodeCmdDashboardResponse,
  decodeCmdHealthResponse,
  decodeCmdLogTailResponse,
  decodeCmdStartDashboardStreamResponse,
  decodeCmdStartLogStreamResponse,
  decodeCmdStatusResponse,
  decodeCmdStorageResponse,
  decodeCmdWioStatusResponse,
  decodeDashboardStreamChunk,
  decodeLogStreamChunk,
  decodePhoneCommandResponse,
  encodeCmdScreenChangeRequest,
  encodeCmdStartDashboardStreamRequest,
  encodeCmdStartLogStreamRequest,
  encodeCmdStopDashboardStreamRequest,
  encodeCmdStopLogStreamRequest,
  encodeCmdTagPayload,
  encodePhoneCommandRequest,
} from '../../protocol/binary';
import type {
  CmdDashboardSnapshotV1,
  CmdHealthResponseV1,
  CmdLogTailResponseV1,
  CmdStartDashboardStreamResponseV1,
  CmdStartLogStreamResponseV1,
  CmdStatusResponseV1,
  CmdWioStatusResponseV1,
  DashboardStreamChunkV1,
  LogStreamChunkV1,
  PhoneStorageFrameV1,
} from '../../protocol/types';
import type {
  PeripheralCommandResponseEvent,
  PeripheralDashboardStreamEvent,
  PeripheralLogStreamEvent,
  SpectrePeripheralBridge,
} from './SpectrePeripheralBridge';

// Request/response wrapper around the peripheral command channel.

export const COMMAND_DEFAULT_TIMEOUT_MS = 8000;

export class CommandError extends Error {
  readonly opcode: number;
  readonly requestId: number;
  readonly status: number;

  constructor(opcode: number, requestId: number, status: number, message: string) {
    super(message);
    this.name = 'CommandError';
    this.opcode = opcode;
    this.requestId = requestId;
    this.status = status;
  }
}

export class CommandTimeoutError extends Error {
  readonly opcode: number;
  readonly requestId: number;

  constructor(opcode: number, requestId: number, timeoutMs: number) {
    super(`Command opcode=0x${opcode.toString(16)} reqId=${requestId} timed out after ${timeoutMs}ms`);
    this.name = 'CommandTimeoutError';
    this.opcode = opcode;
    this.requestId = requestId;
  }
}

type Pending = {
  opcode: number;
  resolve: (payload: Uint8Array) => void;
  reject: (reason: unknown) => void;
  timeoutHandle: ReturnType<typeof setTimeout>;
  issuedAt: number;
};

export type LogStreamHandle = {
  streamId: number;
  grantedLineCap: number;
  grantedDurationMs: number;
  // Cancels the subscription and asks the device to stop streaming.  Returns
  // the device's STOP response promise (rejected on transport failure).
  stop: () => Promise<void>;
};

type LogStreamSubscription = {
  onChunk: (chunk: LogStreamChunkV1) => void;
  onEnd?: (reason: 'ended' | 'cancelled' | 'transport') => void;
};

export type DashboardStreamHandle = {
  streamId: number;
  grantedIntervalMs: number;
  grantedDurationMs: number;
  stop: () => Promise<void>;
};

type DashboardStreamSubscription = {
  onSnapshot: (snapshot: CmdDashboardSnapshotV1, chunk: DashboardStreamChunkV1) => void;
  onEnd?: (reason: 'ended' | 'cancelled' | 'transport') => void;
};

export class SpectreCommandService {
  private readonly bridge: SpectrePeripheralBridge;
  private readonly pending = new Map<number, Pending>();
  private readonly streamSubscriptions = new Map<number, LogStreamSubscription>();
  private readonly dashboardSubscriptions = new Map<number, DashboardStreamSubscription>();
  // 1..0xFFFF; 0 is reserved (a peer that echoes 0 indicates "no request").
  private nextRequestId = 1;
  private defaultTimeoutMs: number;

  constructor(bridge: SpectrePeripheralBridge, defaultTimeoutMs = COMMAND_DEFAULT_TIMEOUT_MS) {
    this.bridge = bridge;
    this.defaultTimeoutMs = defaultTimeoutMs;
  }

  handleResponseEvent(event: PeripheralCommandResponseEvent) {
    let response;
    try {
      response = decodePhoneCommandResponse(event.base64);
    } catch (error) {
      // Malformed envelope — there's no requestId to route to, so we can't
      // resolve anything.  Caller logs via the bridge log channel.
      return;
    }

    const pending = this.pending.get(response.requestId);
    if (!pending) {
      // Response for a request we already timed out, or unknown id.  Discard.
      return;
    }
    this.pending.delete(response.requestId);
    clearTimeout(pending.timeoutHandle);

    if (response.opcode !== pending.opcode) {
      pending.reject(
        new CommandError(
          response.opcode,
          response.requestId,
          response.status,
          `Opcode mismatch (sent 0x${pending.opcode.toString(16)}, got 0x${response.opcode.toString(16)})`,
        ),
      );
      return;
    }

    if (response.status !== CMD_STATUS_OK) {
      pending.reject(
        new CommandError(
          response.opcode,
          response.requestId,
          response.status,
          `Device returned non-OK status=${response.status}`,
        ),
      );
      return;
    }

    pending.resolve(response.payload);
  }

  cancelAllPending(reason = 'Command channel closed') {
    for (const [requestId, entry] of this.pending) {
      clearTimeout(entry.timeoutHandle);
      entry.reject(new CommandError(entry.opcode, requestId, 0xff, reason));
    }
    this.pending.clear();

    for (const [, sub] of this.streamSubscriptions) {
      sub.onEnd?.('transport');
    }
    this.streamSubscriptions.clear();

    for (const [, sub] of this.dashboardSubscriptions) {
      sub.onEnd?.('transport');
    }
    this.dashboardSubscriptions.clear();
  }

  handleLogStreamEvent(event: PeripheralLogStreamEvent) {
    let chunk: LogStreamChunkV1;
    try {
      chunk = decodeLogStreamChunk(event.base64);
    } catch {
      return;
    }
    const sub = this.streamSubscriptions.get(chunk.streamId);
    if (!sub) {
      return;
    }
    sub.onChunk(chunk);
    if (chunk.ending) {
      this.streamSubscriptions.delete(chunk.streamId);
      sub.onEnd?.('ended');
    }
  }

  handleDashboardStreamEvent(event: PeripheralDashboardStreamEvent) {
    let chunk: DashboardStreamChunkV1;
    try {
      chunk = decodeDashboardStreamChunk(event.base64);
    } catch {
      return;
    }
    const sub = this.dashboardSubscriptions.get(chunk.streamId);
    if (!sub) {
      return;
    }
    sub.onSnapshot(chunk.snapshot, chunk);
    if (chunk.ending) {
      this.dashboardSubscriptions.delete(chunk.streamId);
      sub.onEnd?.('ended');
    }
  }

  async sendRaw(
    opcode: number,
    payload: Uint8Array = new Uint8Array(0),
    timeoutMs = this.defaultTimeoutMs,
  ): Promise<Uint8Array> {
    if (!this.bridge.isCommandChannelAvailable()) {
      throw new Error('Command channel native bridge unavailable');
    }

    const requestId = this.allocateRequestId();
    const encoded = encodePhoneCommandRequest({
      version: PHONE_COMMAND_VERSION,
      opcode,
      requestId,
      payload,
    });

    return new Promise<Uint8Array>((resolve, reject) => {
      const timeoutHandle = setTimeout(() => {
        if (this.pending.delete(requestId)) {
          reject(new CommandTimeoutError(opcode, requestId, timeoutMs));
        }
      }, timeoutMs);

      this.pending.set(requestId, {
        opcode,
        resolve,
        reject,
        timeoutHandle,
        issuedAt: Date.now(),
      });

      this.bridge.sendCommandRequest(encoded).catch(error => {
        if (this.pending.delete(requestId)) {
          clearTimeout(timeoutHandle);
          reject(error);
        }
      });
    });
  }

  async getStatus(timeoutMs?: number): Promise<CmdStatusResponseV1> {
    const payload = await this.sendRaw(CMD_OP_GET_STATUS, undefined, timeoutMs);
    return decodeCmdStatusResponse(payload);
  }

  async getHealth(timeoutMs?: number): Promise<CmdHealthResponseV1> {
    const payload = await this.sendRaw(CMD_OP_GET_HEALTH, undefined, timeoutMs);
    return decodeCmdHealthResponse(payload);
  }

  async getStorage(timeoutMs?: number): Promise<PhoneStorageFrameV1> {
    const payload = await this.sendRaw(CMD_OP_GET_STORAGE, undefined, timeoutMs);
    return decodeCmdStorageResponse(payload);
  }

  async getWioStatus(timeoutMs?: number): Promise<CmdWioStatusResponseV1> {
    const payload = await this.sendRaw(CMD_OP_GET_WIO_STATUS, undefined, timeoutMs);
    return decodeCmdWioStatusResponse(payload);
  }

  async getLogTail(timeoutMs?: number): Promise<CmdLogTailResponseV1> {
    const payload = await this.sendRaw(CMD_OP_GET_LOG_TAIL, undefined, timeoutMs);
    return decodeCmdLogTailResponse(payload);
  }

  async getDashboard(timeoutMs?: number): Promise<CmdDashboardSnapshotV1> {
    const payload = await this.sendRaw(CMD_OP_GET_DASHBOARD, undefined, timeoutMs);
    return decodeCmdDashboardResponse(payload);
  }

  // ── Slice #3 — log streaming ──────────────────────────────────────────────

  async startLogStream(options: {
    durationMs?: number;
    lineCap?: number;
    onChunk: (chunk: LogStreamChunkV1) => void;
    onEnd?: (reason: 'ended' | 'cancelled' | 'transport') => void;
    timeoutMs?: number;
  }): Promise<LogStreamHandle> {
    const request = encodeCmdStartLogStreamRequest({
      leaseDurationMs: options.durationMs ?? PHONE_LOG_STREAM_DEFAULT_LEASE_MS,
      lineCap: options.lineCap ?? 0,
    });

    const payload = await this.sendRaw(
      CMD_OP_START_LOG_STREAM,
      request,
      options.timeoutMs,
    );
    const resp: CmdStartLogStreamResponseV1 =
      decodeCmdStartLogStreamResponse(payload);

    if (resp.streamId === 0) {
      throw new CommandError(
        CMD_OP_START_LOG_STREAM,
        0,
        0xff,
        'Device returned streamId=0',
      );
    }

    this.streamSubscriptions.set(resp.streamId, {
      onChunk: options.onChunk,
      onEnd: options.onEnd,
    });

    return {
      streamId: resp.streamId,
      grantedLineCap: resp.grantedLineCap,
      grantedDurationMs: resp.grantedDurationMs,
      stop: () => this.stopLogStream(resp.streamId),
    };
  }

  async stopLogStream(streamId: number, timeoutMs?: number): Promise<void> {
    const sub = this.streamSubscriptions.get(streamId);
    // We intentionally don't remove the subscription here — the device's
    // final chunk (ending=true) will trigger removal + onEnd via the chunk
    // handler.  That keeps the lifecycle uniform with lease-expiry.

    const request = encodeCmdStopLogStreamRequest({streamId});
    try {
      await this.sendRaw(CMD_OP_STOP_LOG_STREAM, request, timeoutMs);
    } catch (error) {
      // Best-effort: if the device rejects (already stopped, etc.), still
      // tear down our local subscription so the UI unsticks.
      if (this.streamSubscriptions.delete(streamId)) {
        sub?.onEnd?.('cancelled');
      }
      throw error;
    }
  }

  async startDashboardStream(options: {
    durationMs?: number;
    intervalMs?: number;
    onSnapshot: (snapshot: CmdDashboardSnapshotV1, chunk: DashboardStreamChunkV1) => void;
    onEnd?: (reason: 'ended' | 'cancelled' | 'transport') => void;
    timeoutMs?: number;
  }): Promise<DashboardStreamHandle> {
    const request = encodeCmdStartDashboardStreamRequest({
      leaseDurationMs: options.durationMs ?? PHONE_DASHBOARD_STREAM_DEFAULT_LEASE_MS,
      intervalMs: options.intervalMs ?? PHONE_DASHBOARD_STREAM_INTERVAL_DEFAULT_MS,
    });

    const payload = await this.sendRaw(
      CMD_OP_START_DASHBOARD_STREAM,
      request,
      options.timeoutMs,
    );
    const resp: CmdStartDashboardStreamResponseV1 =
      decodeCmdStartDashboardStreamResponse(payload);

    if (resp.streamId === 0) {
      throw new CommandError(
        CMD_OP_START_DASHBOARD_STREAM,
        0,
        0xff,
        'Device returned streamId=0',
      );
    }

    this.dashboardSubscriptions.set(resp.streamId, {
      onSnapshot: options.onSnapshot,
      onEnd: options.onEnd,
    });

    return {
      streamId: resp.streamId,
      grantedIntervalMs: resp.grantedIntervalMs,
      grantedDurationMs: resp.grantedDurationMs,
      stop: () => this.stopDashboardStream(resp.streamId),
    };
  }

  async stopDashboardStream(streamId: number, timeoutMs?: number): Promise<void> {
    const sub = this.dashboardSubscriptions.get(streamId);
    const request = encodeCmdStopDashboardStreamRequest({streamId});
    try {
      await this.sendRaw(CMD_OP_STOP_DASHBOARD_STREAM, request, timeoutMs);
    } catch (error) {
      if (this.dashboardSubscriptions.delete(streamId)) {
        sub?.onEnd?.('cancelled');
      }
      throw error;
    }
  }

  // ── Slice #5 — safe write commands ────────────────────────────────────────

  async enrichNow(timeoutMs?: number): Promise<void> {
    await this.sendRaw(CMD_OP_ENRICH_NOW, undefined, timeoutMs);
  }

  async uploadNow(timeoutMs?: number): Promise<void> {
    await this.sendRaw(CMD_OP_UPLOAD_NOW, undefined, timeoutMs);
  }

  async tagSession(tag: string, timeoutMs?: number): Promise<void> {
    await this.sendRaw(CMD_OP_TAG_SESSION, encodeCmdTagPayload(tag), timeoutMs);
  }

  async saveLocation(tag: string, timeoutMs?: number): Promise<void> {
    await this.sendRaw(CMD_OP_SAVE_LOCATION, encodeCmdTagPayload(tag), timeoutMs);
  }

  async setScreen(targetScreen: number, timeoutMs?: number): Promise<void> {
    await this.sendRaw(
      CMD_OP_SCREEN_CHANGE,
      encodeCmdScreenChangeRequest(targetScreen),
      timeoutMs,
    );
  }

  async requestDebrief(timeoutMs?: number): Promise<void> {
    await this.sendRaw(CMD_OP_DEBRIEF_REQUEST, undefined, timeoutMs);
  }

  private allocateRequestId(): number {
    // Wrap around 0xFFFF, skipping 0.  In-flight collisions are theoretically
    // possible after 65535 unanswered requests but extremely unlikely in
    // practice — the request map would already be cleared by timeouts.
    for (let i = 0; i < 0x10000; i++) {
      const candidate = this.nextRequestId;
      this.nextRequestId = (this.nextRequestId + 1) & 0xffff;
      if (this.nextRequestId === 0) {
        this.nextRequestId = 1;
      }
      if (!this.pending.has(candidate)) {
        return candidate;
      }
    }
    throw new Error('Command request id space exhausted');
  }
}
