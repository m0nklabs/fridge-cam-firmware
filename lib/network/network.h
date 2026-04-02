#pragma once

#include <Arduino.h>
#include "esp_camera.h"
#include "config.h"

/**
 * WiFi network functions for Smart Fridge Cam.
 *
 * GDMA WORKAROUND: ESP32-S3 camera GDMA corrupts WiFi DMA TX buffers.
 * Fast "blast" sends with redundancy work around this — the corruption
 * is time-dependent, so sending fast and multiple times overcomes it.
 *
 * UDP Protocol (per packet):
 *   [session_id:2][chunk_idx:2][total_chunks:2][crc16:2] + payload
 *   8-byte header + payload. CRC16 covers the first 6 header bytes + payload.
 *   Server uses CRC to reject corrupted packets.
 *
 * Strategy: send all chunks 3 times (blast rounds), no delays.
 * Server deduplicates by session_id + chunk_idx.
 */

bool networkConnect();
void networkDisconnect();
int8_t networkRSSI();
String networkSSID();

/**
 * Upload via UDP blast: sends all chunks UPLOAD_ROUNDS times with no delays.
 * Returns total unique chunks sent across all rounds, or -1 on failure.
 */
int networkUpload(camera_fb_t* fb, uint8_t frameSeq,
                  uint16_t batteryMV, uint8_t batteryPct);

/**
 * Upload with retry (UPLOAD_RETRIES full attempts).
 */
bool networkUploadWithRetry(camera_fb_t* fb, uint8_t frameSeq,
                            uint16_t batteryMV, uint8_t batteryPct);
