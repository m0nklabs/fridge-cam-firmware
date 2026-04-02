#pragma once

#include <Arduino.h>
#include "esp_camera.h"
#include "config.h"

/**
 * Scan networks and connect to the strongest known AP.
 * Returns true if connected within timeout.
 */
bool networkConnect();

/**
 * Disconnect WiFi to save power before sleep.
 */
void networkDisconnect();

/**
 * Get WiFi RSSI (signal strength in dBm). Lower = weaker.
 */
int8_t networkRSSI();

/**
 * Get the SSID we're connected to.
 */
String networkSSID();

/**
 * Upload a JPEG frame to the server via UDP datagrams.
 * Protocol: 6-byte header [session_id:2][chunk_idx:2][total_chunks:2] + payload.
 * Chunk 0 = JSON metadata, chunks 1..N = raw JPEG data.
 * Must be called AFTER WiFi is connected (networkConnect).
 * Returns number of chunks sent, or -1 on failure.
 */
int networkUpload(camera_fb_t* fb, uint8_t frameSeq,
                  uint16_t batteryMV, uint8_t batteryPct);

/**
 * Upload with retry logic (UPLOAD_RETRIES attempts).
 * Returns true if any attempt sent all chunks.
 */
bool networkUploadWithRetry(camera_fb_t* fb, uint8_t frameSeq,
                            uint16_t batteryMV, uint8_t batteryPct);
