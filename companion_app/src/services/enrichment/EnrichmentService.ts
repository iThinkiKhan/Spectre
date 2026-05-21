import {decodeEventBatchRecords} from '../../protocol/binary';
import {validateEventBatchPayloadLength} from '../../protocol/contracts';
import type {EventBatchRecord} from '../../protocol/types';
import type {PeripheralEventBatch} from '../peripheral/SpectrePeripheralBridge';

type BatchSource = 'spectre' | 'mock';

export type FinalizedEventBatch = {
  base64: string;
  events: EventBatchRecord[];
  receivedAt: number;
  source: BatchSource;
};

type Callbacks = {
  onBatchReady: (batch: FinalizedEventBatch) => void;
  onLog?: (message: string, level?: 'info' | 'warn' | 'error') => void;
};

const SPECTRE_BATCH_SETTLE_MS = 150;
const DEDUPE_WINDOW_MS = 1500;

export class EnrichmentService {
  private readonly callbacks: Callbacks;
  private settleTimer: ReturnType<typeof setTimeout> | null = null;
  private pendingSpectreBatch: PeripheralEventBatch | null = null;
  private lastDeliveredKey: string | null = null;
  private lastDeliveredAt = 0;

  constructor(callbacks: Callbacks) {
    this.callbacks = callbacks;
  }

  ingest(payload: PeripheralEventBatch, source: BatchSource) {
    if (source === 'mock') {
      this.finalize(payload, source);
      return;
    }

    this.pendingSpectreBatch = payload;
    this.clearTimer();
    this.settleTimer = setTimeout(() => {
      const batch = this.pendingSpectreBatch;
      this.pendingSpectreBatch = null;
      if (batch) {
        this.finalize(batch, 'spectre');
      }
    }, SPECTRE_BATCH_SETTLE_MS);
  }

  destroy() {
    this.clearTimer();
    this.pendingSpectreBatch = null;
  }

  private finalize(payload: PeripheralEventBatch, source: BatchSource) {
    const validation = validateEventBatchPayloadLength(payload.length);
    if (!validation.ok) {
      this.callbacks.onLog?.(validation.reason, 'warn');
      return;
    }

    let events: EventBatchRecord[];
    try {
      events = decodeEventBatchRecords(payload.base64);
    } catch (error: any) {
      this.callbacks.onLog?.(
        error?.message || 'Failed to decode Spectre event batch payload.',
        'error',
      );
      return;
    }

    if (events.length !== validation.recordCount) {
      this.callbacks.onLog?.(
        `Event batch decode mismatch (${events.length}/${validation.recordCount} records).`,
        'warn',
      );
      return;
    }

    const batchKey = `${source}:${payload.length}:${payload.base64}`;
    if (
      batchKey === this.lastDeliveredKey &&
      Date.now() - this.lastDeliveredAt < DEDUPE_WINDOW_MS
    ) {
      return;
    }

    this.lastDeliveredKey = batchKey;
    this.lastDeliveredAt = Date.now();

    this.callbacks.onBatchReady({
      base64: payload.base64,
      events,
      receivedAt: payload.receivedAt,
      source,
    });
  }

  private clearTimer() {
    if (this.settleTimer) {
      clearTimeout(this.settleTimer);
      this.settleTimer = null;
    }
  }
}
