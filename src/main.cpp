/**
 * Smart Fridge Cam — Main Entry Point
 *
 * Wake from deep sleep on LDR trigger → capture → upload → sleep.
 * Zero inference. All AI runs on the server.
 *
 * See docs/ARCHITECTURE.md for the full state machine.
 */

#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "pins.h"
#include "trigger.h"
#include "power.h"
#include "capture.h"
#include "network.h"

RTC_DATA_ATTR uint32_t bootCount = 0;

static void runBurstMode(uint16_t batteryMV, uint8_t batteryPct) {
    camera_fb_t* best = captureBurst();
    if (!best) {
        Serial.println("[FridgeCam] Burst capture failed");
        return;
    }
    networkUploadWithRetry(best, 0, batteryMV, batteryPct);
    esp_camera_fb_return(best);
}

static void runStreamMode(uint16_t batteryMV, uint8_t batteryPct) {
    int totalFrames = (STREAM_DURATION_S * 1000) / STREAM_INTERVAL_MS;
    Serial.printf("[FridgeCam] Stream mode: %d frames over %d s\n",
                  totalFrames, STREAM_DURATION_S);

    for (int seq = 0; seq < totalFrames; seq++) {
        camera_fb_t* fb = captureSingle();
        if (fb) {
            networkUploadWithRetry(fb, (uint8_t)seq, batteryMV, batteryPct);
            esp_camera_fb_return(fb);
        }
        if (seq < totalFrames - 1) {
            delay(STREAM_INTERVAL_MS);
        }
    }
}

void setup() {
    Serial.begin(115200);

    // Wait for USB CDC serial to be ready (native USB needs time)
    unsigned long serialStart = millis();
    while (!Serial && (millis() - serialStart < 3000)) {
        delay(10);
    }
    delay(500);  // Extra settle time

    bootCount++;
    esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
    Serial.printf("\n[FridgeCam] ===== Boot #%u, wake reason: %d =====\n",
                  bootCount, wakeReason);
    Serial.printf("[FridgeCam] Free heap: %u, PSRAM: %u\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());

    // 1. Init peripherals
    triggerInit();
    powerInit();

    // 2. Read battery
    uint16_t batteryMV = powerReadVoltageMV();
    uint8_t batteryPct = powerVoltageToPct(batteryMV);
    Serial.printf("[FridgeCam] Battery: %u mV (%u%%)\n", batteryMV, batteryPct);

    // 3. Re-check LDR — debounce false triggers
    if (!triggerIsLightOn()) {
        Serial.println("[FridgeCam] Light off — false trigger, sleeping");
        powerDeepSleep();
        return;
    }

    // 4. Connect WiFi (auto-selects strongest known AP)
    if (!networkConnect()) {
        Serial.println("[FridgeCam] WiFi failed, sleeping");
        powerDeepSleep();
        return;
    }

    // DEBUG: Test TCP before camera init
    {
        WiFiClient tc;
        IPAddress srv;
        srv.fromString(SERVER_HOST);
        Serial.printf("[FridgeCam] TCP test BEFORE camera init → %s:%d ... ",
                      SERVER_HOST, SERVER_PORT);
        if (tc.connect(srv, SERVER_PORT, 5000)) {
            Serial.println("OK ✓");
            tc.stop();
        } else {
            Serial.printf("FAILED (errno %d)\n", errno);
        }
    }

    // 5. Init camera
    if (!captureInit()) {
        Serial.println("[FridgeCam] Camera failed, sleeping");
        networkDisconnect();
        powerDeepSleep();
        return;
    }

    // 6. Capture + upload
    #if CAPTURE_MODE == CAPTURE_MODE_BURST
        runBurstMode(batteryMV, batteryPct);
    #elif CAPTURE_MODE == CAPTURE_MODE_STREAM
        runStreamMode(batteryMV, batteryPct);
    #endif

    // 7. Cleanup and sleep
    captureDeinit();
    networkDisconnect();

    #if LDR_THRESHOLD == 0
        // Testing mode: no LDR, don't deep sleep — reboot after delay
        Serial.println("[FridgeCam] TEST MODE — sleeping 30s then reboot");
        delay(30000);
        ESP.restart();
    #else
        Serial.println("[FridgeCam] Done. Sleeping...");
        powerDeepSleep();
    #endif
}

void loop() {
    // Never reached — setup() ends with deep sleep
}
