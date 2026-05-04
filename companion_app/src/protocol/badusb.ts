/* eslint-disable no-bitwise, no-control-regex */

import {
  BADUSB_DESC_MAX_BYTES,
  BADUSB_FILE_MAX_BYTES,
  BADUSB_NAME_MAX_BYTES,
  BADUSB_UPLOAD_CHUNK_DATA_BYTES,
  BADUSB_UPLOAD_COMMAND_MAX_BYTES,
  BADUSB_UPLOAD_HEADER_BYTES,
  BADUSB_UPLOAD_MAX_SCRIPT_BYTES,
  BADUSB_UPLOAD_PROTOCOL_VERSION,
} from './contracts';
import {bytesToBase64, utf8ToBytes} from './base64';
import {parseStatusBlob} from './binary';

export type BadUsbScriptDraft = {
  fileName: string;
  displayName: string;
  description: string;
  body: string;
};

export type NormalizedBadUsbScript = {
  fileName: string;
  displayName: string;
  description: string;
  body: string;
  bodyBytes: Uint8Array;
  totalBytes: number;
  totalChunks: number;
  lineCount: number;
  crc32: number;
  chunkSize: number;
  warnings: string[];
};

export type BadUsbUploadPhase =
  | 'idle'
  | 'ready'
  | 'uploading'
  | 'committing'
  | 'complete'
  | 'error'
  | 'cancelled';

export type BadUsbUploadState = {
  phase: BadUsbUploadPhase;
  rawStatus: string | null;
  token: number | null;
  fileName: string | null;
  message: string | null;
  receivedChunks: number;
  totalChunks: number;
  receivedBytes: number;
  totalBytes: number;
  available: boolean;
  updatedAt: number | null;
};

function sanitizeAsciiToken(value: string) {
  return value.replace(/[^a-zA-Z0-9._-]+/g, '_');
}

function baseNameFromFileName(fileName: string) {
  const trimmed = fileName.trim();
  if (!trimmed.length) {
    return 'payload';
  }

  return trimmed.replace(/\.txt$/i, '') || 'payload';
}

function humanizeFileName(fileName: string) {
  return baseNameFromFileName(fileName)
    .replace(/[_-]+/g, ' ')
    .trim();
}

function normalizeFileName(fileName: string, displayName: string) {
  const source = fileName.trim() || displayName.trim() || 'payload';
  let normalized = sanitizeAsciiToken(source).replace(/^_+|_+$/g, '');
  normalized = baseNameFromFileName(normalized);

  if (!normalized.length) {
    normalized = 'payload';
  }

  let fileNameWithExt = `${normalized}.txt`;
  while (utf8ToBytes(fileNameWithExt).length > BADUSB_FILE_MAX_BYTES) {
    normalized = normalized.slice(0, -1);
    if (!normalized.length) {
      normalized = 'payload';
      break;
    }
    fileNameWithExt = `${normalized}.txt`;
  }

  return fileNameWithExt;
}

function normalizeBody(body: string) {
  return body.replace(/\r\n?/g, '\n').replace(/\u0000/g, '');
}

export function countBadUsbLines(body: string) {
  return normalizeBody(body)
    .split('\n')
    .map(line => line.trim())
    .filter(Boolean).length;
}

export function crc32(bytes: Uint8Array) {
  let crc = 0xffffffff;

  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) {
      const mask = -(crc & 1);
      crc = (crc >>> 1) ^ (0xedb88320 & mask);
    }
  }

  return (crc ^ 0xffffffff) >>> 0;
}

export function normalizeBadUsbScriptDraft(
  draft: BadUsbScriptDraft,
  chunkSize = BADUSB_UPLOAD_CHUNK_DATA_BYTES,
): NormalizedBadUsbScript {
  const warnings: string[] = [];
  const fileName = normalizeFileName(draft.fileName, draft.displayName);
  const displayNameSource =
    draft.displayName.trim() || humanizeFileName(fileName) || 'Payload';
  const displayName = truncateUtf8(displayNameSource, BADUSB_NAME_MAX_BYTES);
  const description = truncateUtf8(
    draft.description.trim(),
    BADUSB_DESC_MAX_BYTES,
  );
  const body = normalizeBody(draft.body);
  const bodyBytes = utf8ToBytes(body);

  if (!body.trim().length || !bodyBytes.length) {
    throw new Error('BadUSB script body cannot be empty.');
  }

  if (bodyBytes.length > BADUSB_UPLOAD_MAX_SCRIPT_BYTES) {
    throw new Error(
      `BadUSB script exceeds the current phone contract (${bodyBytes.length}/${BADUSB_UPLOAD_MAX_SCRIPT_BYTES} bytes).`,
    );
  }

  if (displayName !== displayNameSource) {
    warnings.push('Display name was trimmed to fit Spectre.');
  }

  if (description !== draft.description.trim()) {
    warnings.push('Description was trimmed to fit Spectre.');
  }

  return {
    fileName,
    displayName,
    description,
    body,
    bodyBytes,
    totalBytes: bodyBytes.length,
    totalChunks: Math.max(1, Math.ceil(bodyBytes.length / chunkSize)),
    lineCount: countBadUsbLines(body),
    crc32: crc32(bodyBytes),
    chunkSize,
    warnings,
  };
}

export function truncateUtf8(value: string, maxBytes: number) {
  let output = '';

  for (const char of value) {
    const next = `${output}${char}`;
    if (utf8ToBytes(next).length > maxBytes) {
      break;
    }
    output = next;
  }

  return output;
}

function ensureCommandSize(command: string) {
  const length = utf8ToBytes(command).length;
  if (length > BADUSB_UPLOAD_COMMAND_MAX_BYTES) {
    throw new Error(
      `BadUSB upload command exceeds the device contract (${length}/${BADUSB_UPLOAD_COMMAND_MAX_BYTES} bytes).`,
    );
  }
  return command;
}

export function encodeBadUsbBeginCommand(
  token: number,
  script: NormalizedBadUsbScript,
) {
  let displayName = script.displayName;
  let description = script.description;

  while (true) {
    const command = JSON.stringify({
      op: 'begin',
      v: BADUSB_UPLOAD_PROTOCOL_VERSION,
      tok: token & 0xffff,
      f: script.fileName,
      n: displayName,
      d: description,
      bytes: script.totalBytes,
      chunks: script.totalChunks,
      crc: script.crc32 >>> 0,
      lines: script.lineCount,
    });

    const length = utf8ToBytes(command).length;
    if (length <= BADUSB_UPLOAD_COMMAND_MAX_BYTES) {
      return command;
    }

    if (description.length > 0) {
      description = truncateUtf8(
        description.slice(0, Math.max(0, description.length - 1)),
        BADUSB_DESC_MAX_BYTES,
      );
      continue;
    }

    if (displayName.length > 1) {
      displayName = truncateUtf8(
        displayName.slice(0, Math.max(1, displayName.length - 1)),
        BADUSB_NAME_MAX_BYTES,
      );
      continue;
    }

    throw new Error(
      `BadUSB upload command exceeds the device contract (${length}/${BADUSB_UPLOAD_COMMAND_MAX_BYTES} bytes).`,
    );
  }
}

export function encodeBadUsbCommitCommand(token: number) {
  return ensureCommandSize(
    JSON.stringify({
      op: 'commit',
      v: BADUSB_UPLOAD_PROTOCOL_VERSION,
      tok: token & 0xffff,
    }),
  );
}

export function encodeBadUsbCancelCommand(token: number) {
  return ensureCommandSize(
    JSON.stringify({
      op: 'cancel',
      v: BADUSB_UPLOAD_PROTOCOL_VERSION,
      tok: token & 0xffff,
    }),
  );
}

export function buildBadUsbChunkFrames(
  token: number,
  script: NormalizedBadUsbScript,
) {
  const frames: string[] = [];

  for (let index = 0; index < script.totalChunks; index += 1) {
    const start = index * script.chunkSize;
    const end = Math.min(start + script.chunkSize, script.bodyBytes.length);
    const payload = script.bodyBytes.subarray(start, end);
    const frame = new Uint8Array(BADUSB_UPLOAD_HEADER_BYTES + payload.length);
    const view = new DataView(frame.buffer);

    view.setUint8(0, BADUSB_UPLOAD_PROTOCOL_VERSION);
    view.setUint16(1, token & 0xffff, true);
    view.setUint16(3, index & 0xffff, true);
    view.setUint16(5, script.totalChunks & 0xffff, true);
    view.setUint16(7, payload.length & 0xffff, true);
    frame.set(payload, BADUSB_UPLOAD_HEADER_BYTES);
    frames.push(bytesToBase64(frame));
  }

  return frames;
}

function parseRatio(value?: string) {
  if (!value) {
    return {
      current: 0,
      total: 0,
    };
  }

  const [current, total] = value.split('/');
  return {
    current: Number.parseInt(current || '0', 10) || 0,
    total: Number.parseInt(total || '0', 10) || 0,
  };
}

export function emptyBadUsbUploadState(): BadUsbUploadState {
  return {
    phase: 'idle',
    rawStatus: null,
    token: null,
    fileName: null,
    message: null,
    receivedChunks: 0,
    totalChunks: 0,
    receivedBytes: 0,
    totalBytes: 0,
    available: false,
    updatedAt: null,
  };
}

export function parseBadUsbUploadStatus(
  rawStatus?: string | null,
): Partial<BadUsbUploadState> {
  const parsed = parseStatusBlob(rawStatus || '');
  const chunkRatio = parseRatio(parsed.c);
  const byteRatio = parseRatio(parsed.b);

  let phase: BadUsbUploadPhase = 'idle';
  switch ((parsed.p || '').toUpperCase()) {
    case 'READY':
      phase = 'ready';
      break;
    case 'RECV':
      phase = 'uploading';
      break;
    case 'COMMIT':
      phase = 'committing';
      break;
    case 'DONE':
      phase = 'complete';
      break;
    case 'ERROR':
      phase = 'error';
      break;
    case 'CANCEL':
      phase = 'cancelled';
      break;
    case 'IDLE':
    default:
      phase = 'idle';
      break;
  }

  return {
    phase,
    rawStatus: rawStatus ?? null,
    token:
      typeof parsed.tok === 'string' && parsed.tok.length
        ? Number(parsed.tok)
        : null,
    fileName: parsed.f || null,
    message: parsed.m || null,
    receivedChunks: chunkRatio.current,
    totalChunks: chunkRatio.total,
    receivedBytes: byteRatio.current,
    totalBytes: byteRatio.total,
    available: !!rawStatus,
    updatedAt: Date.now(),
  };
}
