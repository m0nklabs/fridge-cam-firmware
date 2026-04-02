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

    // Use raw lwIP socket with non-blocking writes.
    // Arduino WiFiClient.write() blocks forever when the TCP send buffer
    // fills, and the GDMA-corrupted lwIP can't drain it. With raw sockets
    // + MSG_DONTWAIT, write returns EAGAIN instead of blocking.

    IPAddress serverIP;
    serverIP.fromString(SERVER_HOST);

    int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        Serial.printf("[Network] socket() failed: %d\n", errno);
        return -1;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(SERVER_PORT);
    dest.sin_addr.s_addr = (uint32_t)serverIP;

    // Set connect timeout via SO_RCVTIMEO (connect uses it internally)
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    Serial.printf("[Network] Connecting TCP to %s:%d...\n", SERVER_HOST, SERVER_PORT);
    if (lwip_connect(sock, (struct sockaddr*)&dest, sizeof(dest)) != 0) {
        Serial.printf("[Network] TCP connect failed (errno %d)\n", errno);
        lwip_close(sock);
        return -1;
    }
    Serial.println("[Network] TCP connected");

    // Collect ESP stats
    int8_t rssi = WiFi.RSSI();
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t freePsram = ESP.getFreePsram();
    uint32_t uptimeMs = millis();
    esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();

    String boundary = "----FridgeCam";

    // Build metadata fields
    struct Field { const char* name; String value; };
    Field fields[] = {
        {"camera_id",    CAMERA_ID},
        {"zone",         ZONE},
        {"frame_seq",    String(frameSeq)},
        {"battery_mv",   String(batteryMV)},
        {"battery_pct",  String(batteryPct)},
        {"wifi_rssi",    String(rssi)},
        {"wifi_ssid",    WiFi.SSID()},
        {"free_heap",    String(freeHeap)},
        {"free_psram",   String(freePsram)},
        {"boot_count",   String(bootCount)},
        {"uptime_ms",    String(uptimeMs)},
        {"wake_reason",  String((int)wakeReason)},
        {"capture_ms",   String(uptimeMs)},
        {"fw_version",   "0.1.1"},
        {"frame_width",  String(fb->width)},
        {"frame_height", String(fb->height)},
        {"frame_bytes",  String(fb->len)},
    };
    int numFields = sizeof(fields) / sizeof(fields[0]);

    // Pre-build multipart body
    String meta;
    meta.reserve(2048);
    for (int i = 0; i < numFields; i++) {
        meta += "--" + boundary + "\r\n";
        meta += "Content-Disposition: form-data; name=\"" + String(fields[i].name) + "\"\r\n\r\n";
        meta += fields[i].value + "\r\n";
    }
    meta += "--" + boundary + "\r\n";
    meta += "Content-Disposition: form-data; name=\"image\"; filename=\"";
    meta += CAMERA_ID;
    meta += ".jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";

    String tail = "\r\n--" + boundary + "--\r\n";
    size_t bodyLen = meta.length() + fb->len + tail.length();

    // Build HTTP headers
    String headers;
    headers.reserve(256);
    headers += "POST ";
    headers += SERVER_PATH;
    headers += " HTTP/1.1\r\nHost: ";
    headers += SERVER_HOST;
    headers += ":";
    headers += String(SERVER_PORT);
    headers += "\r\nContent-Type: multipart/form-data; boundary=";
    headers += boundary;
    headers += "\r\nContent-Length: ";
    headers += String(bodyLen);
    headers += "\r\nConnection: close\r\n\r\n";

    // Assemble entire request in PSRAM
    size_t totalLen = headers.length() + bodyLen;
    Serial.printf("[Network] Assembling %u byte request in PSRAM...\n", totalLen);

    uint8_t* reqBuf = (uint8_t*)ps_malloc(totalLen);
    if (!reqBuf) {
        Serial.println("[Network] PSRAM alloc failed");
        lwip_close(sock);
        return -1;
    }

    size_t pos = 0;
    memcpy(reqBuf + pos, headers.c_str(), headers.length()); pos += headers.length();
    memcpy(reqBuf + pos, meta.c_str(), meta.length()); pos += meta.length();
    memcpy(reqBuf + pos, fb->buf, fb->len); pos += fb->len;
    memcpy(reqBuf + pos, tail.c_str(), tail.length()); pos += tail.length();

    Serial.printf("[Network] Sending %u bytes via raw lwIP socket...\n", totalLen);

    // Non-blocking send loop
    size_t sent = 0;
    int stallCount = 0;
    unsigned long sendStart = millis();
    while (sent < totalLen) {
        size_t toSend = totalLen - sent;
        if (toSend > 2048) toSend = 2048;

        int written = lwip_send(sock, reqBuf + sent, toSend, MSG_DONTWAIT);
        if (written > 0) {
            sent += written;
            stallCount = 0;
            if (sent % 16384 < 2048) {
                Serial.printf("[Network]   %u/%u bytes (%.0f%%)\n",
                              sent, totalLen, 100.0 * sent / totalLen);
            }
            delay(20);
            yield();
        } else if (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            stallCount++;
            if (stallCount > 150) {  // 150 * 200ms = 30s stall
                Serial.printf("[Network] Write stalled at %u/%u bytes (EAGAIN x%d, %lums)\n",
                              sent, totalLen, stallCount, millis() - sendStart);
                free(reqBuf);
                lwip_close(sock);
                return -1;
            }
            delay(200);
            yield();
        } else {
            Serial.printf("[Network] Write error at %u/%u bytes (ret=%d, errno=%d)\n",
                          sent, totalLen, written, errno);
            free(reqBuf);
            lwip_close(sock);
            return -1;
        }
    }
    free(reqBuf);
    Serial.printf("[Network] Upload sent in %lu ms\n", millis() - sendStart);

    // Read response
    char respBuf[256];
    unsigned long respStart = millis();
    int respLen = 0;
    while (millis() - respStart < UPLOAD_TIMEOUT_MS) {
        int r = lwip_recv(sock, respBuf + respLen, sizeof(respBuf) - respLen - 1, MSG_DONTWAIT);
        if (r > 0) {
            respLen += r;
            respBuf[respLen] = '\0';
            if (strstr(respBuf, "\r\n")) break;  // Got status line
        } else if (r == 0) {
            break;  // Connection closed
        }
        delay(50);
    }

    int httpCode = -1;
    if (respLen > 0) {
        respBuf[respLen] = '\0';
        // Parse "HTTP/1.1 200 OK"
        char* space = strchr(respBuf, ' ');
        if (space) {
            httpCode = atoi(space + 1);
        }
        Serial.printf("[Network] HTTP %d\n", httpCode);
    } else {
        Serial.println("[Network] No response (timeout)");
    }

    lwip_close(sock);
    return httpCode;
}

bool networkUploadWithRetry(camera_fb_t* fb, uint8_t frameSeq,
                            uint16_t batteryMV, uint8_t batteryPct) {
    for (int attempt = 0; attempt < UPLOAD_RETRIES; attempt++) {
        if (attempt > 0) {
            int backoffMs = 1000 * (1 << (attempt - 1));
            Serial.printf("[Network] Retry %d/%d in %d ms\n",
                          attempt + 1, UPLOAD_RETRIES, backoffMs);
            delay(backoffMs);
        }

        int code = networkUpload(fb, frameSeq, batteryMV, batteryPct);
        if (code == 200) return true;
    }
    Serial.println("[Network] All upload attempts failed");
    return false;
}
