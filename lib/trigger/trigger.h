#pragma once

#include <Arduino.h>
#include "pins.h"
#include "config.h"

/**
 * Initialize LDR pin for analog reading.
 */
void triggerInit();

/**
 * Read the current LDR value (12-bit ADC, 0-4095).
 */
int triggerReadLDR();

/**
 * Check if the fridge light is on (above threshold).
 */
bool triggerIsLightOn();

/**
 * Configure ext0 deep sleep wakeup on LDR pin (wake on HIGH).
 */
void triggerConfigureWakeup();
