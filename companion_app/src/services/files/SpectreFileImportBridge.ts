import {NativeModules, Platform} from 'react-native';

type NativeFileImportModule = {
  pickTextFile(): Promise<ImportedBadUsbFile | null>;
};

export type ImportedBadUsbFile = {
  name: string;
  size: number;
  mimeType?: string | null;
  text: string;
};

const nativeModule =
  Platform.OS === 'android'
    ? (NativeModules.SpectreFileImport as NativeFileImportModule | undefined)
    : undefined;

export class SpectreFileImportBridge {
  isAvailable() {
    return !!nativeModule;
  }

  async pickTextFile() {
    if (!nativeModule) {
      throw new Error('Native Android file import is unavailable on this device.');
    }

    return nativeModule.pickTextFile();
  }
}

export const spectreFileImportBridge = new SpectreFileImportBridge();
