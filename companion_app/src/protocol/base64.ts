/* eslint-disable no-bitwise */

const BASE64_ALPHABET =
  'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

const reverseLookup = (() => {
  const table = new Int16Array(256).fill(-1);
  for (let index = 0; index < BASE64_ALPHABET.length; index += 1) {
    table[BASE64_ALPHABET.charCodeAt(index)] = index;
  }
  table['='.charCodeAt(0)] = 0;
  return table;
})();

export function bytesToBase64(bytes: Uint8Array): string {
  let output = '';

  for (let index = 0; index < bytes.length; index += 3) {
    const first = bytes[index] ?? 0;
    const second = bytes[index + 1] ?? 0;
    const third = bytes[index + 2] ?? 0;
    const chunk = (first << 16) | (second << 8) | third;

    output += BASE64_ALPHABET[(chunk >> 18) & 0x3f];
    output += BASE64_ALPHABET[(chunk >> 12) & 0x3f];
    output += index + 1 < bytes.length ? BASE64_ALPHABET[(chunk >> 6) & 0x3f] : '=';
    output += index + 2 < bytes.length ? BASE64_ALPHABET[chunk & 0x3f] : '=';
  }

  return output;
}

export function base64ToBytes(value: string): Uint8Array {
  const sanitized = value.replace(/[^A-Za-z0-9+/=]/g, '');
  if (!sanitized.length) {
    return new Uint8Array(0);
  }

  const outputLength = Math.floor((sanitized.length * 3) / 4) -
    (sanitized.endsWith('==') ? 2 : sanitized.endsWith('=') ? 1 : 0);
  const bytes = new Uint8Array(Math.max(outputLength, 0));

  let outputIndex = 0;

  for (let index = 0; index < sanitized.length; index += 4) {
    const first = reverseLookup[sanitized.charCodeAt(index)];
    const second = reverseLookup[sanitized.charCodeAt(index + 1)];
    const third = reverseLookup[sanitized.charCodeAt(index + 2)];
    const fourth = reverseLookup[sanitized.charCodeAt(index + 3)];

    if (first < 0 || second < 0 || third < 0 || fourth < 0) {
      throw new Error('Invalid base64 payload');
    }

    const chunk = (first << 18) | (second << 12) | (third << 6) | fourth;

    if (outputIndex < bytes.length) {
      bytes[outputIndex] = (chunk >> 16) & 0xff;
      outputIndex += 1;
    }
    if (outputIndex < bytes.length) {
      bytes[outputIndex] = (chunk >> 8) & 0xff;
      outputIndex += 1;
    }
    if (outputIndex < bytes.length) {
      bytes[outputIndex] = chunk & 0xff;
      outputIndex += 1;
    }
  }

  return bytes;
}

export function utf8ToBytes(value: string): Uint8Array {
  const encoded = encodeURIComponent(value);
  const bytes: number[] = [];

  for (let index = 0; index < encoded.length; index += 1) {
    const char = encoded[index];
    if (char === '%') {
      bytes.push(parseInt(encoded.slice(index + 1, index + 3), 16));
      index += 2;
      continue;
    }

    bytes.push(char.charCodeAt(0));
  }

  return Uint8Array.from(bytes);
}

export function bytesToUtf8(bytes: Uint8Array): string {
  let encoded = '';

  bytes.forEach(byte => {
    if (byte < 0x80) {
      encoded += String.fromCharCode(byte);
      return;
    }

    encoded += `%${byte.toString(16).padStart(2, '0').toUpperCase()}`;
  });

  return decodeURIComponent(encoded);
}

export function utf8ToBase64(value: string): string {
  return bytesToBase64(utf8ToBytes(value));
}

export function base64ToUtf8(value?: string | null): string | null {
  if (!value) {
    return null;
  }

  try {
    return bytesToUtf8(base64ToBytes(value));
  } catch {
    return null;
  }
}
