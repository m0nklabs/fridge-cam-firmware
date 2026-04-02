#include "network.h"
#include <WiFi.h>

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
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
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
        return false;
    }

    Serial.printf("[Network] Connecting to %s (%d dBm)", knownAPs[bestIdx].ssid, bestRSSI);
    WiFi.begin(knownAPs[bestIdx].ssid, knownAPs[bestIdx].pass);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > WIFI_TIMEOUT_MS) {
            Serial.printf(" TIMEOUT (status=%d)\n", WiFi.status());
            return false;
        }
        delay(250);
        Serial.printf("[%d]", WiFi.status());
    }
    Serial.printf(" OK (%s, RSSI: %d dBm)\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
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

    // Open fresh TCP connection
    WiFiClient client;
    client.setNoDelay(true);

    IPAddress serverIP;
    serverIP.fromString(SERVER_HOST);

    Serial.printf("[Network] Connecting TCP to %s:%d...\n", SERVER_HOST, SERVER_PORT);
    if (!client.connect(serverIP, SERVER_PORT, 5000)) {
        Serial.printf("[Network] TCP connect failed (errno %d)\n", errno);
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

    // Pre-build multipart body prefix (metadata + image header)
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
    size_t totalLen = meta.length() + fb->len + tail.length();

    Serial.printf("[Network] Uploading %u bytes (%u img)...\n", totalLen, fb->len);

    // Send HTTP headers
    client.printf("POST %s HTTP/1.1\r\n", SERVER_PATH);
    client.printf("Host: %s:%d\r\n", SERVER_HOST, SERVER_PORT);
    client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary.c_str());
    client.printf("Content-Length: %u\r\n", totalLen);
    client.print("Connection: close\r\n\r\n");

    // Send metadata
    client.print(meta);

    // Send image in chunks with flow control
    size_t sent = 0;
    const size_t chunkSize = 1024;
    while (sent < fb->len) {
        size_t toSend = fb->len - sent;
        if (toSend > chunkSize) toSend = chunkSize;

        size_t written = client.write(fb->buf + sent, toSend);
        if (written == 0) {
            // Brief wait for TCP send buffer to drain
            delay(50);
            yield();
            written = client.write(fb->buf + sent, toSend);
        }
        if (written == 0) {
            Serial.printf("[Network] Write stalled at %u/%u bytes\n", sent, fb->len);
            client.stop();
            return -1;
        }
        sent += written;
        yield();
    }

    // Send closing boundary
    client.print(tail);
    client.flush();

    Serial.printf("[Network] Sent %u bytes, waiting for response...\n", totalLen);

    // Read response
    unsigned long respStart = millis();
    while (!client.available() && millis() - respStart < UPLOAD_TIMEOUT_MS) {
        delay(10);
    }

    int httpCode = -1;
    if (client.available()) {
        String statusLine = client.readStringUntil('\n');
        int space1 = statusLine.indexOf(' ');
        if (space1 > 0) {
            httpCode = statusLine.substring(space1 + 1, space1 + 4).toInt();
        }
        // Drain remaining response
        while (client.available()) {
            client.read();
        }
        Serial.printf("[Network] HTTP %d\n", httpCode);
    } else {
        Serial.println("[Network] No response (timeout)");
    }

    client.stop();
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
