import type {PromptKind} from '../../protocol/contracts';
import type {BadUsbUploadState} from '../../protocol/badusb';

export type SpectreConnectionState =
  | 'idle'
  | 'scanning'
  | 'connecting'
  | 'connected'
  | 'reconnecting'
  | 'disconnected'
  | 'error';

export type SpectreDeviceSummary = {
  id: string;
  name: string;
  localName: string | null;
  rssi: number | null;
  lastSeenMs: number;
};

export type SpectrePromptState = {
  promptText: string | null;
  pending: boolean;
  awaitingReply: boolean;
  submittedReply: boolean;
  receipt: string | null;
  rawStatus: string | null;
  parsedStatus: Record<string, string>;
  token: number | null;
  promptKind: PromptKind;
  replyError: string | null;
  updatedAt: number | null;
};

export type SpectreStatusSnapshot = {
  sessionId: string | null;
  linkState: number | null;
  linkStateLabel: string;
  gpsValid: boolean;
  inputState: string | null;
  wireGuardState: string | null;
  token: number | null;
};

export type BleClientListener = {
  onAdapterState?: (state: string) => void;
  onScanUpdate?: (devices: SpectreDeviceSummary[]) => void;
  onConnectionChange?: (
    state: SpectreConnectionState,
    detail?: string,
    device?: SpectreDeviceSummary | null,
  ) => void;
  onPromptChange?: (prompt: SpectrePromptState) => void;
  onBadUsbUploadChange?: (state: BadUsbUploadState) => void;
  onLog?: (message: string) => void;
};
