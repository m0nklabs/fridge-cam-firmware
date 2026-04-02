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

    // 4. Connect WiFi FIRST (must happen before camera init)
    //    Camera DMA permanently breaks TCP sockets on ESP32-S3.
    if (!networkConnect()) {
        Serial.println("[FridgeCam] WiFi failed, sleeping");
        powerDeepSleep();
        return;
    }

    // 5. Pre-open TCP socket to server (before camera DMA corrupts lwIP)
    if (!networkPreConnect()) {
        Serial.println("[FridgeCam] TCP pre-connect failed, sleeping");
        networkDisconnect();
        powerDeepSleep();
        return;
    }

    // 6. Init camera (WARNING: this breaks new TCP sockets via GDMA)
    if (!captureInit()) {
        Serial.println("[FridgeCam] Camera failed, sleeping");
        networkDisconnect();
        powerDeepSleep();
        return;
    }

    // 7. Capture burst
    camera_fb_t* best = captureBurst();
    uint8_t* imgBuf = nullptr;
    size_t imgLen = 0;
    uint16_t imgW = 0, imgH = 0;

    if (best) {
        imgLen = best->len;
        imgW = best->width;
        imgH = best->height;
        imgBuf = (uint8_t*)ps_malloc(imgLen);
        if (imgBuf) {
            memcpy(imgBuf, best->buf, imgLen);
        }
        esp_camera_fb_return(best);
    }

    // 8. Deinit camera (frees DMA channels)
    captureDeinit();

    // Give lwIP time to recover from GDMA interference
    delay(500);
    yield();

    if (!imgBuf) {
        Serial.println("[FridgeCam] Capture failed or alloc failed, sleeping");
        networkDisconnect();
        powerDeepSleep();
        return;
    }

    Serial.printf("[FridgeCam] Image ready: %ux%u, %u bytes\n", imgW, imgH, imgLen);

    // 9. Upload over pre-connected socket
    camera_fb_t fakeFb;
    fakeFb.buf = imgBuf;
    fakeFb.len = imgLen;
    fakeFb.width = imgW;
    fakeFb.height = imgH;
    fakeFb.format = PIXFORMAT_JPEG;

    networkUploadWithRetry(&fakeFb, 0, batteryMV, batteryPct);
    free(imgBuf);

    // 10. Cleanup and sleep
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
