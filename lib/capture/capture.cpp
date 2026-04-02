#include "capture.h"

bool captureInit() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = CAM_PIN_D0;
    config.pin_d1       = CAM_PIN_D1;
    config.pin_d2       = CAM_PIN_D2;
    config.pin_d3       = CAM_PIN_D3;
    config.pin_d4       = CAM_PIN_D4;
    config.pin_d5       = CAM_PIN_D5;
    config.pin_d6       = CAM_PIN_D6;
    config.pin_d7       = CAM_PIN_D7;
    config.pin_xclk     = CAM_PIN_XCLK;
    config.pin_pclk     = CAM_PIN_PCLK;
    config.pin_vsync    = CAM_PIN_VSYNC;
    config.pin_href     = CAM_PIN_HREF;
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;
    config.pin_pwdn     = CAM_PIN_PWDN;
    config.pin_reset    = CAM_PIN_RESET;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode    = CAMERA_GRAB_LATEST;

    // Use PSRAM for framebuffer
    if (psramFound()) {
        config.frame_size   = CAMERA_RESOLUTION;
        config.jpeg_quality = JPEG_QUALITY;
        config.fb_count     = 2;
        config.fb_location  = CAMERA_FB_IN_PSRAM;
        Serial.printf("[Capture] PSRAM found: %u bytes free\n", ESP.getFreePsram());
    } else {
        // Fallback without PSRAM — lower resolution
        config.frame_size   = FRAMESIZE_SVGA;
        config.jpeg_quality = 12;
        config.fb_count     = 1;
        config.fb_location  = CAMERA_FB_IN_DRAM;
        Serial.println("[Capture] WARNING: No PSRAM, using SVGA fallback");
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[Capture] Camera init FAILED: 0x%x\n", err);
        return false;
    }

    // Warm up: discard first 2 frames (often garbage/green)
    for (int i = 0; i < 2; i++) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) esp_camera_fb_return(fb);
        delay(100);
    }

    Serial.println("[Capture] Camera initialized OK");
    return true;
}

camera_fb_t* captureSingle() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[Capture] Frame capture failed");
        return nullptr;
    }
    Serial.printf("[Capture] Frame: %ux%u, %u bytes\n", fb->width, fb->height, fb->len);
    return fb;
}

float captureSharpness(camera_fb_t* fb) {
    // Simple sharpness estimate: variance of byte-level differences
    // For JPEG, we approximate by looking at compressed size vs resolution
    // Sharper images compress less efficiently (more detail = larger JPEG)
    // This is a fast heuristic that avoids decoding the JPEG on ESP32
    if (!fb || fb->len == 0) return 0.0f;

    float pixels = (float)(fb->width * fb->height);
    float bytesPerPixel = (float)fb->len / pixels;

    // Also look at high-frequency content in the JPEG stream
    // Count byte transitions > 32 (indicates detail, not smooth gradients)
    uint32_t transitions = 0;
    for (size_t i = 1; i < fb->len && i < 20000; i++) {
        int diff = abs((int)fb->buf[i] - (int)fb->buf[i - 1]);
        if (diff > 32) transitions++;
    }
    float transitionRate = (float)transitions / (float)(fb->len < 20000 ? fb->len : 20000);

    float sharpness = bytesPerPixel * 1000.0f + transitionRate * 10000.0f;
    Serial.printf("[Capture] Sharpness: %.1f (bpp=%.3f, transitions=%.3f)\n",
                  sharpness, bytesPerPixel, transitionRate);
    return sharpness;
}

camera_fb_t* captureBurst() {
    Serial.printf("[Capture] Burst: %d frames, %d ms interval\n",
                  BURST_FRAMES, BURST_INTERVAL_MS);

    camera_fb_t* best = nullptr;
    float bestSharp = -1.0f;
    int bestIdx = -1;
    int captured = 0;

    for (int i = 0; i < BURST_FRAMES; i++) {
        camera_fb_t* fb = captureSingle();
        if (fb) {
            captured++;
            float sharp = captureSharpness(fb);
            if (sharp > bestSharp) {
                // New best — release previous best
                if (best) esp_camera_fb_return(best);
                best = fb;
                bestSharp = sharp;
                bestIdx = i;
            } else {
                // Not better — release immediately
                esp_camera_fb_return(fb);
            }
        }
        if (i < BURST_FRAMES - 1) {
            delay(BURST_INTERVAL_MS);
        }
    }

    Serial.printf("[Capture] Burst done: %d/%d captured, best=#%d (sharpness %.1f)\n",
                  captured, BURST_FRAMES, bestIdx, bestSharp);

    return best;  // Caller must return this (nullptr if all failed)
}

void captureDeinit() {
    esp_camera_deinit();
    Serial.println("[Capture] Camera deinitialized");
}
