#pragma once

#include <Arduino.h>
#include "pins.h"

/**
 * Initialize battery ADC pin.
 */
void powerInit();

/**
 * Read battery voltage in millivolts.
 * Uses 100k+100k voltage divider (2:1 ratio).
 */
uint16_t powerReadVoltageMV();

/**
 * Convert battery voltage to percentage (simple linear mapping).
 * 3300mV = 0%, 4200mV = 100%.
 */
uint8_t powerVoltageToPct(uint16_t mv);

/**
 * Enter deep sleep. Configures LDR wakeup before sleeping.
 */
void powerDeepSleep();
