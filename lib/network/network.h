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
 * Pre-open a TCP socket to the upload server.
 * Must be called BEFORE camera init (camera DMA breaks TCP sockets).
 * Returns true if the connection is established.
 */
bool networkPreConnect();

/**
 * Upload a JPEG frame over the pre-opened TCP socket.
 * Sends raw HTTP request manually since the socket was opened before camera init.
 * Returns HTTP status code (200 = success), or -1 on failure.
 */
int networkUploadPreConnected(camera_fb_t* fb, uint8_t frameSeq,
                              uint16_t batteryMV, uint8_t batteryPct);

/**
 * Upload with retry logic (UPLOAD_RETRIES attempts, exponential backoff).
 * Falls back to fresh connections on retry (may fail after camera init).
 * Returns true if any attempt succeeded.
 */
bool networkUploadWithRetry(camera_fb_t* fb, uint8_t frameSeq,
                            uint16_t batteryMV, uint8_t batteryPct);
