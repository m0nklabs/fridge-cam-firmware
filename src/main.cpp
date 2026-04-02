/**
 * Smart Fridge Cam — Main Entry Point
 *
 * Wake from deep sleep on LDR trigger → capture → upload → sleep.
 * Zero inference. All AI runs on the server.
 *
 * See docs/ARCHITECTURE.md for the full state machine.
 */

#include <Arduino.h>
#include "config.h"
#include "pins.h"

// TODO: Implement these modules
// #include "capture.h"    // Camera init, burst/stream capture
// #include "network.h"    // WiFi connect, HTTP upload
// #include "power.h"      // Battery ADC, deep sleep

RTC_DATA_ATTR uint32_t bootCount = 0;

void setup() {
    Serial.begin(115200);
    bootCount++;
    Serial.printf("[FridgeCam] Boot #%u, wake reason: %d\n",
                  bootCount, esp_sleep_get_wakeup_cause());

    // TODO: Read battery voltage
    // float batteryV = readBatteryVoltage();
    // int batteryPct = voltageToPct(batteryV);

    // TODO: Re-check LDR — confirm light is still on (debounce)
    // int ldrValue = analogRead(PIN_LDR);
    // if (ldrValue < LDR_THRESHOLD) {
    //     Serial.println("[FridgeCam] False trigger, going back to sleep");
    //     goToSleep();
    //     return;
    // }

    // TODO: Connect WiFi
    // if (!connectWiFi(WIFI_SSID, WIFI_PASSWORD, WIFI_TIMEOUT_MS)) {
    //     Serial.println("[FridgeCam] WiFi failed, sleeping");
    //     goToSleep();
    //     return;
    // }

    // TODO: Init camera
    // if (!initCamera()) {
    //     Serial.println("[FridgeCam] Camera init failed, sleeping");
    //     goToSleep();
    //     return;
    // }

    // TODO: Capture + upload based on mode
    // #if CAPTURE_MODE == BURST
    //     captureAndUploadBurst();
    // #else
    //     captureAndUploadStream();
    // #endif

    // TODO: Deep sleep
    // goToSleep();

    Serial.println("[FridgeCam] Firmware skeleton — implement capture modules");
    Serial.println("[FridgeCam] Going to deep sleep in 5 seconds...");
    delay(5000);

    // Configure ext0 wakeup on LDR pin (wake on HIGH = light detected)
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_LDR, 1);
    esp_deep_sleep_start();
}

void loop() {
    // Never reached — setup() ends with deep sleep
}
