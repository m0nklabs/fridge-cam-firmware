#pragma once

// ============================================================
// GPIO Pin Assignments — Smart Fridge Cam
// ============================================================
// Camera DVP pins are defined in platformio.ini build_flags
// (board-specific, adjusted per variant).
//
// This file defines the custom wiring pins.
// ============================================================

// LDR light sensor (GL5528 + 10kΩ voltage divider)
// Voltage divider: 3.3V → LDR → GPIO → 10kΩ → GND
// Reads high (~1.1-1.65V) when fridge light is on
#define PIN_LDR          1    // ADC1_CH0 — analog read + ext0 wakeup

// Battery voltage monitor (100kΩ + 100kΩ voltage divider)
// Divider halves VBAT: 4.2V cell → 2.1V at ADC
#define PIN_BATTERY_ADC  2    // ADC1_CH1 — analog read

// Status LED (brief flash on capture)
// Freenove board: GPIO48 = onboard RGB LED
// Other boards: wire an external LED + 220Ω resistor
#define PIN_STATUS_LED   48

// SD card — Freenove ESP32-S3-WROOM uses SDMMC 1-bit mode
// Pins are defined in lib/storage/storage.cpp:
//   SD_MMC_CMD = GPIO 38, SD_MMC_CLK = GPIO 39, SD_MMC_D0 = GPIO 40
// No SPI pin defines needed.
