#include <Arduino.h>
#include <nrf_gpio.h>

namespace {

constexpr uint32_t XIAO_LED_RED = NRF_GPIO_PIN_MAP(0, 26);
constexpr uint32_t XIAO_LED_GREEN = NRF_GPIO_PIN_MAP(0, 30);
constexpr uint32_t XIAO_LED_BLUE = NRF_GPIO_PIN_MAP(0, 6);
constexpr uint32_t BLINK_MS = 250;
constexpr uint32_t PRINT_MS = 1000;

uint32_t bootMs = 0;
uint32_t lastBlinkMs = 0;
uint32_t lastPrintMs = 0;
uint32_t tick = 0;
bool ledOn = false;
bool bannerPrinted = false;

void writeXiaoLeds(bool on) {
  const uint32_t level = on ? 0UL : 1UL;
  nrf_gpio_pin_write(XIAO_LED_RED, level);
  nrf_gpio_pin_write(XIAO_LED_GREEN, level);
  nrf_gpio_pin_write(XIAO_LED_BLUE, level);
}

void initXiaoLeds() {
  nrf_gpio_cfg_output(XIAO_LED_RED);
  nrf_gpio_cfg_output(XIAO_LED_GREEN);
  nrf_gpio_cfg_output(XIAO_LED_BLUE);
  writeXiaoLeds(true);
}

void printBanner() {
  if (!Serial || bannerPrinted) {
    return;
  }
  bannerPrinted = true;
  Serial.println();
  Serial.println("SPECTRE NRF USB BLINK DIAG");
  Serial.println("If you can read this, USB CDC and sketch loop are alive.");
  Serial.println("Typing should echo immediately.");
  Serial.print("diag> ");
}

void pollUsbEcho() {
  if (!Serial) {
    bannerPrinted = false;
    return;
  }
  printBanner();
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
      Serial.print("diag> ");
      continue;
    }
    Serial.write(static_cast<uint8_t>(c));
  }
}

void pollBlink() {
  const uint32_t now = millis();
  if (now - lastBlinkMs < BLINK_MS) {
    return;
  }
  lastBlinkMs = now;
  ledOn = !ledOn;
  writeXiaoLeds(ledOn);
}

void pollPrint() {
  if (!Serial) {
    return;
  }
  const uint32_t now = millis();
  if (now - lastPrintMs < PRINT_MS) {
    return;
  }
  lastPrintMs = now;
  Serial.print("diag tick=");
  Serial.print(++tick);
  Serial.print(" uptime_ms=");
  Serial.println(now - bootMs);
  Serial.print("diag> ");
}

}  // namespace

void setup() {
  bootMs = millis();
  initXiaoLeds();
  Serial.begin(115200);
}

void loop() {
  pollBlink();
  pollUsbEcho();
  pollPrint();
  delay(1);
}
