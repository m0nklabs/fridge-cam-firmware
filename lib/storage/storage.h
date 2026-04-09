#pragma once

#include <Arduino.h>
#include "esp_camera.h"
#include "config.h"

/**
 * SD card buffered storage for fridge cam frames.
 *
 * Frames are written to SD during capture (camera active, WiFi off).
 * After capture, files are read back for upload (WiFi active, camera off).
 * This decouples capture speed from upload speed and survives power loss.
 *
 * File layout on SD:
 *   /fc_SSSSSS_NNN.jpg   — JPEG frame (session + sequence number)
 *   Session = boot count (6 digits zero-padded)
 *   NNN = frame sequence (3 digits zero-padded)
 */

/**
 * Initialize SD card via SPI.
 * Returns true on success.
 */
bool storageInit();

/**
 * Deinitialize SD card (release SPI bus).
 */
void storageDeinit();

/**
 * Write a camera frame to SD card.
 * Returns the filename written, or empty string on failure.
 */
String storageWriteFrame(camera_fb_t* fb, uint32_t session, uint16_t seq);

/**
 * Write raw JPEG buffer to SD card.
 * Returns the filename written, or empty string on failure.
 */
String storageWriteBuffer(const uint8_t* buf, size_t len, uint32_t session, uint16_t seq);

/**
 * Get list of pending JPEG files on SD (oldest first).
 * Returns number of files found.
 */
int storageListPending(String* filenames, int maxFiles);

/**
 * Read a file from SD into a PSRAM-allocated buffer.
 * Caller must free() the returned buffer.
 * Returns nullptr on failure; sets *outLen to file size.
 */
uint8_t* storageReadFile(const String& filename, size_t* outLen);

/**
 * Delete a file from SD after successful upload.
 */
bool storageDeleteFile(const String& filename);

/**
 * Get number of pending files on SD.
 */
int storagePendingCount();

/**
 * Get total SD card size and used space (in bytes).
 */
void storageGetInfo(uint64_t* totalBytes, uint64_t* usedBytes);
