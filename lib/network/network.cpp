#include "network.h"
#include <WiFi.h>
#include <HTTPClient.h>

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

    // Wait for DHCP/network stack to fully settle
    Serial.printf("[Network] Pre-upload: WiFi.status()=%d, IP=%s, subnet=%s, GW=%s\n",
                  WiFi.status(),
                  WiFi.localIP().toString().c_str(),
                  WiFi.subnetMask().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str());

    // Give lwIP time to finish DHCP and ARP gratuitous
    delay(1000);

    // Test raw socket to gateway
    WiFiClient testClient;
    testClient.setTimeout(5);
    IPAddress gw = WiFi.gatewayIP();
    Serial.printf("[Network] TCP test → gateway %s:80 ... ", gw.toString().c_str());
    if (testClient.connect(gw, 80, 5000)) {
        Serial.println("OK");
        testClient.stop();
    } else {
        Serial.printf("FAILED\n");
        // Try again after 2s — ARP may resolve
        delay(2000);
        Serial.printf("[Network] TCP test → gateway (retry) ... ");
        if (testClient.connect(gw, 80, 5000)) {
            Serial.println("OK (after delay)");
            testClient.stop();
        } else {
            Serial.printf("FAILED again\n");
        }
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

    // Use HTTPClient for proper connection handling (incl. ARP resolution)
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(UPLOAD_TIMEOUT_MS);

    if (!http.begin(String("http://") + SERVER_HOST + ":" + String(SERVER_PORT) + SERVER_PATH)) {
        Serial.println("[Network] HTTPClient begin failed");
        return -1;
    }

    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http.addHeader("Content-Length", String(totalLen));

    // Build complete body in PSRAM (we have 8MB, body is ~125KB)
    uint8_t* body = (uint8_t*)ps_malloc(totalLen);
    if (!body) {
        Serial.println("[Network] Failed to allocate upload buffer in PSRAM");
        http.end();
        return -1;
    }

    size_t offset = 0;
    memcpy(body + offset, meta.c_str(), meta.length());
    offset += meta.length();
    memcpy(body + offset, fb->buf, fb->len);
    offset += fb->len;
    memcpy(body + offset, tail.c_str(), tail.length());
    offset += tail.length();

    Serial.printf("[Network] Sending %u bytes via HTTPClient...\n", offset);
    int httpCode = http.sendRequest("POST", body, offset);
    free(body);

    if (httpCode > 0) {
        String response = http.getString();
        Serial.printf("[Network] HTTP %d: %s\n", httpCode, response.c_str());
    } else {
        Serial.printf("[Network] HTTP error: %s (%d)\n",
                      http.errorToString(httpCode).c_str(), httpCode);
    }

    http.end();
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
