import {NativeModules, Platform} from 'react-native';

type NativeKeyValueStoreModule = {
  getString: (key: string) => Promise<string | null>;
  setString: (key: string, value: string) => Promise<boolean>;
  remove: (key: string) => Promise<boolean>;
};

const memoryStore = new Map<string, string>();

const nativeStore =
  Platform.OS === 'android'
    ? (NativeModules.SpectreKeyValueStore as NativeKeyValueStoreModule | undefined)
    : undefined;

export async function getStoredString(key: string): Promise<string | null> {
  if (nativeStore) {
    return nativeStore.getString(key);
  }
  return memoryStore.get(key) ?? null;
}

export async function setStoredString(
  key: string,
  value: string,
): Promise<void> {
  if (nativeStore) {
    await nativeStore.setString(key, value);
    return;
  }
  memoryStore.set(key, value);
}

export async function removeStoredString(key: string): Promise<void> {
  if (nativeStore) {
    await nativeStore.remove(key);
    return;
  }
  memoryStore.delete(key);
}
