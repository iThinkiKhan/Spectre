/**
 * @format
 */

import React from 'react';
import ReactTestRenderer from 'react-test-renderer';
import App from '../App';

jest.mock('react-native-ble-plx', () => {
  class MockBleManager {
    onStateChange(listener: (state: string) => void) {
      listener('PoweredOn');
      return {remove() {}};
    }

    startDeviceScan() {}
    stopDeviceScan() {}
    cancelDeviceConnection() {
      return Promise.resolve();
    }
    destroy() {
      return Promise.resolve();
    }
  }

  return {
    BleManager: MockBleManager,
  };
});

test('renders correctly', async () => {
  await ReactTestRenderer.act(() => {
    ReactTestRenderer.create(<App />);
  });
});
