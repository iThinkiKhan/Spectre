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
- `SPECTRE/1 SUBGHZ_CONFIG freq=<hz> bw=<hz> sf=<n> cr=<5..8> preamble=<n> sync=<0xNN> power=<dbm>`
- `SPECTRE/1 SUBGHZ_MODE mode=<off|standby|rx>`
- `SPECTRE/1 SUBGHZ_TX id=<n> len=<n>` followed by raw bytes
- `SPECTRE/1 SUBGHZ_STATUS` (request a status line)

WIO -> ESP:

- `WIO/1 CAPS BLE_PROXY UART BLE_WRITE_BIN [SX1262_PRESENT] ver=<v>`
- `WIO/1 STATUS phone=<state> connected=<0|1> rssi=<dbm>`
- `WIO/1 BLE_PROBE id=<n> result=<found|miss> ...`
- `WIO/1 BLE_WRITE id=<n> ok=<0|1>`
- `WIO/1 BLE_RX_BIN char=<name> len=<n>` followed by raw bytes
- `WIO/1 BLE_DROP reason=<reason>`
- `WIO/1 SUBGHZ_TX id=<n> ok=<0|1>` (ack, sent on TxDone so the ESP can pace by airtime)
- `WIO/1 SUBGHZ_RX len=<n> rssi=<dbm> snr=<dB> crc=<crc32>` followed by raw bytes
- `WIO/1 SUBGHZ_STATUS present=<0|1> mode=<0|1|2> freq=<hz> bw=<hz> sf=<n> cr=<n> sync=0xNN power=<dbm> rx=<n> tx=<n> txfail=<n> rxdrop=<n> rssi=<n> snr=<n>`

## SX1262 modem bridge

The nRF drives the onboard SX1262 via RadioLib as a **thin modem**: it owns the
radio silicon and the DIO1 interrupt, but all protocol logic (Spectre-native
SubGhz and Meshtastic) lives on the ESP32-S3, which configures the PHY and
exchanges raw LoRa frames over the `SUBGHZ_*` verbs above. Payload bytes stay
opaque to the relay, exactly like the BLE path. RX frames carry the same CRC-32
(`uartFrameCrc32`) integrity guard as `BLE_RX_BIN`.

`mode` values: `0` off (sleep), `1` standby, `2` receive. The radio holds **one**
modulation config at a time, so the ESP selects either a Spectre-native LoRa
profile or the Meshtastic LongFast profile via `SUBGHZ_CONFIG` — they cannot run
simultaneously on the single SX1262.

Pin map is for the **XIAO nRF52840 + Wio-SX1262 kit** (SKU 102010710): CS=D4,
DIO1=D1, BUSY=D3, RESET=D2, RXEN=D5, DIO2-as-RF-switch, TCXO 1.8 V, SPI on
D8/D9/D10. The UART link to the ESP uses D6/D7, so BLE, UART, and LoRa coexist.
For the 30-pin board-to-board carrier build with `-DSPECTRE_WIO_SX1262_BTB=1`
(CS=D3, DIO1=D0, BUSY=D1, RESET=D2, RXEN=D4). Build with `-DSPECTRE_WIO_SX1262=0`
to drop the SX1262 entirely and revert to a Bluetooth-only relay.

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
- `led auto|red|blue|off|white` - control the status LED; auto is red while waiting and blue when connected/services-ready.
- `sx [status]` - print SX1262 modem state (present, mode, freq, counters, last RSSI/SNR).
- `sx rx` / `sx standby` - arm the SX1262 for receive or drop to standby (bring-up).
- `sx tx <text>` - transmit a raw LoRa test frame on the current PHY profile (bring-up).
- `reboot` - software reset the nRF.
