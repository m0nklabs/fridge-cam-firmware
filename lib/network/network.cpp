#include "network.h"
#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>

// Boot count from main.cpp (RTC_DATA_ATTR)
extern RTC_DATA_ATTR uint32_t bootCount;

// Known AP list
struct KnownAP {
    const char* ssid;
    const char* pass;
};

static const KnownAP knownAPs[] = {
    {WIFI_SSID_1, WIFI_PASSWORD_1},
    #if WIFI_AP_COUNT >= 2
    {WIFI_SSID_2, WIFI_PASSWORD_2},
    #endif
};
static const int numKnownAPs = sizeof(knownAPs) / sizeof(knownAPs[0]);

bool networkConnect() {
    // Try connecting up to 3 times with full WiFi reset between attempts
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            Serial.printf("[Network] WiFi retry %d/3 — full radio reset\n", attempt + 1);
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            delay(500);
        }

        WiFi.mode(WIFI_STA);
        delay(100);

        Serial.println("[Network] Scanning...");
        int n = WiFi.scanNetworks();

        int bestIdx = -1;
        int bestRSSI = -999;
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < numKnownAPs; j++) {
                if (strlen(knownAPs[j].ssid) > 0 && WiFi.SSID(i) == knownAPs[j].ssid) {
                    Serial.printf("[Network]   %s: %d dBm\n", knownAPs[j].ssid, WiFi.RSSI(i));
                    if (WiFi.RSSI(i) > bestRSSI) {
                        bestRSSI = WiFi.RSSI(i);
                        bestIdx = j;
                    }
                }
            }
        }
        WiFi.scanDelete();

        if (bestIdx < 0) {
            Serial.println("[Network] No known AP found");
            continue;
        }

        Serial.printf("[Network] Connecting to %s (%d dBm)", knownAPs[bestIdx].ssid, bestRSSI);
        WiFi.begin(knownAPs[bestIdx].ssid, knownAPs[bestIdx].pass);

        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED) {
            if (millis() - start > WIFI_TIMEOUT_MS) {
                Serial.printf(" TIMEOUT (status=%d)\n", WiFi.status());
                break;
            }
            delay(250);
            Serial.printf("[%d]", WiFi.status());
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf(" OK (%s, RSSI: %d dBm)\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            return true;
        }
    }

    Serial.println("[Network] All WiFi attempts failed");
    return false;
}

void networkDisconnect() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[Network] WiFi off");
}

int8_t networkRSSI() {
    return WiFi.RSSI();
}

String networkSSID() {
    return WiFi.SSID();
}

int networkUpload(camera_fb_t* fb, uint8_t frameSeq,
                  uint16_t batteryMV, uint8_t batteryPct) {
    if (!fb || fb->len == 0) {
        Serial.println("[Network] No frame to upload");
        return -1;
    }

    // UDP upload — bypasses broken lwIP TCP task after GDMA corruption.
    // Protocol: 6-byte header per packet + payload.
    // Chunk 0 = JSON metadata, chunks 1..N = raw JPEG data.

    const int MAX_PAYLOAD = 1466;  // 1472 MTU - 6 byte header
    const int HEADER_SIZE = 6;

    // Create UDP socket
    int sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        Serial.printf("[Network] UDP socket() failed: %d\n", errno);
        return -1;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(UDP_PORT);
    IPAddress serverIP;
    serverIP.fromString(SERVER_HOST);
    dest.sin_addr.s_addr = (uint32_t)serverIP;

    // Random session ID to group packets
    uint16_t sessionId = (uint16_t)(esp_random() & 0xFFFF);

    // Build JSON metadata for chunk 0
    int8_t rssi = WiFi.RSSI();
    char metaJson[512];
    int metaLen = snprintf(metaJson, sizeof(metaJson),
        "{\"camera_id\":\"%s\",\"zone\":\"%s\",\"frame_seq\":%u,"
        "\"battery_mv\":%u,\"battery_pct\":%u,\"wifi_rssi\":%d,"
        "\"wifi_ssid\":\"%s\",\"free_heap\":%u,\"free_psram\":%u,"
        "\"boot_count\":%u,\"uptime_ms\":%lu,\"wake_reason\":%d,"
        "\"fw_version\":\"0.2.0-udp\",\"frame_width\":%u,"
        "\"frame_height\":%u,\"frame_bytes\":%u}",
        CAMERA_ID, ZONE, frameSeq,
        batteryMV, batteryPct, rssi,
        WiFi.SSID().c_str(), ESP.getFreeHeap(), ESP.getFreePsram(),
        bootCount, millis(), (int)esp_sleep_get_wakeup_cause(),
        fb->width, fb->height, fb->len);

    // Calculate total chunks: 1 (metadata) + ceil(jpeg_len / MAX_PAYLOAD)
    uint16_t jpegChunks = (fb->len + MAX_PAYLOAD - 1) / MAX_PAYLOAD;
    uint16_t totalChunks = 1 + jpegChunks;

    Serial.printf("[Network] UDP upload: session=0x%04X, %u bytes JPEG, %u chunks\n",
                  sessionId, fb->len, totalChunks);

    // Packet buffer (header + payload)
    uint8_t pkt[HEADER_SIZE + MAX_PAYLOAD];

    // Helper to fill header
    auto fillHeader = [&](uint16_t chunkIdx) {
        pkt[0] = (sessionId >> 8) & 0xFF;
        pkt[1] = sessionId & 0xFF;
        pkt[2] = (chunkIdx >> 8) & 0xFF;
        pkt[3] = chunkIdx & 0xFF;
        pkt[4] = (totalChunks >> 8) & 0xFF;
        pkt[5] = totalChunks & 0xFF;
    };

    int chunksSent = 0;

    // Send chunk 0: metadata JSON
    fillHeader(0);
    memcpy(pkt + HEADER_SIZE, metaJson, metaLen);
    int sent = lwip_sendto(sock, pkt, HEADER_SIZE + metaLen, 0,
                           (struct sockaddr*)&dest, sizeof(dest));
    if (sent < 0) {
        Serial.printf("[Network] UDP send meta failed: errno=%d\n", errno);
        lwip_close(sock);
        return -1;
    }
    chunksSent++;
    delay(10);  // Let metadata packet transmit before flooding

    // Send chunks 1..N: JPEG data
    size_t offset = 0;
    for (uint16_t i = 1; i < totalChunks; i++) {
        size_t payloadLen = fb->len - offset;
        if (payloadLen > MAX_PAYLOAD) payloadLen = MAX_PAYLOAD;

        fillHeader(i);
        memcpy(pkt + HEADER_SIZE, fb->buf + offset, payloadLen);

        // Retry on ENOMEM — lwIP pbufs need time to drain
        int retries = 0;
        while (true) {
            sent = lwip_sendto(sock, pkt, HEADER_SIZE + payloadLen, 0,
                               (struct sockaddr*)&dest, sizeof(dest));
            if (sent >= 0) {
                chunksSent++;
                break;
            }
            if (errno == ENOMEM && retries < 50) {
                // Buffer full — wait for TX to drain
                retries++;
                delay(20);
                yield();
            } else {
                Serial.printf("[Network] UDP chunk %u failed: errno=%d (retries=%d)\n",
                              i, errno, retries);
                break;
            }
        }

        offset += payloadLen;

        // Progress every 20 chunks
        if (i % 20 == 0) {
            Serial.printf("[Network]   %u/%u chunks (%.0f%%)\n",
                          i, totalChunks, 100.0 * i / totalChunks);
        }

        // Pace: 10ms base + yield to let lwIP process TX
        delay(10);
        yield();
    }

    lwip_close(sock);
    Serial.printf("[Network] UDP upload done: %u/%u chunks sent\n",
                  chunksSent, totalChunks);
    return chunksSent;
}

bool networkUploadWithRetry(camera_fb_t* fb, uint8_t frameSeq,
                            uint16_t batteryMV, uint8_t batteryPct) {
    for (int attempt = 0; attempt < UPLOAD_RETRIES; attempt++) {
        if (attempt > 0) {
            Serial.printf("[Network] Retry %d/%d\n", attempt + 1, UPLOAD_RETRIES);
            delay(500);
        }

        int sent = networkUpload(fb, frameSeq, batteryMV, batteryPct);
        if (sent > 0) {
            Serial.printf("[Network] Upload succeeded (%d chunks)\n", sent);
            return true;
        }
    }
    Serial.println("[Network] All upload attempts failed");
    return false;
}
