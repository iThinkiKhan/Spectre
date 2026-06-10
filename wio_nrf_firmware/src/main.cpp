#include <Arduino.h>
#include <bluefruit.h>
#include <nrf_gpio.h>

#include <ctype.h>
#include <stdint.h>
#include <string.h>

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
constexpr uint32_t DEFAULT_PROBE_TIMEOUT_MS = 12000;
constexpr uint32_t STATUS_INTERVAL_MS = 5000;
constexpr uint32_t USB_BANNER_INTERVAL_MS = 1500;
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 500;
constexpr uint32_t BLE_INIT_DELAY_MS = 3000;
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
int lastRssi = 0;
uint32_t lastStatusMs = 0;
uint32_t lastUsbBannerMs = 0;
uint32_t lastHeartbeatMs = 0;
uint32_t bootMs = 0;
uint32_t usbCommandCount = 0;
uint32_t uartLineCount = 0;
uint32_t uartTxLineCount = 0;
uint32_t bleRxCount = 0;
uint32_t bleWriteCount = 0;
uint32_t ledStatusTicks = 0;
uint32_t usbProbeId = 900000;
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

bool pendingBin = false;
char pendingBinChr[18] = {};
uint32_t pendingBinId = 0;
size_t pendingBinExpected = 0;
size_t pendingBinLen = 0;
uint8_t pendingBinBuf[UART_PAYLOAD_MAX] = {};

void initBle();

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
    if (now - lastHeartbeatMs < HEARTBEAT_INTERVAL_MS) {
        return;
    }
    lastHeartbeatMs = now;
    ++ledStatusTicks;
    setLedColor(connected && servicesReady ? LedColor::Blue : LedColor::Red);
}

bool startsWith(const char* text, const char* prefix) {
    return text && prefix && strncmp(text, prefix, strlen(prefix)) == 0;
}

void sendLine(const char* line) {
    Serial1.print(line);
    Serial1.print("\r\n");
    ++uartTxLineCount;
    if (Serial) {
        Serial.print("[tx] ");
        Serial.println(line);
    }
}

void sendCaps() {
    char line[96] = {};
    snprintf(line, sizeof(line),
             "WIO/1 CAPS BLE_PROXY UART BLE_WRITE_BIN ver=%s",
             SPECTRE_WIO_RELAY_VERSION);
    sendLine(line);
}

void sendStatus() {
    char line[96] = {};
    snprintf(line, sizeof(line),
             "WIO/1 STATUS phone=%s connected=%u rssi=%d",
             connected ? (servicesReady ? "ready" : "connected") : "idle",
             connected ? 1u : 0u,
             lastRssi);
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
    if (strcmp(name, "auth") == 0) return &authChr;
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

void sendBleRxBin(const char* name, const uint8_t* data, uint16_t len) {
    if (!name || (!data && len != 0) || len > UART_PAYLOAD_MAX) {
        return;
    }
    char header[64] = {};
    snprintf(header, sizeof(header),
             "WIO/1 BLE_RX_BIN char=%s len=%u",
             name,
             static_cast<unsigned>(len));
    sendLine(header);
    ++bleRxCount;
    if (len > 0) {
        Serial1.write(data, len);
    }
    Serial1.print("\r\n");
}

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

    if (!discoverPhoneService(handle)) {
        ++probe.connectFailures;
        suppressNextDrop = true;
        Bluefruit.disconnect(handle);
        connected = false;
        servicesReady = false;
        textReady = false;
        connHandle = BLE_CONN_HANDLE_INVALID;
        if (probe.active) {
            sendProbeResult(false);
        }
        return;
    }

    strlcpy(probe.err, "none", sizeof(probe.err));
    if (probe.active) {
        sendProbeResult(true);
    } else {
        sendStatus();
    }
}

void disconnectCallback(uint16_t handle, uint8_t reason) {
    (void)handle;
    snprintf(probe.disc, sizeof(probe.disc), "0x%02X", reason);
    connected = false;
    servicesReady = false;
    textReady = false;
    connHandle = BLE_CONN_HANDLE_INVALID;
    if (suppressNextDrop) {
        suppressNextDrop = false;
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
    pendingBin = true;
    pendingBinId = id;
    pendingBinExpected = len;
    pendingBinLen = 0;
    strlcpy(pendingBinChr, chr, sizeof(pendingBinChr));
    if (pendingBinExpected == 0) {
        pendingBin = false;
        sendWriteAck(pendingBinId, writeBleValue(pendingBinChr, nullptr, 0));
    }
}

void handleLine(const char* line) {
    ++uartLineCount;
    if (Serial) {
        Serial.print("[rx] ");
        Serial.println(line);
    }

    if (strcmp(line, "SPECTRE/1 HELLO") == 0) {
        sendCaps();
        return;
    }
    if (strcmp(line, "SPECTRE/1 BLE_STATUS") == 0) {
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
}

void pollUart() {
    while (Serial1.available()) {
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
            const bool ok = writeBleValue(pendingBinChr, pendingBinBuf, pendingBinLen);
            sendWriteAck(pendingBinId, ok);
            pendingBin = false;
            pendingBinChr[0] = '\0';
            pendingBinId = 0;
            pendingBinExpected = 0;
            pendingBinLen = 0;
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
    if (bleInitStarted) {
        return;
    }
    bleInitStarted = true;
    logUsb("USB/1 BLE_INIT begin");
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
    initDebugLed();
    Serial.begin(115200);
    Serial1.begin(UART_BAUD);
    delay(100);
    logUsb("Spectre WIO nRF relay boot (USB console first, BLE manual)");
    sendCaps();
}

void loop() {
    pollUsbConsole();
    pollStatusLed();
    pollBleInit();
    pollUart();
    pollProbeTimeout();

    const uint32_t now = millis();
    if (now - lastStatusMs >= STATUS_INTERVAL_MS) {
        lastStatusMs = now;
        if (connected) {
            sendStatus();
        }
    }

    delay(1);
}
