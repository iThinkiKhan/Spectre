#include "variant.h"

#include "nrf.h"
#include "wiring_constants.h"
#include "wiring_digital.h"

const uint32_t g_ADigitalPinMap[] = {
    2,   // D0  is P0.02 (A0)
    3,   // D1  is P0.03 (A1)
    28,  // D2  is P0.28 (A2)
    29,  // D3  is P0.29 (A3)
    4,   // D4  is P0.04 (A4, SDA)
    5,   // D5  is P0.05 (A5, SCL)
    43,  // D6  is P1.11 (UART TX)
    44,  // D7  is P1.12 (UART RX)
    45,  // D8  is P1.13 (SCK)
    46,  // D9  is P1.14 (MISO)
    47,  // D10 is P1.15 (MOSI)
    26,  // D11 is P0.26 (LED RED)
    6,   // D12 is P0.06 (LED BLUE)
    30,  // D13 is P0.30 (LED GREEN)
    14,  // D14 is P0.14 (READ_BAT)
    40,  // D15 is P1.08 (IMU power)
    27,  // D16 is P0.27 (IMU I2C SCL)
    7,   // D17 is P0.07 (IMU I2C SDA)
    11,  // D18 is P0.11 (IMU INT1)
    42,  // D19 is P1.10 (MIC power)
    32,  // D20 is P1.00 (PDM clock)
    16,  // D21 is P0.16 (PDM data)
    13,  // D22 is P0.13 (HICHG)
    17,  // D23 is P0.17 (~CHG)
    21,  // D24 is P0.21 (QSPI SCK)
    25,  // D25 is P0.25 (QSPI CS)
    20,  // D26 is P0.20 (QSPI IO0)
    24,  // D27 is P0.24 (QSPI IO1)
    22,  // D28 is P0.22 (QSPI IO2)
    23,  // D29 is P0.23 (QSPI IO3)
    9,   // D30 is P0.09 (NFC1)
    10,  // D31 is P0.10 (NFC2)
    31,  // D32 is P0.31 (VBAT)
};

void initVariant() {
    pinMode(PIN_QSPI_CS, OUTPUT);
    digitalWrite(PIN_QSPI_CS, HIGH);

    pinMode(LED_RED, OUTPUT);
    pinMode(LED_GREEN, OUTPUT);
    pinMode(LED_BLUE, OUTPUT);
    digitalWrite(LED_RED, HIGH);
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_BLUE, HIGH);
}
