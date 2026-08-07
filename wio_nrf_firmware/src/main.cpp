#include <Arduino.h>
#include <bluefruit.h>
#include <nrf_gpio.h>

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#if SPECTRE_WIO_SX1262
#include <RadioLib.h>
#endif

#ifndef SPECTRE_WIO_RELAY_VERSION
#define SPECTRE_WIO_RELAY_VERSION "0.0.0"
#endif

#ifndef SPECTRE_NRF_AUTO_BLE
#define SPECTRE_NRF_AUTO_BLE 1
#endif

namespace {

constexpr uint32_t UART_BAUD = 115200;
constexpr size_t UART_LINE_MAX = 640;
constexpr size_t USB_LINE_MAX = 160;
constexpr size_t UART_PAYLOAD_MAX = 256;

// Bound nRF->S3 UART bytes while the ESP32 is busy with WiFi capture.
// Frames = complete wire units (one line,
// or one BLE_RX_BIN/SUBGHZ_RX header+payload+CRLF bundled together).
constexpr size_t UART_TX_QUEUE_DEPTH = 32;             // buffered frames
constexpr size_t UART_TX_FRAME_MAX = UART_LINE_MAX + 8;  // largest wire unit + CRLF
constexpr uint32_t FLOWCTRL_WINDOW = 4;               // max frames in flight
constexpr uint32_t FLOWCTRL_STALL_MS = 750;           // no ack progress -> resync baseline
constexpr uint32_t DEFAULT_PROBE_TIMEOUT_MS = 12000;
constexpr uint32_t STATUS_INTERVAL_MS = 5000;
constexpr uint32_t USB_BANNER_INTERVAL_MS = 1500;
// Idle LED blink + host-idle power-down. On battery, with the ESP host not
// driving the relay, holding the status LED solid (and the SX1262 in standby)
// drains the pack overnight. Instead we blink a short heartbeat while waiting,
// and once the UART link has been silent past HOST_IDLE_MS we treat the host as
// gone: LED fully off and the radio dropped to sleep.
constexpr uint32_t LED_BLINK_PERIOD_MS = 2000;
constexpr uint32_t LED_BLINK_ON_MS = 24;
constexpr uint32_t HOST_IDLE_MS = 30000;
constexpr uint32_t BLE_INIT_DELAY_MS = 3000;
constexpr uint32_t SERVICE_DISCOVERY_RETRY_MS = 250;
constexpr uint32_t SERVICE_DISCOVERY_TIMEOUT_MS = 3500;
// A first-attempt link-layer failure (HCI 0x3E CONN_FAILED_TO_BE_ESTABLISHED)
// or a drop while service discovery is still pending is transient, not a real
// "service missing" — re-scan and reconnect within the probe window instead of
// failing the whole probe on one hiccup.
constexpr uint8_t MAX_PROBE_CONNECT_RETRIES = 2;
constexpr uint32_t XIAO_LED_RED = NRF_GPIO_PIN_MAP(0, 26);
constexpr uint32_t XIAO_LED_GREEN = NRF_GPIO_PIN_MAP(0, 30);
constexpr uint32_t XIAO_LED_BLUE = NRF_GPIO_PIN_MAP(0, 6);

enum class LedColor : uint8_t {
    Off,
    Red,
    Green,
    Blue,
    White,
};

constexpr const char* PHONE_SERVICE_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0001";
constexpr const char* PHONE_GPS_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0002";
constexpr const char* PHONE_CONTROL_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0003";
constexpr const char* PHONE_METADATA_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0004";
constexpr const char* PHONE_EVENT_BATCH_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0005";
constexpr const char* PHONE_ENRICHMENT_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0006";
constexpr const char* PHONE_AUTH_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0007";
constexpr const char* PHONE_AUTH_REQUEST_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a000e";
constexpr const char* PHONE_STORAGE_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0008";
constexpr const char* PHONE_COMMAND_REQ_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a0009";
constexpr const char* PHONE_COMMAND_RESP_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a000a";
constexpr const char* PHONE_LOG_STREAM_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a000b";
constexpr const char* PHONE_DASHBOARD_STREAM_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a000c";
constexpr const char* PHONE_NOTIFICATION_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a000d";

constexpr const char* TEXT_SERVICE_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a1001";
constexpr const char* TEXT_PROMPT_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a1002";
constexpr const char* TEXT_INPUT_UUID = "84f03a80-6d7b-4d4d-9a64-6b2d6f3a1003";

BLEClientService phoneService(PHONE_SERVICE_UUID);
BLEClientCharacteristic gpsChr(PHONE_GPS_UUID);
BLEClientCharacteristic controlChr(PHONE_CONTROL_UUID);
BLEClientCharacteristic metadataChr(PHONE_METADATA_UUID);
BLEClientCharacteristic batchChr(PHONE_EVENT_BATCH_UUID);
BLEClientCharacteristic enrichChr(PHONE_ENRICHMENT_UUID);
BLEClientCharacteristic authChr(PHONE_AUTH_UUID);
BLEClientCharacteristic authRequestChr(PHONE_AUTH_REQUEST_UUID);
BLEClientCharacteristic storageChr(PHONE_STORAGE_UUID);
BLEClientCharacteristic commandReqChr(PHONE_COMMAND_REQ_UUID);
BLEClientCharacteristic commandRespChr(PHONE_COMMAND_RESP_UUID);
BLEClientCharacteristic logStreamChr(PHONE_LOG_STREAM_UUID);
BLEClientCharacteristic dashboardStreamChr(PHONE_DASHBOARD_STREAM_UUID);
BLEClientCharacteristic notificationChr(PHONE_NOTIFICATION_UUID);

BLEClientService textService(TEXT_SERVICE_UUID);
BLEClientCharacteristic textPromptChr(TEXT_PROMPT_UUID);
BLEClientCharacteristic textInputChr(TEXT_INPUT_UUID);

struct ProbeStats {
    uint32_t id = 0;
    uint32_t startedMs = 0;
    uint32_t timeoutMs = DEFAULT_PROBE_TIMEOUT_MS;
    uint32_t seen = 0;
    uint32_t uuidSeen = 0;
    uint32_t connectAttempts = 0;
    uint32_t connectFailures = 0;
    uint8_t connectRetries = 0;
    int bestRssi = -127;
    char err[24] = "none";
    char disc[16] = "0x00";
    char source[16] = "none";
    bool active = false;
};

ProbeStats probe;

uint16_t connHandle = BLE_CONN_HANDLE_INVALID;
bool connected = false;
bool servicesReady = false;
bool textReady = false;
bool suppressNextDrop = false;
bool serviceDiscoveryPending = false;
int lastRssi = 0;
uint32_t lastStatusMs = 0;
uint32_t lastUsbBannerMs = 0;
uint32_t lastUartRxMs = 0;
uint32_t serviceDiscoveryStartedMs = 0;
uint32_t serviceDiscoveryLastAttemptMs = 0;
uint32_t bootMs = 0;
uint32_t usbCommandCount = 0;
uint32_t uartLineCount = 0;
uint32_t uartTxLineCount = 0;
uint32_t bleRxCount = 0;
uint32_t bleWriteCount = 0;
uint32_t ledStatusTicks = 0;
uint32_t usbProbeId = 900000;
uint8_t serviceDiscoveryAttempts = 0;
LedColor ledColor = LedColor::Off;
bool statusLedAuto = true;
bool usbBannerPrinted = false;
bool bleInitStarted = false;
bool bleReady = false;
bool bleInitBlocked = false;

char lineBuf[UART_LINE_MAX] = {};
size_t lineLen = 0;
bool lineOverflow = false;

char usbLineBuf[USB_LINE_MAX] = {};
size_t usbLineLen = 0;
bool usbLineOverflow = false;

// Destination for an in-progress raw-binary UART frame. The byte-collection
// loop in pollUart() is shared between BLE characteristic writes and SX1262
// transmits; this tag tells it where the assembled bytes should go.
enum class PendingBinTarget : uint8_t {
    None = 0,
    Ble,
    Subghz,
};

bool pendingBin = false;
PendingBinTarget pendingBinTarget = PendingBinTarget::None;
char pendingBinChr[18] = {};
uint32_t pendingBinId = 0;
size_t pendingBinExpected = 0;
size_t pendingBinLen = 0;
uint32_t pendingBinCrc = 0;
bool pendingBinCrcPresent = false;
uint8_t pendingBinBuf[UART_PAYLOAD_MAX] = {};

// UART TX queue: protect ring indices, but write bytes outside the lock.
struct UartTxFrame {
    uint16_t len = 0;
    uint8_t data[UART_TX_FRAME_MAX] = {};
};
UartTxFrame uartTxQueue[UART_TX_QUEUE_DEPTH];
size_t uartTxHead = 0;          // index of the next frame to send
size_t uartTxCount = 0;         // frames currently queued
uint32_t uartTxFramesSent = 0;  // cumulative frames written to Serial1
uint32_t uartTxDropped = 0;     // frames dropped because the ring was full

// Flow control engages the first time the S3 acks it honors credits, so a
// half-flashed pair (old S3 that never sends RXCREDIT, or old nRF without the
// CAPS bit) safely falls back to today's fire-and-forget streaming.
bool flowCtrlActive = false;
uint32_t flowCtrlAckTotal = 0;  // latest cumulative frames the S3 reports draining
uint32_t flowCtrlSentBase = 0;  // uartTxFramesSent at engage / last resync
uint32_t flowCtrlAckBase = 0;   // flowCtrlAckTotal at engage / last resync
uint32_t flowCtrlLastAckMs = 0; // last time the ack advanced (stall watchdog)

inline void uartTxLock()   { taskENTER_CRITICAL(); }
inline void uartTxUnlock() { taskEXIT_CRITICAL(); }

void enqueueUartFrame(const uint8_t* bytes, size_t len) {
    if (!bytes || len == 0 || len > UART_TX_FRAME_MAX) {
        return;
    }
    uartTxLock();
    if (uartTxCount >= UART_TX_QUEUE_DEPTH) {
        ++uartTxDropped;
        uartTxUnlock();
        return;  // Should not happen once flow control is pacing us.
    }
    const size_t slot = (uartTxHead + uartTxCount) % UART_TX_QUEUE_DEPTH;
    memcpy(uartTxQueue[slot].data, bytes, len);
    uartTxQueue[slot].len = static_cast<uint16_t>(len);
    ++uartTxCount;
    uartTxUnlock();
}

// Frames sent minus frames acked, measured from the engage baseline so the two
// boards' independent boot-time counters never matter. Only touched from loop
// context (pump + RXCREDIT handler), so no lock needed for the cumulative vars.
uint32_t flowCtrlInFlight() {
    const uint32_t sent = uartTxFramesSent - flowCtrlSentBase;
    const uint32_t acked = flowCtrlAckTotal - flowCtrlAckBase;
    return (sent > acked) ? (sent - acked) : 0;
}

void pumpUartTx() {
    // Stall watchdog: if we are blocked on credits but the S3 has not advanced
    // the ack in a while, the counters desynced (e.g. a frame was lost before
    // flow control engaged). Resync the baseline so we can never wedge forever.
    if (flowCtrlActive && uartTxCount > 0 &&
        flowCtrlInFlight() >= FLOWCTRL_WINDOW &&
        millis() - flowCtrlLastAckMs > FLOWCTRL_STALL_MS) {
        flowCtrlSentBase = uartTxFramesSent;
        flowCtrlAckBase = flowCtrlAckTotal;
        flowCtrlLastAckMs = millis();
    }

    while (true) {
        if (flowCtrlActive && flowCtrlInFlight() >= FLOWCTRL_WINDOW) {
            break;
        }
        uartTxLock();
        if (uartTxCount == 0) {
            uartTxUnlock();
            break;
        }
        const size_t head = uartTxHead;
        const uint16_t len = uartTxQueue[head].len;
        uartTxUnlock();

        // Safe to read the head slot unlocked: it is not reused until we
        // decrement uartTxCount below.
        Serial1.write(uartTxQueue[head].data, len);

        uartTxLock();
        uartTxHead = (uartTxHead + 1) % UART_TX_QUEUE_DEPTH;
        --uartTxCount;
        uartTxUnlock();
        ++uartTxFramesSent;
    }
}

#if SPECTRE_WIO_SX1262
// SX1262 bridge: nRF owns radio IRQs; ESP32-S3 owns protocol logic.
#if defined(SPECTRE_WIO_SX1262_BTB)
#define SX_PIN_CS    D3
#define SX_PIN_DIO1  D0
#define SX_PIN_BUSY  D1
#define SX_PIN_RESET D2
#define SX_PIN_RXEN  D4
#else
#define SX_PIN_CS    D4
#define SX_PIN_DIO1  D1
#define SX_PIN_BUSY  D3
#define SX_PIN_RESET D2
#define SX_PIN_RXEN  D5
#endif

// Radio mode the ESP last asked us to hold. After a TX completes we return to
// this mode (so an RX session resumes automatically once airtime is free).
enum class LoraMode : uint8_t {
    Off = 0,
    Standby,
    Rx,
};

SX1262 loraRadio = new Module(SX_PIN_CS, SX_PIN_DIO1, SX_PIN_RESET, SX_PIN_BUSY);

bool loraPresent = false;
bool loraTxBusy = false;
LoraMode loraMode = LoraMode::Off;
uint32_t loraTxId = 0;

// Cached PHY config (defaults are a sane 915 MHz LoRa bring-up profile; the
// ESP overrides these via SUBGHZ_CONFIG for native SubGhz or Meshtastic).
uint32_t loraFreqHz   = 915000000UL;
uint32_t loraBwHz     = 125000UL;
uint8_t  loraSf       = 9;
uint8_t  loraCr       = 5;       // RadioLib coding rate denominator 4/CR (5..8)
uint8_t  loraSync     = 0x12;    // 0x12 private; Meshtastic uses 0x2B
uint16_t loraPreamble = 8;
int8_t   loraPower    = 17;      // dBm

uint32_t loraRxCount = 0;
uint32_t loraTxCount = 0;
uint32_t loraTxFail  = 0;
uint32_t loraRxDrop  = 0;
int      loraLastRssi = 0;
int      loraLastSnr  = 0;

volatile bool loraDio1Fired = false;

void onLoraDio1() {
    loraDio1Fired = true;
}
#endif  // SPECTRE_WIO_SX1262

void initBle();
void sendProbeResult(bool found);

void logUsb(const char* msg) {
    if (Serial) {
        Serial.println(msg);
    }
}

void usbPrintPrompt() {
    if (Serial) {
        Serial.print("nrf> ");
    }
}

void usbPrintBanner() {
    if (!Serial) {
        return;
    }
    Serial.println();
    Serial.println("Spectre WIO nRF relay console");
    Serial.print("version=");
    Serial.print(SPECTRE_WIO_RELAY_VERSION);
    Serial.print(" baud=");
    Serial.println(UART_BAUD);
    Serial.println("type 'help' for commands; typed characters echo here");
    usbPrintPrompt();
}

void printUsbHelp() {
    if (!Serial) {
        return;
    }
    Serial.println("Commands:");
    Serial.println("  help              show this help");
    Serial.println("  status            print nRF/BLE/UART counters");
    Serial.println("  caps              send relay CAPS to S3 UART");
    Serial.println("  wio-status        send relay STATUS to S3 UART");
    Serial.println("  probe [ms]        BLE probe for phone peripheral");
    Serial.println("  drop              disconnect phone BLE link");
    Serial.println("  ble start         initialize Bluefruit BLE stack");
    Serial.println("  echo <text>       loop typed text back over USB");
    Serial.println("  uart <line>       send raw line to S3 UART");
    Serial.println("  led auto|red|blue|off set status LED mode");
#if SPECTRE_WIO_SX1262
    Serial.println("  sx [status]       print SX1262 modem state");
    Serial.println("  sx rx|standby     arm SX1262 receive / standby");
    Serial.println("  sx tx <text>      transmit a raw LoRa test frame");
#endif
    Serial.println("  reboot            software reset the nRF");
}

const char* ledColorName(LedColor color) {
    switch (color) {
        case LedColor::Red:
            return "red";
        case LedColor::Green:
            return "green";
        case LedColor::Blue:
            return "blue";
        case LedColor::White:
            return "white";
        case LedColor::Off:
        default:
            return "off";
    }
}

void printUsbStatus() {
    if (!Serial) {
        return;
    }
    Serial.println("USB/1 STATUS");
    Serial.print("  version=");
    Serial.println(SPECTRE_WIO_RELAY_VERSION);
    Serial.print("  uptime_ms=");
    Serial.println(millis() - bootMs);
    Serial.print("  phone=");
    Serial.println(connected ? (servicesReady ? "ready" : "connected") : "idle");
    Serial.print("  connected=");
    Serial.println(connected ? 1 : 0);
    Serial.print("  services_ready=");
    Serial.println(servicesReady ? 1 : 0);
    Serial.print("  text_ready=");
    Serial.println(textReady ? 1 : 0);
    Serial.print("  conn_handle=");
    Serial.println(connHandle);
    Serial.print("  rssi=");
    Serial.println(lastRssi);
    Serial.print("  probe_active=");
    Serial.println(probe.active ? 1 : 0);
    Serial.print("  probe_seen=");
    Serial.println(probe.seen);
    Serial.print("  probe_uuid_seen=");
    Serial.println(probe.uuidSeen);
    Serial.print("  probe_err=");
    Serial.println(probe.err);
    Serial.print("  uart_rx_lines=");
    Serial.println(uartLineCount);
    Serial.print("  uart_tx_lines=");
    Serial.println(uartTxLineCount);
    Serial.print("  ble_rx=");
    Serial.println(bleRxCount);
    Serial.print("  ble_write=");
    Serial.println(bleWriteCount);
    Serial.print("  usb_commands=");
    Serial.println(usbCommandCount);
    Serial.print("  led_mode=");
    Serial.println(statusLedAuto ? "auto" : "manual");
    Serial.print("  led_color=");
    Serial.println(ledColorName(ledColor));
    Serial.print("  ble_init_started=");
    Serial.println(bleInitStarted ? 1 : 0);
    Serial.print("  ble_ready=");
    Serial.println(bleReady ? 1 : 0);
    Serial.print("  ble_init_blocked=");
    Serial.println(bleInitBlocked ? 1 : 0);
}

void setLedColor(LedColor color) {
    if (ledColor == color) {
        return;
    }
    ledColor = color;
    nrf_gpio_pin_write(XIAO_LED_RED, (color == LedColor::Red || color == LedColor::White) ? 0UL : 1UL);
    nrf_gpio_pin_write(XIAO_LED_GREEN, (color == LedColor::Green || color == LedColor::White) ? 0UL : 1UL);
    nrf_gpio_pin_write(XIAO_LED_BLUE, (color == LedColor::Blue || color == LedColor::White) ? 0UL : 1UL);
}

void initDebugLed() {
    nrf_gpio_cfg_output(XIAO_LED_RED);
    nrf_gpio_cfg_output(XIAO_LED_GREEN);
    nrf_gpio_cfg_output(XIAO_LED_BLUE);
    setLedColor(LedColor::Red);
}

void pollStatusLed() {
    if (!statusLedAuto) {
        return;
    }
    const uint32_t now = millis();

    // Active use (phone connected + services discovered) is the only state we
    // hold the LED solid for. Everything else blinks a short heartbeat, and once
    // the host has gone quiet on the UART link we go fully dark — so an idle
    // accessory on battery isn't burning an LED over GPIO all night.
    if (connected && servicesReady) {
        setLedColor(LedColor::Blue);
        return;
    }
    if (now - lastUartRxMs >= HOST_IDLE_MS) {
        setLedColor(LedColor::Off);
        return;
    }
    ++ledStatusTicks;
    const uint32_t phase = (now - bootMs) % LED_BLINK_PERIOD_MS;
    setLedColor(phase < LED_BLINK_ON_MS ? LedColor::Red : LedColor::Off);
}

bool startsWith(const char* text, const char* prefix) {
    return text && prefix && strncmp(text, prefix, strlen(prefix)) == 0;
}

void sendLine(const char* line) {
    // Assemble the whole wire unit (line + CRLF) as one queued frame so the S3
    // counts it as exactly one credit, matching its per-line drain.
    uint8_t frame[UART_TX_FRAME_MAX];
    size_t n = 0;
    for (const char* p = line; *p && n < UART_TX_FRAME_MAX - 2; ++p) {
        frame[n++] = static_cast<uint8_t>(*p);
    }
    frame[n++] = '\r';
    frame[n++] = '\n';
    enqueueUartFrame(frame, n);
    ++uartTxLineCount;
    if (Serial) {
        Serial.print("[tx] ");
        Serial.println(line);
    }
}

void sendCaps() {
    char line[112] = {};
#if SPECTRE_WIO_SX1262
    const char* sx = loraPresent ? " SX1262_PRESENT" : "";
#else
    const char* sx = "";
#endif
    snprintf(line, sizeof(line),
             "WIO/1 CAPS BLE_PROXY UART BLE_WRITE_BIN UART_FLOWCTRL%s ver=%s",
             sx,
             SPECTRE_WIO_RELAY_VERSION);
    sendLine(line);
}

void sendStatus() {
    char line[128] = {};
    snprintf(line, sizeof(line),
             "WIO/1 STATUS phone=%s connected=%u rssi=%d ble_ready=%u ble_init_started=%u",
             connected ? (servicesReady ? "ready" : "connected") : "idle",
             connected ? 1u : 0u,
             lastRssi,
             bleReady ? 1u : 0u,
             bleInitStarted ? 1u : 0u);
    sendLine(line);
}

bool readKeyValue(const char* text, const char* key, char* out, size_t outLen) {
    if (!text || !key || !out || outLen == 0) {
        return false;
    }
    const size_t keyLen = strlen(key);
    const char* p = text;
    while (*p) {
        while (*p == ' ') {
            ++p;
        }
        const char* start = p;
        while (*p && *p != ' ') {
            ++p;
        }
        const char* eq = static_cast<const char*>(memchr(start, '=', p - start));
        if (eq && static_cast<size_t>(eq - start) == keyLen &&
            strncmp(start, key, keyLen) == 0) {
            const size_t valueLen = static_cast<size_t>(p - eq - 1);
            const size_t copyLen = min(valueLen, outLen - 1);
            memcpy(out, eq + 1, copyLen);
            out[copyLen] = '\0';
            return true;
        }
    }
    return false;
}

bool readUintValue(const char* text, const char* key, uint32_t& out) {
    char value[18] = {};
    if (!readKeyValue(text, key, value, sizeof(value))) {
        return false;
    }
    out = strtoul(value, nullptr, 10);
    return true;
}

uint8_t hexNibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    return 0xFF;
}

bool decodeHex(const char* hex, uint8_t* out, size_t outCap, size_t& outLen) {
    outLen = 0;
    if (!hex || !out) {
        return false;
    }
    const size_t hexLen = strlen(hex);
    if ((hexLen & 1U) != 0 || hexLen / 2 > outCap) {
        return false;
    }
    for (size_t i = 0; i < hexLen; i += 2) {
        const uint8_t hi = hexNibble(hex[i]);
        const uint8_t lo = hexNibble(hex[i + 1]);
        if (hi > 0x0F || lo > 0x0F) {
            return false;
        }
        out[outLen++] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

BLEClientCharacteristic* writableForName(const char* name) {
    if (!name) return nullptr;
    if (strcmp(name, "auth") == 0) return &authRequestChr;
    if (strcmp(name, "batch") == 0) return &batchChr;
    if (strcmp(name, "storage") == 0) return &storageChr;
    if (strcmp(name, "command_resp") == 0) return &commandRespChr;
    if (strcmp(name, "log_stream") == 0) return &logStreamChr;
    if (strcmp(name, "dashboard_stream") == 0) return &dashboardStreamChr;
    if (strcmp(name, "notification") == 0) return &notificationChr;
    if (strcmp(name, "text_prompt") == 0) return textReady ? &textPromptChr : nullptr;
    return nullptr;
}

bool writeBleValue(const char* name, const uint8_t* data, size_t len) {
    if (!bleReady || !connected || !servicesReady || len > UART_PAYLOAD_MAX) {
        return false;
    }
    BLEClientCharacteristic* chr = writableForName(name);
    if (!chr) {
        return false;
    }
    const bool ok =
        len == 0 ? chr->write(nullptr, 0) : chr->write(data, static_cast<uint16_t>(len));
    if (ok) {
        ++bleWriteCount;
    }
    return ok;
}

void sendWriteAck(uint32_t id, bool ok) {
    char line[64] = {};
    snprintf(line, sizeof(line),
             "WIO/1 BLE_WRITE id=%lu ok=%u",
             static_cast<unsigned long>(id),
             ok ? 1u : 0u);
    sendLine(line);
}

// Must match ESP32 WioNrfAccessory::uartFrameCrc32 byte-for-byte.
uint32_t uartFrameCrc32(const uint8_t* data, uint16_t len) {
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint16_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320UL & mask);
        }
    }
    return ~crc;
}

void sendBleRxBin(const char* name, const uint8_t* data, uint16_t len) {
    if (!name || (!data && len != 0) || len > UART_PAYLOAD_MAX) {
        return;
    }
    // Header + payload + CRLF are one flow-control credit.
    const uint32_t crc = uartFrameCrc32(data, len);
    uint8_t frame[UART_TX_FRAME_MAX];
    const int hdr = snprintf(reinterpret_cast<char*>(frame), UART_TX_FRAME_MAX,
                             "WIO/1 BLE_RX_BIN char=%s len=%u crc=%lu\r\n",
                             name,
                             static_cast<unsigned>(len),
                             static_cast<unsigned long>(crc));
    if (hdr < 0 || static_cast<size_t>(hdr) + len + 2 > UART_TX_FRAME_MAX) {
        return;
    }
    size_t total = static_cast<size_t>(hdr);
    if (len > 0) {
        memcpy(frame + total, data, len);
        total += len;
    }
    frame[total++] = '\r';
    frame[total++] = '\n';
    enqueueUartFrame(frame, total);
    ++bleRxCount;
}

#if SPECTRE_WIO_SX1262
void sendSubghzTxAck(uint32_t id, bool ok) {
    char line[64] = {};
    snprintf(line, sizeof(line),
             "WIO/1 SUBGHZ_TX id=%lu ok=%u",
             static_cast<unsigned long>(id),
             ok ? 1u : 0u);
    sendLine(line);
}

// Mirror of sendBleRxBin for sub-GHz frames.
void sendSubghzRx(const uint8_t* data, uint16_t len, int rssi, int snr) {
    if (len > UART_PAYLOAD_MAX) {
        return;
    }
    const uint32_t crc = uartFrameCrc32(data, len);
    uint8_t frame[UART_TX_FRAME_MAX];
    const int hdr = snprintf(reinterpret_cast<char*>(frame), UART_TX_FRAME_MAX,
                             "WIO/1 SUBGHZ_RX len=%u rssi=%d snr=%d crc=%lu\r\n",
                             static_cast<unsigned>(len),
                             rssi,
                             snr,
                             static_cast<unsigned long>(crc));
    if (hdr < 0 || static_cast<size_t>(hdr) + len + 2 > UART_TX_FRAME_MAX) {
        return;
    }
    size_t total = static_cast<size_t>(hdr);
    if (len > 0) {
        memcpy(frame + total, data, len);
        total += len;
    }
    frame[total++] = '\r';
    frame[total++] = '\n';
    enqueueUartFrame(frame, total);
}

void sendSubghzStatus() {
    char line[176] = {};
    snprintf(line, sizeof(line),
             "WIO/1 SUBGHZ_STATUS present=%u mode=%u freq=%lu bw=%lu sf=%u cr=%u "
             "sync=0x%02X power=%d rx=%lu tx=%lu txfail=%lu rxdrop=%lu rssi=%d snr=%d",
             loraPresent ? 1u : 0u,
             static_cast<unsigned>(loraMode),
             static_cast<unsigned long>(loraFreqHz),
             static_cast<unsigned long>(loraBwHz),
             static_cast<unsigned>(loraSf),
             static_cast<unsigned>(loraCr),
             static_cast<unsigned>(loraSync),
             static_cast<int>(loraPower),
             static_cast<unsigned long>(loraRxCount),
             static_cast<unsigned long>(loraTxCount),
             static_cast<unsigned long>(loraTxFail),
             static_cast<unsigned long>(loraRxDrop),
             loraLastRssi,
             loraLastSnr);
    sendLine(line);
}

// Push the cached PHY profile into the radio. Drops to standby first; resumes
// RX afterward if that was the held mode.
bool loraApplyConfig() {
    if (!loraPresent) {
        return false;
    }
    const LoraMode prev = loraMode;
    loraRadio.standby();
    loraTxBusy = false;
    bool ok = true;
    ok &= (loraRadio.setFrequency(static_cast<float>(loraFreqHz) / 1000000.0f) == RADIOLIB_ERR_NONE);
    ok &= (loraRadio.setBandwidth(static_cast<float>(loraBwHz) / 1000.0f) == RADIOLIB_ERR_NONE);
    ok &= (loraRadio.setSpreadingFactor(loraSf) == RADIOLIB_ERR_NONE);
    ok &= (loraRadio.setCodingRate(loraCr) == RADIOLIB_ERR_NONE);
    ok &= (loraRadio.setSyncWord(loraSync) == RADIOLIB_ERR_NONE);
    ok &= (loraRadio.setPreambleLength(loraPreamble) == RADIOLIB_ERR_NONE);
    ok &= (loraRadio.setOutputPower(loraPower) == RADIOLIB_ERR_NONE);
    if (prev == LoraMode::Rx) {
        loraRadio.startReceive();
        loraMode = LoraMode::Rx;
    } else {
        loraMode = LoraMode::Standby;
    }
    return ok;
}

bool initLora() {
    const float freqMhz = static_cast<float>(loraFreqHz) / 1000000.0f;
    const float bwKhz = static_cast<float>(loraBwHz) / 1000.0f;
    const int16_t st = loraRadio.begin(freqMhz, bwKhz, loraSf, loraCr, loraSync,
                                       loraPower, loraPreamble, 1.8f, false);
    if (st != RADIOLIB_ERR_NONE) {
        loraPresent = false;
        if (Serial) {
            Serial.print("USB/1 SX1262 init failed st=");
            Serial.println(st);
        }
        return false;
    }
    loraRadio.setDio2AsRfSwitch(true);
#ifdef SX_PIN_RXEN
    // DIO2 auto-drives the TX side of the RF switch; RXEN enables the RX/LNA
    // side. If TX works but RX hears nothing, this pairing is the first suspect
    // (some carriers wire the antenna switch differently).
    loraRadio.setRfSwitchPins(SX_PIN_RXEN, RADIOLIB_NC);
#endif
    loraRadio.setDio1Action(onLoraDio1);
    // Idle in warm sleep (config retained) until the ESP configures a PHY and
    // asks for rx/standby. Standby would burn ~1 mA the whole time the host is
    // off; sleep is a few µA. SUBGHZ_CONFIG/MODE wakes it via standby().
    loraRadio.sleep();
    loraMode = LoraMode::Off;
    loraPresent = true;
    if (Serial) {
        Serial.println("USB/1 SX1262 ready");
    }
    return true;
}

void loraSetMode(LoraMode m) {
    if (!loraPresent) {
        return;
    }
    loraTxBusy = false;
    switch (m) {
        case LoraMode::Rx:
            loraRadio.startReceive();
            break;
        case LoraMode::Standby:
            loraRadio.standby();
            break;
        case LoraMode::Off:
            loraRadio.sleep();  // warm sleep, config retained
            break;
    }
    loraMode = m;
}

void loraStartTx(const uint8_t* data, size_t len, uint32_t id) {
    if (!loraPresent || len == 0 || len > UART_PAYLOAD_MAX) {
        loraTxFail++;
        sendSubghzTxAck(id, false);
        return;
    }
    const int16_t st = loraRadio.startTransmit(const_cast<uint8_t*>(data),
                                               static_cast<size_t>(len));
    if (st != RADIOLIB_ERR_NONE) {
        loraTxFail++;
        sendSubghzTxAck(id, false);
        if (loraMode == LoraMode::Rx) {
            loraRadio.startReceive();
        }
        return;
    }
    // Ack deferred to TxDone (DIO1) so the ESP can pace by real airtime.
    loraTxBusy = true;
    loraTxId = id;
}

void handleSubghzConfig(const char* args) {
    uint32_t v = 0;
    if (readUintValue(args, "freq", v)) loraFreqHz = v;
    if (readUintValue(args, "bw", v)) loraBwHz = v;
    if (readUintValue(args, "sf", v)) loraSf = static_cast<uint8_t>(v);
    if (readUintValue(args, "cr", v)) loraCr = static_cast<uint8_t>(v);
    if (readUintValue(args, "preamble", v)) loraPreamble = static_cast<uint16_t>(v);
    if (readUintValue(args, "power", v)) loraPower = static_cast<int8_t>(v);
    char sync[8] = {};
    if (readKeyValue(args, "sync", sync, sizeof(sync))) {
        loraSync = static_cast<uint8_t>(strtoul(sync, nullptr, 0));
    }
    loraApplyConfig();
    sendSubghzStatus();
}

void handleSubghzMode(const char* args) {
    char mode[12] = {};
    if (!readKeyValue(args, "mode", mode, sizeof(mode))) {
        sendSubghzStatus();
        return;
    }
    if (strcmp(mode, "rx") == 0) {
        loraSetMode(LoraMode::Rx);
    } else if (strcmp(mode, "standby") == 0) {
        loraSetMode(LoraMode::Standby);
    } else if (strcmp(mode, "off") == 0) {
        loraSetMode(LoraMode::Off);
    }
    sendSubghzStatus();
}

void beginSubghzTx(const char* args) {
    uint32_t id = 0;
    uint32_t len = 0;
    const bool parsed = readUintValue(args, "id", id) &&
                        readUintValue(args, "len", len);
    if (!parsed || len == 0 || len > UART_PAYLOAD_MAX) {
        sendSubghzTxAck(id, false);
        return;
    }
    pendingBin = true;
    pendingBinTarget = PendingBinTarget::Subghz;
    pendingBinId = id;
    pendingBinExpected = len;
    pendingBinLen = 0;
    pendingBinChr[0] = '\0';
}

// Serviced from loop(). Handles the single shared DIO1 IRQ: TxDone (ack +
// resume RX) or RxDone (forward the frame + restart RX).
void pollSubGhz() {
    if (!loraPresent || !loraDio1Fired) {
        return;
    }
    loraDio1Fired = false;

    if (loraTxBusy) {
        loraTxBusy = false;
        loraRadio.finishTransmit();
        loraTxCount++;
        sendSubghzTxAck(loraTxId, true);
        if (loraMode == LoraMode::Rx) {
            loraRadio.startReceive();
        }
        return;
    }

    const size_t len = loraRadio.getPacketLength();
    if (len == 0 || len > UART_PAYLOAD_MAX) {
        loraRxDrop++;
        if (loraMode == LoraMode::Rx) {
            loraRadio.startReceive();
        }
        return;
    }
    uint8_t buf[UART_PAYLOAD_MAX] = {};
    const int16_t st = loraRadio.readData(buf, len);
    if (st != RADIOLIB_ERR_NONE) {
        loraRxDrop++;
        if (loraMode == LoraMode::Rx) {
            loraRadio.startReceive();
        }
        return;
    }
    loraLastRssi = static_cast<int>(loraRadio.getRSSI());
    loraLastSnr = static_cast<int>(loraRadio.getSNR());
    loraRxCount++;
    sendSubghzRx(buf, static_cast<uint16_t>(len), loraLastRssi, loraLastSnr);
    if (loraMode == LoraMode::Rx) {
        loraRadio.startReceive();
    }
}
#endif  // SPECTRE_WIO_SX1262

const char* notifyNameFor(BLEClientCharacteristic* chr) {
    if (chr == &authChr) return "auth";
    if (chr == &controlChr) return "control";
    if (chr == &gpsChr) return "gps";
    if (chr == &enrichChr) return "enrich";
    if (chr == &commandReqChr) return "command_req";
    if (chr == &textInputChr) return "text_input";
    return nullptr;
}

void notifyCallback(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len) {
    const char* name = notifyNameFor(chr);
    if (!name) {
        return;
    }
    if (len > UART_PAYLOAD_MAX) {
        char line[96] = {};
        snprintf(line, sizeof(line),
                 "WIO/1 BLE_RX_DROP char=%s len=%u reason=too_big",
                 name,
                 static_cast<unsigned>(len));
        sendLine(line);
        return;
    }
    sendBleRxBin(name, data, len);
}

bool discoverPhoneService(uint16_t handle) {
    textReady = false;
    servicesReady = false;

    if (!phoneService.discover(handle)) {
        strlcpy(probe.err, "service_missing", sizeof(probe.err));
        return false;
    }

    bool ok = true;
    ok = gpsChr.discover() && ok;
    ok = controlChr.discover() && ok;
    metadataChr.discover();
    ok = batchChr.discover() && ok;
    ok = enrichChr.discover() && ok;
    ok = authChr.discover() && ok;
    ok = authRequestChr.discover() && ok;
    ok = storageChr.discover() && ok;
    ok = commandReqChr.discover() && ok;
    ok = commandRespChr.discover() && ok;
    ok = logStreamChr.discover() && ok;
    ok = dashboardStreamChr.discover() && ok;
    ok = notificationChr.discover() && ok;

    if (!ok) {
        strlcpy(probe.err, "char_missing", sizeof(probe.err));
        return false;
    }

    authChr.setNotifyCallback(notifyCallback);
    controlChr.setNotifyCallback(notifyCallback);
    gpsChr.setNotifyCallback(notifyCallback);
    enrichChr.setNotifyCallback(notifyCallback);
    commandReqChr.setNotifyCallback(notifyCallback);

    authChr.enableNotify();
    controlChr.enableNotify();
    gpsChr.enableNotify();
    enrichChr.enableNotify();
    commandReqChr.enableNotify();

    if (textService.discover(handle)) {
        const bool promptOk = textPromptChr.discover();
        const bool inputOk = textInputChr.discover();
        if (promptOk && inputOk) {
            textInputChr.setNotifyCallback(notifyCallback);
            textInputChr.enableNotify();
            textReady = true;
        }
    }

    servicesReady = true;
    return true;
}

void finishServiceDiscovery(bool found) {
    serviceDiscoveryPending = false;
    if (found) {
        strlcpy(probe.err, "none", sizeof(probe.err));
        if (probe.active) {
            sendProbeResult(true);
        } else {
            sendStatus();
        }
        return;
    }

    ++probe.connectFailures;
    suppressNextDrop = true;
    Bluefruit.disconnect(connHandle);
    connected = false;
    servicesReady = false;
    textReady = false;
    connHandle = BLE_CONN_HANDLE_INVALID;
    if (probe.active) {
        sendProbeResult(false);
    }
}

void beginServiceDiscovery(uint16_t handle) {
    serviceDiscoveryPending = true;
    serviceDiscoveryStartedMs = millis();
    serviceDiscoveryLastAttemptMs = serviceDiscoveryStartedMs;
    serviceDiscoveryAttempts = 1;

    if (discoverPhoneService(handle)) {
        finishServiceDiscovery(true);
    }
}

void pollServiceDiscovery() {
    if (!serviceDiscoveryPending ||
        connHandle == BLE_CONN_HANDLE_INVALID ||
        servicesReady) {
        return;
    }

    const uint32_t now = millis();
    if (now - serviceDiscoveryLastAttemptMs < SERVICE_DISCOVERY_RETRY_MS) {
        return;
    }

    serviceDiscoveryLastAttemptMs = now;
    ++serviceDiscoveryAttempts;

    if (discoverPhoneService(connHandle)) {
        finishServiceDiscovery(true);
        return;
    }

    if (now - serviceDiscoveryStartedMs >= SERVICE_DISCOVERY_TIMEOUT_MS) {
        finishServiceDiscovery(false);
    }
}

void sendProbeResult(bool found) {
    char line[192] = {};
    snprintf(line, sizeof(line),
             "WIO/1 BLE_PROBE id=%lu result=%s connected=%u rssi=%d seen=%lu uuid=%lu conn=%lu fail=%lu text=%u best=%d disc=%s err=%s source=%s",
             static_cast<unsigned long>(probe.id),
             found ? "found" : "miss",
             connected && servicesReady ? 1u : 0u,
             lastRssi,
             static_cast<unsigned long>(probe.seen),
             static_cast<unsigned long>(probe.uuidSeen),
             static_cast<unsigned long>(probe.connectAttempts),
             static_cast<unsigned long>(probe.connectFailures),
             textReady ? 1u : 0u,
             probe.bestRssi,
             probe.disc,
             probe.err,
             probe.source);
    sendLine(line);
    probe.active = false;
}

void disconnectPhone(const char* reason) {
    if (connHandle != BLE_CONN_HANDLE_INVALID) {
        suppressNextDrop = true;
        Bluefruit.disconnect(connHandle);
    }
    connected = false;
    servicesReady = false;
    textReady = false;
    serviceDiscoveryPending = false;
    connHandle = BLE_CONN_HANDLE_INVALID;
    char line[64] = {};
    snprintf(line, sizeof(line), "WIO/1 BLE_DROP reason=%s", reason ? reason : "local");
    sendLine(line);
}

void connectCallback(uint16_t handle) {
    connHandle = handle;
    connected = true;
    lastRssi = probe.bestRssi == -127 ? 0 : probe.bestRssi;
    strlcpy(probe.source, "scan", sizeof(probe.source));

    Bluefruit.Connection(handle)->requestMtuExchange(247);
    beginServiceDiscovery(handle);
}

void disconnectCallback(uint16_t handle, uint8_t reason) {
    (void)handle;
    snprintf(probe.disc, sizeof(probe.disc), "0x%02X", reason);
    const bool wasServiceDiscoveryPending = serviceDiscoveryPending;
    connected = false;
    servicesReady = false;
    textReady = false;
    serviceDiscoveryPending = false;
    connHandle = BLE_CONN_HANDLE_INVALID;
    if (suppressNextDrop) {
        suppressNextDrop = false;
        return;
    }
    if (probe.active) {
        ++probe.connectFailures;
        // 0x3E (CONN_FAILED_TO_BE_ESTABLISHED) and any drop while discovery was
        // still pending are transient link-setup failures — the phone's GATT
        // server is fine. Re-scan and retry within the probe window rather than
        // reporting a (mislabeled) miss on the first hiccup.
        const bool transientSetup = (reason == 0x3E) || wasServiceDiscoveryPending;
        if (transientSetup &&
            probe.connectRetries < MAX_PROBE_CONNECT_RETRIES &&
            millis() - probe.startedMs < probe.timeoutMs) {
            ++probe.connectRetries;
            snprintf(probe.err, sizeof(probe.err), "retry_0x%02X", reason);
            Bluefruit.Scanner.start(0);
            return;
        }
        // Final failure: report the actual HCI reason instead of guessing
        // "service_missing" (a genuine missing service/char is handled earlier
        // in finishServiceDiscovery, which suppresses this drop).
        snprintf(probe.err, sizeof(probe.err), "conn_failed_0x%02X", reason);
        sendProbeResult(false);
        return;
    }
    char line[64] = {};
    snprintf(line, sizeof(line), "WIO/1 BLE_DROP reason=0x%02X", reason);
    sendLine(line);
}

void scanCallback(ble_gap_evt_adv_report_t* report) {
    ++probe.seen;
    if (report->rssi > probe.bestRssi) {
        probe.bestRssi = report->rssi;
    }

    if (Bluefruit.Scanner.checkReportForService(report, phoneService)) {
        ++probe.uuidSeen;
        lastRssi = report->rssi;
        ++probe.connectAttempts;
        Bluefruit.Scanner.stop();
        if (!Bluefruit.Central.connect(report)) {
            ++probe.connectFailures;
            strlcpy(probe.err, "connect_start_failed", sizeof(probe.err));
            Bluefruit.Scanner.start(0);
        }
        return;
    }

    Bluefruit.Scanner.resume();
}

void startProbe(uint32_t id, uint32_t timeoutMs) {
    if (!bleReady && !bleInitStarted) {
        initBle();
    }
    if (!bleReady) {
        probe = ProbeStats{};
        probe.id = id;
        probe.active = true;
        strlcpy(probe.err, "ble_not_ready", sizeof(probe.err));
        sendProbeResult(false);
        return;
    }

    if (connected && servicesReady) {
        probe.id = id;
        probe.active = true;
        strlcpy(probe.err, "none", sizeof(probe.err));
        sendProbeResult(true);
        return;
    }

    probe = ProbeStats{};
    probe.id = id;
    probe.timeoutMs = timeoutMs == 0 ? DEFAULT_PROBE_TIMEOUT_MS : timeoutMs;
    probe.startedMs = millis();
    probe.active = true;

    Bluefruit.Scanner.stop();
    Bluefruit.Scanner.clearFilters();
    Bluefruit.Scanner.setRxCallback(scanCallback);
    Bluefruit.Scanner.restartOnDisconnect(false);
    Bluefruit.Scanner.setInterval(80, 40);
    Bluefruit.Scanner.useActiveScan(true);
    Bluefruit.Scanner.start(0);
}

void handleUsbCommand(const char* rawLine) {
    if (!rawLine) {
        return;
    }
    while (*rawLine == ' ') {
        ++rawLine;
    }
    if (*rawLine == '\0') {
        return;
    }

    ++usbCommandCount;
    if (strcmp(rawLine, "help") == 0 || strcmp(rawLine, "?") == 0) {
        printUsbHelp();
        return;
    }
    if (strcmp(rawLine, "status") == 0) {
        printUsbStatus();
        return;
    }
    if (strcmp(rawLine, "caps") == 0) {
        sendCaps();
        Serial.println("USB/1 OK caps sent to S3 UART");
        return;
    }
    if (strcmp(rawLine, "wio-status") == 0) {
        sendStatus();
        Serial.println("USB/1 OK status sent to S3 UART");
        return;
    }
    if (strcmp(rawLine, "drop") == 0) {
        disconnectPhone("usb_console");
        Serial.println("USB/1 OK drop requested");
        return;
    }
    if (strcmp(rawLine, "ble start") == 0) {
        initBle();
        Serial.print("USB/1 BLE_READY ");
        Serial.println(bleReady ? 1 : 0);
        return;
    }
    if (startsWith(rawLine, "probe")) {
        uint32_t timeoutMs = DEFAULT_PROBE_TIMEOUT_MS;
        const char* arg = rawLine + strlen("probe");
        while (*arg == ' ') {
            ++arg;
        }
        if (*arg) {
            timeoutMs = strtoul(arg, nullptr, 10);
        }
        startProbe(++usbProbeId, timeoutMs);
        Serial.print("USB/1 OK probe id=");
        Serial.print(usbProbeId);
        Serial.print(" timeout=");
        Serial.println(timeoutMs);
        return;
    }
    if (startsWith(rawLine, "echo ")) {
        Serial.print("USB/1 ECHO ");
        Serial.println(rawLine + strlen("echo "));
        return;
    }
    if (startsWith(rawLine, "uart ")) {
        sendLine(rawLine + strlen("uart "));
        Serial.println("USB/1 OK uart line sent");
        return;
    }
    if (startsWith(rawLine, "led ")) {
        const char* arg = rawLine + strlen("led ");
        if (strcmp(arg, "auto") == 0) {
            statusLedAuto = true;
            pollStatusLed();
            Serial.println("USB/1 OK led auto");
            return;
        }
        if (strcmp(arg, "red") == 0) {
            statusLedAuto = false;
            setLedColor(LedColor::Red);
            Serial.println("USB/1 OK led red");
            return;
        }
        if (strcmp(arg, "blue") == 0) {
            statusLedAuto = false;
            setLedColor(LedColor::Blue);
            Serial.println("USB/1 OK led blue");
            return;
        }
        if (strcmp(arg, "off") == 0) {
            statusLedAuto = false;
            setLedColor(LedColor::Off);
            Serial.println("USB/1 OK led off");
            return;
        }
        if (strcmp(arg, "white") == 0) {
            statusLedAuto = false;
            setLedColor(LedColor::White);
            Serial.println("USB/1 OK led white");
            return;
        }
    }
#if SPECTRE_WIO_SX1262
    if (startsWith(rawLine, "sx")) {
        const char* arg = rawLine + 2;
        while (*arg == ' ') {
            ++arg;
        }
        if (*arg == '\0' || strcmp(arg, "status") == 0) {
            Serial.print("USB/1 SX present=");
            Serial.print(loraPresent ? 1 : 0);
            Serial.print(" mode=");
            Serial.print(static_cast<unsigned>(loraMode));
            Serial.print(" freq=");
            Serial.print(loraFreqHz);
            Serial.print(" bw=");
            Serial.print(loraBwHz);
            Serial.print(" sf=");
            Serial.print(loraSf);
            Serial.print(" rx=");
            Serial.print(loraRxCount);
            Serial.print(" tx=");
            Serial.print(loraTxCount);
            Serial.print(" rssi=");
            Serial.print(loraLastRssi);
            Serial.print(" snr=");
            Serial.println(loraLastSnr);
            return;
        }
        if (!loraPresent) {
            Serial.println("USB/1 SX ERR not present");
            return;
        }
        if (strcmp(arg, "rx") == 0) {
            loraSetMode(LoraMode::Rx);
            Serial.println("USB/1 SX rx armed");
            return;
        }
        if (strcmp(arg, "standby") == 0) {
            loraSetMode(LoraMode::Standby);
            Serial.println("USB/1 SX standby");
            return;
        }
        if (startsWith(arg, "tx ")) {
            const char* msg = arg + strlen("tx ");
            const int16_t st = loraRadio.transmit(msg);
            // Blocking transmit() handled its own DIO1; clear our bridge flags
            // so pollSubGhz() doesn't double-service the stale interrupt.
            loraDio1Fired = false;
            loraTxBusy = false;
            if (st == RADIOLIB_ERR_NONE) {
                loraTxCount++;
                Serial.println("USB/1 SX tx ok");
            } else {
                loraTxFail++;
                Serial.print("USB/1 SX tx err st=");
                Serial.println(st);
            }
            if (loraMode == LoraMode::Rx) {
                loraRadio.startReceive();
            }
            return;
        }
        Serial.println("USB/1 SX usage: sx [status] | sx rx | sx standby | sx tx <text>");
        return;
    }
#endif
    if (strcmp(rawLine, "reboot") == 0) {
        Serial.println("USB/1 OK rebooting");
        Serial.flush();
        delay(50);
        NVIC_SystemReset();
        return;
    }

    Serial.print("USB/1 ERR unknown command: ");
    Serial.println(rawLine);
    Serial.println("USB/1 HINT type 'help'");
}

void pollUsbConsole() {
    if (!Serial) {
        usbBannerPrinted = false;
        return;
    }

    const uint32_t now = millis();
    if (!usbBannerPrinted && now - lastUsbBannerMs >= USB_BANNER_INTERVAL_MS) {
        lastUsbBannerMs = now;
        usbBannerPrinted = true;
        usbPrintBanner();
    }

    while (Serial.available()) {
        const int raw = Serial.read();
        if (raw < 0) {
            return;
        }
        const char c = static_cast<char>(raw);
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            Serial.println();
            if (usbLineOverflow) {
                Serial.println("USB/1 ERR line too long");
                usbLineLen = 0;
                usbLineOverflow = false;
                usbPrintPrompt();
                continue;
            }
            usbLineBuf[usbLineLen] = '\0';
            handleUsbCommand(usbLineBuf);
            usbLineLen = 0;
            usbPrintPrompt();
            continue;
        }
        if (c == '\b' || static_cast<uint8_t>(c) == 0x7F) {
            if (usbLineLen > 0) {
                --usbLineLen;
                Serial.print("\b \b");
            }
            continue;
        }
        if (static_cast<uint8_t>(c) < 0x20) {
            continue;
        }
        Serial.write(static_cast<uint8_t>(c));
        if (usbLineLen + 1 >= sizeof(usbLineBuf)) {
            usbLineOverflow = true;
            continue;
        }
        usbLineBuf[usbLineLen++] = c;
    }
}

void handleBleWriteLine(const char* args) {
    uint32_t id = 0;
    uint32_t len = 0;
    char chr[18] = {};
    char hex[UART_LINE_MAX] = {};
    const bool parsed = readUintValue(args, "id", id) &&
                        readKeyValue(args, "char", chr, sizeof(chr)) &&
                        readUintValue(args, "len", len) &&
                        readKeyValue(args, "hex", hex, sizeof(hex));
    if (!parsed || len > UART_PAYLOAD_MAX) {
        sendWriteAck(id, false);
        return;
    }
    uint8_t data[UART_PAYLOAD_MAX] = {};
    size_t decodedLen = 0;
    if (!decodeHex(hex, data, sizeof(data), decodedLen) || decodedLen != len) {
        sendWriteAck(id, false);
        return;
    }
    sendWriteAck(id, writeBleValue(chr, data, decodedLen));
}

void beginBleWriteBin(const char* args) {
    uint32_t len = 0;
    char chr[18] = {};
    uint32_t id = 0;
    const bool parsed = readUintValue(args, "id", id) &&
                        readKeyValue(args, "char", chr, sizeof(chr)) &&
                        readUintValue(args, "len", len);
    if (!parsed || len > UART_PAYLOAD_MAX) {
        sendWriteAck(id, false);
        return;
    }
    uint32_t crc = 0;
    const bool crcPresent = readUintValue(args, "crc", crc);
    pendingBin = true;
    pendingBinTarget = PendingBinTarget::Ble;
    pendingBinId = id;
    pendingBinExpected = len;
    pendingBinLen = 0;
    pendingBinCrc = crc;
    pendingBinCrcPresent = crcPresent;
    strlcpy(pendingBinChr, chr, sizeof(pendingBinChr));
    if (pendingBinExpected == 0) {
        pendingBin = false;
        pendingBinTarget = PendingBinTarget::None;
        sendWriteAck(pendingBinId, writeBleValue(pendingBinChr, nullptr, 0));
    }
}

void handleLine(const char* line) {
    ++uartLineCount;
    if (Serial) {
        Serial.print("[rx] ");
        Serial.println(line);
    }

    if (startsWith(line, "SPECTRE/1 RXCREDIT ")) {
        uint32_t total = 0;
        if (readUintValue(line + strlen("SPECTRE/1 RXCREDIT "), "total", total)) {
            if (!flowCtrlActive) {
                // Engage: baseline both counters here so pre-engage history and
                // the two boards' independent boot counters drop out. In-flight
                // is measured only from this moment forward.
                flowCtrlActive = true;
                flowCtrlSentBase = uartTxFramesSent;
                flowCtrlAckBase = total;
                flowCtrlAckTotal = total;
                flowCtrlLastAckMs = millis();
            } else if (total != flowCtrlAckTotal) {
                flowCtrlAckTotal = total;
                flowCtrlLastAckMs = millis();
            }
        }
        return;
    }
    if (strcmp(line, "SPECTRE/1 HELLO") == 0) {
        sendCaps();
        return;
    }
    if (strcmp(line, "SPECTRE/1 BLE_STATUS") == 0) {
        sendStatus();
        return;
    }
    if (strcmp(line, "SPECTRE/1 BLE_START") == 0) {
        initBle();
        sendStatus();
        return;
    }
    if (strcmp(line, "SPECTRE/1 BLE_DROP") == 0) {
        disconnectPhone("host_drop");
        return;
    }
    if (startsWith(line, "SPECTRE/1 BLE_PROBE ")) {
        uint32_t id = 0;
        uint32_t timeoutMs = DEFAULT_PROBE_TIMEOUT_MS;
        const char* args = line + strlen("SPECTRE/1 BLE_PROBE ");
        readUintValue(args, "id", id);
        readUintValue(args, "timeout", timeoutMs);
        startProbe(id, timeoutMs);
        return;
    }
    if (startsWith(line, "SPECTRE/1 BLE_WRITE_BIN ")) {
        beginBleWriteBin(line + strlen("SPECTRE/1 BLE_WRITE_BIN "));
        return;
    }
    if (startsWith(line, "SPECTRE/1 BLE_WRITE ")) {
        handleBleWriteLine(line + strlen("SPECTRE/1 BLE_WRITE "));
        return;
    }
#if SPECTRE_WIO_SX1262
    if (strcmp(line, "SPECTRE/1 SUBGHZ_STATUS") == 0) {
        sendSubghzStatus();
        return;
    }
    if (startsWith(line, "SPECTRE/1 SUBGHZ_CONFIG ")) {
        handleSubghzConfig(line + strlen("SPECTRE/1 SUBGHZ_CONFIG "));
        return;
    }
    if (startsWith(line, "SPECTRE/1 SUBGHZ_MODE ")) {
        handleSubghzMode(line + strlen("SPECTRE/1 SUBGHZ_MODE "));
        return;
    }
    if (startsWith(line, "SPECTRE/1 SUBGHZ_TX ")) {
        beginSubghzTx(line + strlen("SPECTRE/1 SUBGHZ_TX "));
        return;
    }
#endif
}

void pollUart() {
    while (Serial1.available()) {
        // Any byte from the ESP means the host is alive; this drives the
        // LED-blink / radio-sleep idle gating in pollStatusLed() and loop().
        lastUartRxMs = millis();
        if (pendingBin) {
            while (Serial1.available() && pendingBinLen < pendingBinExpected) {
                const int raw = Serial1.read();
                if (raw < 0) {
                    break;
                }
                pendingBinBuf[pendingBinLen++] = static_cast<uint8_t>(raw);
            }
            if (pendingBinLen < pendingBinExpected) {
                return;
            }
            const PendingBinTarget target = pendingBinTarget;
            const uint32_t id = pendingBinId;
            const size_t binLen = pendingBinLen;
            const uint32_t expectCrc = pendingBinCrc;
            const bool haveCrc = pendingBinCrcPresent;
            char chr[sizeof(pendingBinChr)] = {};
            strlcpy(chr, pendingBinChr, sizeof(chr));
            pendingBin = false;
            pendingBinTarget = PendingBinTarget::None;
            pendingBinChr[0] = '\0';
            pendingBinId = 0;
            pendingBinExpected = 0;
            pendingBinLen = 0;
            pendingBinCrc = 0;
            pendingBinCrcPresent = false;
#if SPECTRE_WIO_SX1262
            if (target == PendingBinTarget::Subghz) {
                loraStartTx(pendingBinBuf, binLen, id);
                continue;
            }
#endif
            // Verify forward-hop integrity before writing to the phone. A byte
            // corrupted/dropped on the S3->nRF UART would otherwise be written
            // verbatim (esp. the auth challenge), which the phone silently
            // rejects -> auth_timeout. NAK so the host re-sends instead.
            if (haveCrc && binLen > 0 &&
                uartFrameCrc32(pendingBinBuf, static_cast<uint16_t>(binLen)) != expectCrc) {
                sendWriteAck(id, false);
                continue;
            }
            sendWriteAck(id, writeBleValue(chr, pendingBinBuf, binLen));
            continue;
        }

        const int raw = Serial1.read();
        if (raw < 0) {
            return;
        }
        const char c = static_cast<char>(raw);
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (lineOverflow) {
                lineLen = 0;
                lineOverflow = false;
                continue;
            }
            if (lineLen == 0) {
                continue;
            }
            lineBuf[lineLen] = '\0';
            handleLine(lineBuf);
            lineLen = 0;
            continue;
        }
        if (static_cast<uint8_t>(c) < 0x20) {
            continue;
        }
        if (lineLen + 1 >= sizeof(lineBuf)) {
            lineOverflow = true;
            lineLen = 0;
            continue;
        }
        lineBuf[lineLen++] = c;
    }
}

void pollProbeTimeout() {
    if (!bleReady || !probe.active || connected) {
        return;
    }
    if (millis() - probe.startedMs < probe.timeoutMs) {
        return;
    }
    Bluefruit.Scanner.stop();
    if (strcmp(probe.err, "none") == 0) {
        if (probe.seen == 0) {
            strlcpy(probe.err, "no_ble_advertisements", sizeof(probe.err));
        } else if (probe.uuidSeen == 0) {
            strlcpy(probe.err, "phone_service_not_advertising", sizeof(probe.err));
        } else {
            strlcpy(probe.err, "phone_not_reachable", sizeof(probe.err));
        }
    }
    sendProbeResult(false);
}

void initBle() {
    if (bleReady) {
        return;
    }
    logUsb(bleInitStarted ? "USB/1 BLE_INIT retry" : "USB/1 BLE_INIT begin");
    bleInitStarted = true;
    Bluefruit.configCentralBandwidth(BANDWIDTH_MAX);
    Bluefruit.begin(0, 1);
    Bluefruit.setTxPower(4);
    Bluefruit.setName("Spectre WIO Relay");
    Bluefruit.Central.setConnectCallback(connectCallback);
    Bluefruit.Central.setDisconnectCallback(disconnectCallback);

    phoneService.begin();
    gpsChr.begin();
    controlChr.begin();
    metadataChr.begin();
    batchChr.begin();
    enrichChr.begin();
    authChr.begin();
    authRequestChr.begin();
    storageChr.begin();
    commandReqChr.begin();
    commandRespChr.begin();
    logStreamChr.begin();
    dashboardStreamChr.begin();
    notificationChr.begin();

    textService.begin();
    textPromptChr.begin();
    textInputChr.begin();
    bleReady = true;
    logUsb("USB/1 BLE_INIT ready");
}

void pollBleInit() {
#if SPECTRE_NRF_AUTO_BLE
    if (bleReady || bleInitStarted) {
        return;
    }
    if (millis() - bootMs < BLE_INIT_DELAY_MS) {
        return;
    }
    bleInitBlocked = true;
    initBle();
    bleInitBlocked = false;
#endif
}

}  // namespace

void setup() {
    bootMs = millis();
    // Treat the host as present through one blink window after boot so the LED
    // shows life immediately; pollUart() refreshes this on real traffic.
    lastUartRxMs = bootMs;
    initDebugLed();
    Serial.begin(115200);
    delay(100);
    logUsb("Spectre WIO nRF relay boot (USB console first, BLE manual)");
#if SPECTRE_WIO_SX1262
    // Initialize the SX1262 (RadioLib SPI begin + GPIO/interrupt setup) BEFORE
    // bringing up the relay UART. RadioLib's SPI/pin bring-up can disturb the
    // shared nRF peripheral/pin state, so Serial1 must be started LAST to stay
    // clean — otherwise the relay UART never receives (regression vs the
    // BLE-only firmware). A missing/failed radio stays non-fatal.
    initLora();
#endif
    // Relay UART comes up last so nothing clobbers it.
    Serial1.begin(UART_BAUD);
    sendCaps();
}

void loop() {
    pollUsbConsole();
    pollStatusLed();
    pollBleInit();
    pollUart();
    pollServiceDiscovery();
    pollProbeTimeout();
#if SPECTRE_WIO_SX1262
    pollSubGhz();
#endif

    const uint32_t now = millis();
    if (now - lastStatusMs >= STATUS_INTERVAL_MS) {
        lastStatusMs = now;
        if (connected) {
            sendStatus();
        }
#if SPECTRE_WIO_SX1262
        if (loraPresent && loraMode != LoraMode::Off) {
            sendSubghzStatus();
        }
#endif
    }

#if SPECTRE_WIO_SX1262
    // Host gone quiet on the UART link: an un-driven radio parked in Standby
    // still burns ~1 mA, so drop it to sleep. Only Standby is touched — an
    // active Rx session is intentional and left alone; the ESP re-arms the PHY
    // (SUBGHZ_CONFIG/MODE) when it comes back.
    if (loraPresent && loraMode == LoraMode::Standby &&
        now - lastUartRxMs >= HOST_IDLE_MS) {
        loraRadio.sleep();
        loraMode = LoraMode::Off;
    }
#endif

    // Drain the outbound queue last, after every producer above has enqueued,
    // paced by the S3's credits (no-op until flow control engages).
    pumpUartTx();

    delay(1);
}
