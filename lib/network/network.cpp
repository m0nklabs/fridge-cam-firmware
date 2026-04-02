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

    // UDP upload via raw lwIP socket.
    // Post-GDMA, lwIP pbufs are scarce. Strategy:
    //   - Send ONE packet at a time
    //   - Wait for ENOMEM to clear (=pbuf freed after actual TX)
    //   - Use generous pacing to let radio actually transmit

    const size_t MAX_PAYLOAD = 1400;  // Conservative: well under MTU
    const int HEADER_SIZE = 6;

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

    uint16_t sessionId = (uint16_t)(esp_random() & 0xFFFF);

    // Build metadata
    int8_t rssi = WiFi.RSSI();
    char metaJson[512];
    int metaLen = snprintf(metaJson, sizeof(metaJson),
        "{\"camera_id\":\"%s\",\"zone\":\"%s\",\"frame_seq\":%u,"
        "\"battery_mv\":%u,\"battery_pct\":%u,\"wifi_rssi\":%d,"
        "\"wifi_ssid\":\"%s\",\"free_heap\":%u,\"free_psram\":%u,"
        "\"boot_count\":%u,\"uptime_ms\":%lu,\"wake_reason\":%d,"
        "\"fw_version\":\"0.2.2-slow\",\"frame_width\":%u,"
        "\"frame_height\":%u,\"frame_bytes\":%u}",
        CAMERA_ID, ZONE, frameSeq,
        batteryMV, batteryPct, rssi,
        WiFi.SSID().c_str(), ESP.getFreeHeap(), ESP.getFreePsram(),
        bootCount, millis(), (int)esp_sleep_get_wakeup_cause(),
        fb->width, fb->height, fb->len);

    uint16_t jpegChunks = (fb->len + MAX_PAYLOAD - 1) / MAX_PAYLOAD;
    uint16_t totalChunks = 1 + jpegChunks;

    Serial.printf("[Network] UDP upload: session=0x%04X, %u bytes, %u chunks\n",
                  sessionId, fb->len, totalChunks);

    uint8_t pkt[HEADER_SIZE + MAX_PAYLOAD];
    auto fillHeader = [&](uint16_t chunkIdx) {
        pkt[0] = (sessionId >> 8) & 0xFF;
        pkt[1] = sessionId & 0xFF;
        pkt[2] = (chunkIdx >> 8) & 0xFF;
        pkt[3] = chunkIdx & 0xFF;
        pkt[4] = (totalChunks >> 8) & 0xFF;
        pkt[5] = totalChunks & 0xFF;
    };

    // Helper: send one packet with ENOMEM retry
    auto sendPacket = [&](const uint8_t* data, size_t len) -> bool {
        for (int retry = 0; retry < 200; retry++) {
            int r = lwip_sendto(sock, data, len, 0,
                                (struct sockaddr*)&dest, sizeof(dest));
            if (r >= 0) return true;
            if (errno != ENOMEM) return false;
            // ENOMEM: radio hasn't TX'd the previous packet yet, wait
            delay(25);
            yield();
        }
        return false;  // 200 retries × 25ms = 5s patience per packet
    };

    int chunksSent = 0;
    unsigned long sendStart = millis();

    // Chunk 0: metadata
    fillHeader(0);
    memcpy(pkt + HEADER_SIZE, metaJson, metaLen);
    if (sendPacket(pkt, HEADER_SIZE + metaLen)) {
        chunksSent++;
    } else {
        Serial.printf("[Network] Failed to send metadata (errno=%d)\n", errno);
        lwip_close(sock);
        return -1;
    }
    yield();
    delay(50);  // Give radio time to TX metadata

    // JPEG chunks
    size_t offset = 0;
    for (uint16_t i = 1; i < totalChunks; i++) {
        size_t payloadLen = fb->len - offset;
        if (payloadLen > MAX_PAYLOAD) payloadLen = MAX_PAYLOAD;

        fillHeader(i);
        memcpy(pkt + HEADER_SIZE, fb->buf + offset, payloadLen);

        if (sendPacket(pkt, HEADER_SIZE + payloadLen)) {
            chunksSent++;
        } else {
            Serial.printf("[Network] Chunk %u failed (errno=%d), continuing\n", i, errno);
        }

        offset += payloadLen;

        if (i % 20 == 0) {
            Serial.printf("[Network]   %u/%u chunks (%.0f%%)\n",
                          i, totalChunks, 100.0 * i / totalChunks);
        }

        yield();
    }

    lwip_close(sock);

    unsigned long elapsed = millis() - sendStart;
    Serial.printf("[Network] UDP done: %u/%u chunks in %lu ms\n",
                  chunksSent, totalChunks, elapsed);
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
