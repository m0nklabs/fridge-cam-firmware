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

    // Scan for networks
    Serial.println("[Network] Scanning...");
    int n = WiFi.scanNetworks();

    // Find strongest known AP
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
            Serial.println(" TIMEOUT");
            return false;
        }
        delay(100);
        Serial.print(".");
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

static String getTimestamp() {
    // Simple timestamp from millis since boot (no NTP for battery life)
    // Server can use receive time as authoritative timestamp
    unsigned long ms = millis();
    char buf[32];
    snprintf(buf, sizeof(buf), "boot+%lu ms", ms);
    return String(buf);
}

int networkUpload(camera_fb_t* fb, uint8_t frameSeq,
                  uint16_t batteryMV, uint8_t batteryPct) {
    if (!fb || fb->len == 0) {
        Serial.println("[Network] No frame to upload");
        return -1;
    }

    // Collect ESP stats
    int8_t rssi = WiFi.RSSI();
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t freePsram = ESP.getFreePsram();
    uint32_t uptimeMs = millis();
    esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();

    String boundary = "----FridgeCam";

    // Build metadata fields as small strings
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

    // Pre-build metadata + image header string (small, ~2KB)
    String meta;
    meta.reserve(2048);
    for (int i = 0; i < numFields; i++) {
        meta += "--" + boundary + "\r\n";
        meta += "Content-Disposition: form-data; name=\"" + String(fields[i].name) + "\"\r\n\r\n";
        meta += fields[i].value + "\r\n";
    }
    // Image part header
    meta += "--" + boundary + "\r\n";
    meta += "Content-Disposition: form-data; name=\"image\"; filename=\"";
    meta += CAMERA_ID;
    meta += ".jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";

    String tail = "\r\n--" + boundary + "--\r\n";
    size_t totalLen = meta.length() + fb->len + tail.length();

    Serial.printf("[Network] Uploading %u bytes (%u img) to %s\n",
                  totalLen, fb->len, SERVER_URL);

    // Pre-connect diagnostics
    Serial.printf("[Network] WiFi status: %d, IP: %s, GW: %s, DNS: %s\n",
                  WiFi.status(),
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  WiFi.dnsIP().toString().c_str());

    // Use raw WiFiClient for streaming upload (no malloc of full body)
    WiFiClient client;
    client.setTimeout(10);  // 10 second connect+write timeout

    // Use IPAddress directly to bypass hostByName() which can fail silently
    IPAddress serverIP;
    serverIP.fromString(SERVER_HOST);
    Serial.printf("[Network] Connecting to %s:%d (resolved: %s)...\n",
                  SERVER_HOST, SERVER_PORT, serverIP.toString().c_str());
    if (!client.connect(serverIP, SERVER_PORT)) {
        Serial.printf("[Network] Connection failed (WiFi status: %d, errno: %d)\n",
                      WiFi.status(), errno);
        return -1;
    }
    Serial.println("[Network] Connected OK");

    // Send HTTP request line + headers
    client.printf("POST %s HTTP/1.1\r\n", SERVER_PATH);
    client.printf("Host: %s:%d\r\n", SERVER_HOST, SERVER_PORT);
    client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary.c_str());
    client.printf("Content-Length: %u\r\n", totalLen);
    client.printf("Connection: close\r\n\r\n");

    // Stream metadata
    client.print(meta);

    // Stream JPEG in chunks (4KB at a time to avoid memory issues)
    size_t sent = 0;
    const size_t chunkSize = 4096;
    while (sent < fb->len) {
        size_t toSend = fb->len - sent;
        if (toSend > chunkSize) toSend = chunkSize;
        size_t written = client.write(fb->buf + sent, toSend);
        if (written == 0) {
            Serial.println("[Network] Write failed mid-upload");
            client.stop();
            return -1;
        }
        sent += written;
    }

    // Send closing boundary
    client.print(tail);
    client.flush();

    // Read response
    unsigned long t0 = millis();
    while (!client.available() && (millis() - t0 < UPLOAD_TIMEOUT_MS)) {
        delay(10);
    }

    int httpCode = -1;
    if (client.available()) {
        String statusLine = client.readStringUntil('\n');
        // Parse "HTTP/1.1 200 OK"
        int spaceIdx = statusLine.indexOf(' ');
        if (spaceIdx > 0) {
            httpCode = statusLine.substring(spaceIdx + 1, spaceIdx + 4).toInt();
        }
        // Read body (skip headers)
        String body;
        bool headersEnded = false;
        while (client.available()) {
            String line = client.readStringUntil('\n');
            if (!headersEnded && line.length() <= 2) {
                headersEnded = true;
                continue;
            }
            if (headersEnded) body += line;
        }
        Serial.printf("[Network] HTTP %d: %s\n", httpCode, body.c_str());
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
            int backoffMs = 1000 * (1 << (attempt - 1));  // 1s, 2s, 4s
            Serial.printf("[Network] Retry %d/%d in %d ms\n",
                          attempt + 1, UPLOAD_RETRIES, backoffMs);
            delay(backoffMs);
        }

        int code = networkUpload(fb, frameSeq, batteryMV, batteryPct);
        if (code == 200) {
            return true;
        }
    }
    Serial.println("[Network] All upload attempts failed");
    return false;
}
