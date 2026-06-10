# Spectre WIO nRF Relay Firmware

Clean-base firmware for the Seeed XIAO nRF52840 + Wio-SX1262 accessory.

The relay sits between the Spectre ESP32-S3 firmware and the Android companion app:

- ESP32-S3 talks to this firmware over UART at 115200 baud.
- This firmware acts as a BLE central/client.
- The Android companion app acts as a BLE peripheral/server for the `PHONE_*` service.
- Payload bytes remain opaque to the relay. Auth and encrypted secure-channel frames are chunked and forwarded, not decrypted here.

## UART wiring

Matches the controller-side comments in `src/config.h`:

- ESP TX GPIO 1 -> XIAO D7 RX
- ESP RX GPIO 2 <- XIAO D6 TX
- GND shared
- Baud: 115200

The PlatformIO env uses the local `seeed_xiao_nrf52840_sense` board definition and local Seeed XIAO variant, so `Serial1` maps to XIAO D6/D7 instead of an Adafruit Feather pinout.

## Implemented protocol

ESP -> WIO:

- `SPECTRE/1 HELLO`
- `SPECTRE/1 BLE_STATUS`
- `SPECTRE/1 BLE_PROBE id=<n> timeout=<ms>`
- `SPECTRE/1 BLE_WRITE id=<n> char=<name> len=<n> hex=<hex>`
- `SPECTRE/1 BLE_WRITE_BIN id=<n> char=<name> len=<n>` followed by raw bytes
- `SPECTRE/1 BLE_DROP`

WIO -> ESP:

- `WIO/1 CAPS BLE_PROXY UART BLE_WRITE_BIN`
- `WIO/1 STATUS phone=<state> connected=<0|1> rssi=<dbm>`
- `WIO/1 BLE_PROBE id=<n> result=<found|miss> ...`
- `WIO/1 BLE_WRITE id=<n> ok=<0|1>`
- `WIO/1 BLE_RX_BIN char=<name> len=<n>` followed by raw bytes
- `WIO/1 BLE_DROP reason=<reason>`

## Notes

SX1262/Sub-GHz verbs are intentionally not implemented yet. This mirrors the ESP32-side placeholder in `WioSx1262Backend`: the WIO relay is Bluetooth-only until the `SUBGHZ_*` UART protocol is designed.

## USB diagnostic console

The firmware also exposes a direct USB serial console at `115200` baud. This is independent of the ESP32-S3 UART link and is meant for bring-up when the relay flashes but appears silent.

On boot or when the USB monitor attaches, the console prints:

```text
Spectre WIO nRF relay console
version=...
type 'help' for commands; typed characters echo here
nrf>
```

Commands:

- `help` - show commands.
- `status` - print nRF uptime, BLE state, UART counters, and probe counters.
- `caps` - send the normal `WIO/1 CAPS...` line to the ESP32-S3 UART.
- `wio-status` - send the normal `WIO/1 STATUS...` line to the ESP32-S3 UART.
- `probe [ms]` - run a BLE phone-peripheral probe from the nRF.
- `drop` - disconnect the phone BLE link.
- `ble start` - initialize Bluefruit BLE stack manually if auto-start has been disabled for a debug build.
- `echo <text>` - loop typed text back over USB.
- `uart <line>` - send a raw line to the ESP32-S3 UART.
- `led auto|red|green|blue|white|off` - control the status LED; auto is red while waiting and blue when connected/services-ready.
- `reboot` - software reset the nRF.
