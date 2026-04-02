/**
 * Smart Fridge Cam — Main Entry Point
 *
 * Wake from deep sleep on LDR trigger → capture → upload → sleep.
 * Zero inference. All AI runs on the server.
 *
 * CRITICAL: ESP32-S3 camera GDMA corrupts lwIP TCP stack.
 * Workaround: full WiFi stack restart after camera deinit.
 *
 * See docs/ARCHITECTURE.md for the full state machine.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_netif.h>
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

    // 4. Init camera FIRST (before WiFi)
    //    GDMA from camera init corrupts shared DMA paths.
    //    By doing camera first and WiFi after, the WiFi stack gets a
    //    clean rebuild on a fresh netif that doesn't share DMA state.
    if (!captureInit()) {
        Serial.println("[FridgeCam] Camera failed, sleeping");
        powerDeepSleep();
        return;
    }

    // 5. Capture burst
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

    // 6. Deinit camera (frees DMA channels)
    captureDeinit();

    if (!imgBuf) {
        Serial.println("[FridgeCam] Capture failed or alloc failed, sleeping");
        powerDeepSleep();
        return;
    }

    Serial.printf("[FridgeCam] Image ready: %ux%u, %u bytes\n", imgW, imgH, imgLen);

    // 7. Full WiFi + lwIP stack teardown and rebuild.
    //    Critical: destroy STA netif so Arduino creates a FRESH one
    //    that doesn't share any DMA state with the camera's GDMA paths.
    Serial.println("[FridgeCam] Rebuilding WiFi stack (post-camera)...");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();
    esp_wifi_deinit();

    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_destroy(sta);
        Serial.println("[FridgeCam] Destroyed STA netif");
    }
    delay(200);

    if (!networkConnect()) {
        Serial.println("[FridgeCam] WiFi failed, sleeping");
        free(imgBuf);
        powerDeepSleep();
        return;
    }

    // 8. Upload via UDP
    camera_fb_t fakeFb;
    fakeFb.buf = imgBuf;
    fakeFb.len = imgLen;
    fakeFb.width = imgW;
    fakeFb.height = imgH;
    fakeFb.format = PIXFORMAT_JPEG;

    networkUploadWithRetry(&fakeFb, 0, batteryMV, batteryPct);
    free(imgBuf);

    // 9. Cleanup and sleep
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
