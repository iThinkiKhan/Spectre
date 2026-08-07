// Custom ESM resolve hook so we can import the REAL companion-app source
// (binary.ts, LocationHistoryStore.ts) under plain Node, without React Native.
//
// Two jobs:
//  1. Map the three RN-only bare/relative specifiers to local stubs. The
//     matching/encoding functions under test don't use them, but they're
//     top-level imports so the module graph won't load without them.
//  2. The source uses extensionless relative imports ('./base64'); Node's ESM
//     resolver needs an explicit extension, so append '.ts' when the bare path
//     resolves to a .ts file on disk.
import {existsSync} from 'node:fs';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {dirname, resolve as pathResolve} from 'node:path';

const STUBS = {
  'react-native': new URL('./stubs/react-native.mjs', import.meta.url).href,
  'react-native-geolocation-service': new URL(
    './stubs/geolocation.mjs',
    import.meta.url,
  ).href,
};

export async function resolve(specifier, context, nextResolve) {
  if (STUBS[specifier]) {
    return {url: STUBS[specifier], shortCircuit: true};
  }

  // ../storage/NativeKeyValueStore (persistence, unused by matching)
  if (specifier.includes('storage/NativeKeyValueStore')) {
    return {
      url: new URL('./stubs/kv.mjs', import.meta.url).href,
      shortCircuit: true,
    };
  }

  // Extensionless relative import -> try .ts
  if (specifier.startsWith('.') && !/\.[a-z]+$/i.test(specifier)) {
    const parentPath = fileURLToPath(context.parentURL);
    const candidate = pathResolve(dirname(parentPath), `${specifier}.ts`);
    if (existsSync(candidate)) {
      return {url: pathToFileURL(candidate).href, shortCircuit: true};
    }
  }

  return nextResolve(specifier, context);
}
