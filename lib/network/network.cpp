#include "network.h"
#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>

// Boot count from main.cpp (RTC_DATA_ATTR)
extern RTC_DATA_ATTR uint32_t bootCount;

// Number of redundant send rounds (blast the same data multiple times)
#ifndef UPLOAD_ROUNDS
#define UPLOAD_ROUNDS 3
#endif

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

// CRC-16/CCITT (polynomial 0x1021, init 0xFFFF)
// Supports chaining: pass previous CRC as init for multi-block computation
static uint16_t crc16(const uint8_t* data, size_t len, uint16_t init = 0xFFFF) {
    uint16_t crc = init;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}

bool networkConnect() {
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

    // BLAST MODE: send fast, no ENOMEM waits, multiple rounds.
    // GDMA corruption is time-dependent — fast sends get more packets through.
    // Header: [session:2][chunk:2][total:2][crc16:2] = 8 bytes
    const size_t MAX_PAYLOAD = 1400;
    const int HEADER_SIZE = 8;

    int sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        Serial.printf("[Network] socket() failed: %d\n", errno);
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

    // Metadata JSON
    char metaJson[512];
    int metaLen = snprintf(metaJson, sizeof(metaJson),
        "{\"camera_id\":\"%s\",\"zone\":\"%s\",\"frame_seq\":%u,"
        "\"battery_mv\":%u,\"battery_pct\":%u,\"wifi_rssi\":%d,"
        "\"wifi_ssid\":\"%s\",\"free_heap\":%u,\"free_psram\":%u,"
        "\"boot_count\":%u,\"uptime_ms\":%lu,\"wake_reason\":%d,"
        "\"fw_version\":\"0.3.0-blast\",\"frame_width\":%u,"
        "\"frame_height\":%u,\"frame_bytes\":%u}",
        CAMERA_ID, ZONE, frameSeq,
        batteryMV, batteryPct, (int)WiFi.RSSI(),
        WiFi.SSID().c_str(), ESP.getFreeHeap(), ESP.getFreePsram(),
        bootCount, millis(), (int)esp_sleep_get_wakeup_cause(),
        fb->width, fb->height, fb->len);

    uint16_t jpegChunks = (fb->len + MAX_PAYLOAD - 1) / MAX_PAYLOAD;
    uint16_t totalChunks = 1 + jpegChunks;

    Serial.printf("[Network] UDP blast: session=0x%04X, %u bytes, %u chunks, %d rounds\n",
                  sessionId, fb->len, totalChunks, UPLOAD_ROUNDS);

    uint8_t pkt[HEADER_SIZE + MAX_PAYLOAD];

    // Build header + compute CRC, then send.
    // CRC covers: [session:2][chunk:2][total:2] + payload (excludes CRC field itself)
    auto sendChunk = [&](uint16_t chunkIdx, const uint8_t* payload, size_t payloadLen) -> bool {
        // Fill header (first 6 bytes)
        pkt[0] = (sessionId >> 8) & 0xFF;
        pkt[1] = sessionId & 0xFF;
        pkt[2] = (chunkIdx >> 8) & 0xFF;
        pkt[3] = chunkIdx & 0xFF;
        pkt[4] = (totalChunks >> 8) & 0xFF;
        pkt[5] = totalChunks & 0xFF;

        // Copy payload after header area (at offset 8, CRC goes at 6-7)
        memcpy(pkt + HEADER_SIZE, payload, payloadLen);

        // CRC over header bytes 0-5 + payload (chained two-pass)
        uint16_t c = crc16(pkt, 6);
        c = crc16(payload, payloadLen, c);

        // CRC into header bytes 6-7
        pkt[6] = (c >> 8) & 0xFF;
        pkt[7] = c & 0xFF;

        int r = lwip_sendto(sock, pkt, HEADER_SIZE + payloadLen, 0,
                            (struct sockaddr*)&dest, sizeof(dest));
        return r >= 0;
    };

    int totalSent = 0;
    int totalFailed = 0;
    unsigned long sendStart = millis();

    for (int round = 0; round < UPLOAD_ROUNDS; round++) {
        int roundSent = 0;
        int roundFail = 0;

        // Chunk 0: metadata
        if (sendChunk(0, (const uint8_t*)metaJson, metaLen)) {
            roundSent++;
        } else {
            roundFail++;
        }

        // JPEG chunks — blast with no delay
        size_t offset = 0;
        for (uint16_t i = 1; i < totalChunks; i++) {
            size_t payloadLen = fb->len - offset;
            if (payloadLen > MAX_PAYLOAD) payloadLen = MAX_PAYLOAD;

            if (sendChunk(i, fb->buf + offset, payloadLen)) {
                roundSent++;
            } else {
                roundFail++;
                // On ENOMEM, yield once to let radio catch up, then continue
                yield();
            }

            offset += payloadLen;
        }

        totalSent += roundSent;
        totalFailed += roundFail;
        Serial.printf("[Network]   Round %d: %d/%u sent, %d ENOMEM\n",
                      round + 1, roundSent, totalChunks, roundFail);

        // Brief yield between rounds to let radio flush
        yield();
        delay(10);
    }

    lwip_close(sock);

    unsigned long elapsed = millis() - sendStart;
    Serial.printf("[Network] Blast done: %d sent / %d failed in %lu ms\n",
                  totalSent, totalFailed, elapsed);
    return totalSent;
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
            Serial.printf("[Network] Upload complete (%d packets across %d rounds)\n",
                          sent, UPLOAD_ROUNDS);
            return true;
        }
    }
    Serial.println("[Network] All upload attempts failed");
    return false;
}
