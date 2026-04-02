#pragma once

#include <Arduino.h>
#include "esp_camera.h"
#include "config.h"

/**
 * Initialize camera with DVP pins from platformio.ini build_flags.
 * Returns true on success.
 */
bool captureInit();

/**
 * Capture a single JPEG frame.
 * Caller must call esp_camera_fb_return() when done.
 */
camera_fb_t* captureSingle();

/**
 * Calculate sharpness of a JPEG frame using Laplacian variance
 * on the luminance channel. Higher = sharper.
 */
float captureSharpness(camera_fb_t* fb);

/**
 * Burst capture: take BURST_FRAMES frames with BURST_INTERVAL_MS delay,
 * return the sharpest one. All other frames are freed.
 * Caller must call esp_camera_fb_return() on the result.
 */
camera_fb_t* captureBurst();

/**
 * Deinitialize camera to free resources before sleep.
 */
void captureDeinit();
