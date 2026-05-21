# Spectre Companion

Android-first companion app for Spectre field units.

This scaffold is intentionally built around one core reality:

- Spectre is the primary platform.
- The phone is an enrichment relay and durable storage extension.
- Spectre must survive without the phone present

## Key design choices

- `react-native-ble-plx` is used for the phone-as-client role only.
- The phone-as-server BLE role is implemented through a native Android GATT server bridge because `react-native-ble-plx` does not support peripheral mode.
- GPS logging uses `react-native-geolocation-service` instead of the older community geolocation split because Android reliability is materially better with fused location.
- Broker credentials are stored in Android Keystore via `react-native-keychain`.
- Raw events are stored immediately and enriched later.

## Folder map

- `src/protocol/`: UUIDs, binary packing, wire types
- `src/services/`: BLE, GPS, SQLite, upload, orchestration
- `src/ui/`: Spectre-themed app shell and screens
- `android/`: native Android BLE peripheral bridge and manifest notes

## Current status

This is a production-oriented scaffold, not a finished shipping app. The hardest unsupported BLE role is already accounted for in the architecture through the native bridge.
