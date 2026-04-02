#include "network.h"
#include <WiFi.h>
#include <WiFiUdp.h>

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

    // UDP upload via Arduino WiFiUDP — avoids raw lwIP socket issues post-GDMA.
    // Protocol: 6-byte header per packet + payload.
    // Chunk 0 = JSON metadata, chunks 1..N = raw JPEG data.

    const size_t MAX_PAYLOAD = 1466;  // 1472 MTU - 6 byte header
    const int HEADER_SIZE = 6;

    WiFiUDP udp;
    IPAddress serverIP;
    serverIP.fromString(SERVER_HOST);

    uint16_t sessionId = (uint16_t)(esp_random() & 0xFFFF);

    // Build JSON metadata
    int8_t rssi = WiFi.RSSI();
    char metaJson[512];
    int metaLen = snprintf(metaJson, sizeof(metaJson),
        "{\"camera_id\":\"%s\",\"zone\":\"%s\",\"frame_seq\":%u,"
        "\"battery_mv\":%u,\"battery_pct\":%u,\"wifi_rssi\":%d,"
        "\"wifi_ssid\":\"%s\",\"free_heap\":%u,\"free_psram\":%u,"
        "\"boot_count\":%u,\"uptime_ms\":%lu,\"wake_reason\":%d,"
        "\"fw_version\":\"0.2.1-wifiudp\",\"frame_width\":%u,"
        "\"frame_height\":%u,\"frame_bytes\":%u}",
        CAMERA_ID, ZONE, frameSeq,
        batteryMV, batteryPct, rssi,
        WiFi.SSID().c_str(), ESP.getFreeHeap(), ESP.getFreePsram(),
        bootCount, millis(), (int)esp_sleep_get_wakeup_cause(),
        fb->width, fb->height, fb->len);

    uint16_t jpegChunks = (fb->len + MAX_PAYLOAD - 1) / MAX_PAYLOAD;
    uint16_t totalChunks = 1 + jpegChunks;

    Serial.printf("[Network] UDP upload: session=0x%04X, %u bytes JPEG, %u chunks\n",
                  sessionId, fb->len, totalChunks);

    uint8_t header[HEADER_SIZE];
    auto fillHeader = [&](uint16_t chunkIdx) {
        header[0] = (sessionId >> 8) & 0xFF;
        header[1] = sessionId & 0xFF;
        header[2] = (chunkIdx >> 8) & 0xFF;
        header[3] = chunkIdx & 0xFF;
        header[4] = (totalChunks >> 8) & 0xFF;
        header[5] = totalChunks & 0xFF;
    };

    int chunksSent = 0;
    unsigned long sendStart = millis();

    // Send chunk 0: metadata
    fillHeader(0);
    if (udp.beginPacket(serverIP, UDP_PORT)) {
        udp.write(header, HEADER_SIZE);
        udp.write((uint8_t*)metaJson, metaLen);
        if (udp.endPacket()) {
            chunksSent++;
        } else {
            Serial.println("[Network] UDP meta endPacket failed");
        }
    }
    delay(15);

    // Send JPEG chunks
    size_t offset = 0;
    for (uint16_t i = 1; i < totalChunks; i++) {
        size_t payloadLen = fb->len - offset;
        if (payloadLen > MAX_PAYLOAD) payloadLen = MAX_PAYLOAD;

        fillHeader(i);

        // Retry on failure (ENOMEM / buffer full)
        for (int retry = 0; retry < 20; retry++) {
            if (udp.beginPacket(serverIP, UDP_PORT)) {
                udp.write(header, HEADER_SIZE);
                udp.write(fb->buf + offset, payloadLen);
                if (udp.endPacket()) {
                    chunksSent++;
                    break;
                }
            }
            // endPacket failed — wait for TX to drain
            delay(25);
            yield();
        }

        offset += payloadLen;

        if (i % 20 == 0) {
            Serial.printf("[Network]   %u/%u chunks (%.0f%%)\n",
                          i, totalChunks, 100.0 * i / totalChunks);
        }

        delay(15);
        yield();
    }

    Serial.printf("[Network] UDP upload done: %u/%u chunks in %lu ms\n",
                  chunksSent, totalChunks, millis() - sendStart);
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
