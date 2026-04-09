/**
 * Smart Fridge Cam — Main Entry Point (Intake Scan Mode)
 *
 * Single ceiling-mounted camera scans packaging as items go in/out.
 * Rapid continuous capture while door is open → buffer to SD card.
 * After door closes → camera off, WiFi on, upload from SD → sleep.
 *
 * CRITICAL: ESP32-S3 camera GDMA corrupts lwIP TCP stack.
 * Solution: capture phase (camera+SD) and upload phase (WiFi) never overlap.
 * SD card uses SPI — no DMA conflict with camera LCD_CAM peripheral.
 *
 * State machine:
 *   DEEP_SLEEP → [LDR wake] → CAPTURE_LOOP → UPLOAD_LOOP → DEEP_SLEEP
 *                                   ↑              |
 *                                   └── light on? ──┘
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
#include "storage.h"

RTC_DATA_ATTR uint32_t bootCount = 0;

// ---- PSRAM Frame Buffer (fallback when SD unavailable) ----
struct PsramFrame {
    uint8_t* data;
    size_t   len;
    uint16_t width;
    uint16_t height;
};

static PsramFrame psramFrames[MAX_FRAMES_PER_SESSION];
static int psramFrameCount = 0;
static bool useSD = false;  // Set during capturePhase, checked in uploadPhase

static void psramFramesFree() {
    for (int i = 0; i < psramFrameCount; i++) {
        if (psramFrames[i].data) {
            free(psramFrames[i].data);
            psramFrames[i].data = nullptr;
        }
    }
    psramFrameCount = 0;
}

// ---- Capture Phase ----
// Camera + SD active, WiFi off.
// Rapid capture loop: grab frame → write to SD/PSRAM → repeat while light on.
static int capturePhase(uint32_t session, uint16_t batteryMV, uint8_t batteryPct) {
    Serial.println("[Main] === CAPTURE PHASE ===");

    if (!captureInit()) {
        Serial.println("[Main] Camera init failed");
        return 0;
    }

    useSD = storageInit();
    if (!useSD) {
        Serial.println("[Main] SD card init failed — using PSRAM buffer");
        memset(psramFrames, 0, sizeof(psramFrames));
        psramFrameCount = 0;
    }

    uint16_t frameCount = 0;
    unsigned long lastCapture = 0;
    unsigned long lightOffSince = 0;
    bool lightWasOff = false;

    while (frameCount < MAX_FRAMES_PER_SESSION) {
        unsigned long now = millis();

        // Check LDR at regular intervals
        if (now - lastCapture >= LDR_CHECK_INTERVAL_MS || lastCapture == 0) {
            bool lightOn = triggerIsLightOn();

            if (!lightOn) {
                if (!lightWasOff) {
                    lightOffSince = now;
                    lightWasOff = true;
                    Serial.printf("[Main] Light off detected, grace period %d ms\n",
                                  LIGHT_OFF_GRACE_MS);
                } else if (now - lightOffSince >= LIGHT_OFF_GRACE_MS) {
                    Serial.println("[Main] Light off confirmed — ending capture");
                    break;
                }
            } else {
                if (lightWasOff) {
                    Serial.println("[Main] Light back on — continuing capture");
                }
                lightWasOff = false;
            }
        }

        // Capture at defined interval
        if (now - lastCapture >= CAPTURE_INTERVAL_MS || lastCapture == 0) {
            lastCapture = now;

            camera_fb_t* fb = captureSingle();
            if (fb) {
                bool saved = false;
                if (useSD) {
                    String filename = storageWriteFrame(fb, session, frameCount);
                    saved = (filename.length() > 0);
                } else {
                    // PSRAM fallback: copy frame data to PSRAM-allocated buffer
                    uint8_t* copy = (uint8_t*)ps_malloc(fb->len);
                    if (copy) {
                        memcpy(copy, fb->buf, fb->len);
                        psramFrames[psramFrameCount].data = copy;
                        psramFrames[psramFrameCount].len = fb->len;
                        psramFrames[psramFrameCount].width = fb->width;
                        psramFrames[psramFrameCount].height = fb->height;
                        psramFrameCount++;
                        saved = true;
                    } else {
                        Serial.println("[Main] PSRAM alloc failed — out of memory");
                    }
                }
                if (saved) {
                    frameCount++;
                    Serial.printf("[Main] Frame %u saved (%s): %ux%u, %u bytes\n",
                                  frameCount, useSD ? "SD" : "PSRAM",
                                  fb->width, fb->height, fb->len);
                }
                esp_camera_fb_return(fb);
            }
        }

        yield();  // Let ESP32 housekeeping run
    }

    // Deinit camera (frees GDMA for WiFi)
    captureDeinit();

    Serial.printf("[Main] Capture phase done: %u frames in %lu ms\n",
                  frameCount, millis());
    return frameCount;
}

// ---- Upload Phase ----
// WiFi active, camera off. Read from SD or PSRAM → upload.
static int uploadPhase(uint16_t batteryMV, uint8_t batteryPct) {
    Serial.println("[Main] === UPLOAD PHASE ===");

    // Determine what we have to upload
    int pending = useSD ? storagePendingCount() : psramFrameCount;
    if (pending == 0) {
        Serial.println("[Main] No pending frames to upload");
        return 0;
    }

    Serial.printf("[Main] %d frames pending upload (%s)\n", pending, useSD ? "SD" : "PSRAM");

    // Full WiFi + lwIP stack rebuild (post-camera, clean DMA state)
    Serial.println("[Main] Rebuilding WiFi stack...");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();
    esp_wifi_deinit();

    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_destroy(sta);
    }
    delay(200);

    if (!networkConnect()) {
        Serial.println("[Main] WiFi failed — frames stay for next boot");
        return 0;
    }

    int uploaded = 0;

    if (useSD) {
        // SD path: list and upload files
        int batchSize = pending < MAX_UPLOAD_BATCH ? pending : MAX_UPLOAD_BATCH;
        String* filenames = new String[batchSize];
        int fileCount = storageListPending(filenames, batchSize);

        for (int i = 0; i < fileCount; i++) {
            size_t imgLen = 0;
            uint8_t* imgBuf = storageReadFile(filenames[i], &imgLen);
            if (!imgBuf) {
                Serial.printf("[Main] Failed to read %s — skipping\n", filenames[i].c_str());
                continue;
            }

            camera_fb_t fakeFb;
            fakeFb.buf = imgBuf;
            fakeFb.len = imgLen;
            fakeFb.width = 0;
            fakeFb.height = 0;
            fakeFb.format = PIXFORMAT_JPEG;

            bool ok = networkUploadWithRetry(&fakeFb, (uint8_t)i, batteryMV, batteryPct);
            free(imgBuf);

            if (ok) {
                storageDeleteFile(filenames[i]);
                uploaded++;
                Serial.printf("[Main] Uploaded %d/%d: %s\n", uploaded, fileCount, filenames[i].c_str());
            } else {
                Serial.printf("[Main] Upload failed for %s — keeping on SD\n", filenames[i].c_str());
                break;
            }
        }
        delete[] filenames;
    } else {
        // PSRAM path: upload directly from memory buffers
        for (int i = 0; i < psramFrameCount; i++) {
            if (!psramFrames[i].data) continue;

            camera_fb_t fakeFb;
            fakeFb.buf = psramFrames[i].data;
            fakeFb.len = psramFrames[i].len;
            fakeFb.width = psramFrames[i].width;
            fakeFb.height = psramFrames[i].height;
            fakeFb.format = PIXFORMAT_JPEG;

            bool ok = networkUploadWithRetry(&fakeFb, (uint8_t)i, batteryMV, batteryPct);
            if (ok) {
                uploaded++;
                Serial.printf("[Main] Uploaded %d/%d from PSRAM (%u bytes)\n",
                              uploaded, psramFrameCount, psramFrames[i].len);
            } else {
                Serial.printf("[Main] Upload failed for PSRAM frame %d\n", i);
                break;
            }
        }
        psramFramesFree();
    }

    networkDisconnect();

    Serial.printf("[Main] Upload phase done: %d/%d uploaded\n", uploaded, pending);
    return uploaded;
}

void setup() {
    Serial.begin(115200);

    // Wait for USB CDC serial
    unsigned long serialStart = millis();
    while (!Serial && (millis() - serialStart < 3000)) {
        delay(10);
    }
    delay(500);

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
        Serial.println("[FridgeCam] Light off — false trigger");

        // Even on false trigger, check for pending SD uploads from previous sessions
        if (storageInit()) {
            int pending = storagePendingCount();
            if (pending > 0) {
                Serial.printf("[FridgeCam] %d orphaned frames on SD — uploading\n", pending);
                uploadPhase(batteryMV, batteryPct);
            }
            storageDeinit();
        }

        powerDeepSleep();
        return;
    }

    // 4. CAPTURE PHASE: rapid continuous capture to SD while door is open
    uint32_t session = bootCount;
    int captured = capturePhase(session, batteryMV, batteryPct);
    Serial.printf("[FridgeCam] Captured %d frames\n", captured);

    // 5. UPLOAD PHASE: send buffered frames to server
    int uploaded = uploadPhase(batteryMV, batteryPct);
    Serial.printf("[FridgeCam] Uploaded %d frames\n", uploaded);

    // 6. Cleanup and sleep
    if (useSD) storageDeinit();
    psramFramesFree();  // Ensure PSRAM buffers freed even on error paths

    #if LDR_THRESHOLD == 0
        Serial.println("[FridgeCam] TEST MODE — sleeping 30s then reboot");
        delay(30000);
        ESP.restart();
    #else
        // Check if light came back on during upload (door reopened)
        triggerInit();
        if (triggerIsLightOn()) {
            Serial.println("[FridgeCam] Light on again — restarting cycle");
            ESP.restart();
        }
        Serial.println("[FridgeCam] Done. Sleeping...");
        powerDeepSleep();
    #endif
}

void loop() {
    // Never reached — setup() ends with deep sleep or restart
}
