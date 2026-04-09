#include "storage.h"
#include "pins.h"
#include <SD_MMC.h>

static bool sdReady = false;

// Freenove ESP32-S3-WROOM SDMMC pins (from official Sketch_04.1_SDMMC_Test)
#define SD_MMC_CMD  38
#define SD_MMC_CLK  39
#define SD_MMC_D0   40

bool storageInit() {
    SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);

    // 1-bit mode, format if mount fails, default freq
    if (!SD_MMC.begin("/sdcard", true, true, SDMMC_FREQ_DEFAULT, 5)) {
        Serial.println("[Storage] SD card init FAILED");
        return false;
    }

    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("[Storage] No SD card detected");
        return false;
    }

    const char* typeStr = "UNKNOWN";
    if (cardType == CARD_MMC) typeStr = "MMC";
    else if (cardType == CARD_SD) typeStr = "SD";
    else if (cardType == CARD_SDHC) typeStr = "SDHC";

    uint64_t totalMB = SD_MMC.totalBytes() / (1024 * 1024);
    uint64_t usedMB = SD_MMC.usedBytes() / (1024 * 1024);
    Serial.printf("[Storage] SD card: %s, %llu MB total, %llu MB used\n",
                  typeStr, totalMB, usedMB);

    sdReady = true;
    return true;
}

void storageDeinit() {
    if (sdReady) {
        SD_MMC.end();
        sdReady = false;
        Serial.println("[Storage] SD card released");
    }
}

String storageWriteFrame(camera_fb_t* fb, uint32_t session, uint16_t seq) {
    if (!fb || fb->len == 0) return "";
    return storageWriteBuffer(fb->buf, fb->len, session, seq);
}

String storageWriteBuffer(const uint8_t* buf, size_t len, uint32_t session, uint16_t seq) {
    if (!sdReady || !buf || len == 0) return "";

    char filename[32];
    snprintf(filename, sizeof(filename), "/fc_%06lu_%03u.jpg", session, seq);

    unsigned long writeStart = millis();
    File file = SD_MMC.open(filename, FILE_WRITE);
    if (!file) {
        Serial.printf("[Storage] Failed to open %s for writing\n", filename);
        return "";
    }

    size_t written = file.write(buf, len);
    file.close();

    unsigned long elapsed = millis() - writeStart;

    if (written != len) {
        Serial.printf("[Storage] Write incomplete: %u/%u bytes to %s\n",
                      written, len, filename);
        SD_MMC.remove(filename);
        return "";
    }

    Serial.printf("[Storage] Wrote %s: %u bytes in %lu ms\n",
                  filename, len, elapsed);
    return String(filename);
}

int storageListPending(String* filenames, int maxFiles) {
    if (!sdReady) return 0;

    File root = SD_MMC.open("/");
    if (!root || !root.isDirectory()) return 0;

    int count = 0;
    File entry;
    while ((entry = root.openNextFile()) && count < maxFiles) {
        String name = String("/") + entry.name();
        if (name.startsWith("/fc_") && name.endsWith(".jpg")) {
            filenames[count++] = name;
        }
        entry.close();
    }
    root.close();

    // Simple sort by filename (session + sequence gives natural order)
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (filenames[j] < filenames[i]) {
                String tmp = filenames[i];
                filenames[i] = filenames[j];
                filenames[j] = tmp;
            }
        }
    }

    Serial.printf("[Storage] Found %d pending frames\n", count);
    return count;
}

uint8_t* storageReadFile(const String& filename, size_t* outLen) {
    if (!sdReady || !outLen) return nullptr;
    *outLen = 0;

    File file = SD_MMC.open(filename, FILE_READ);
    if (!file) {
        Serial.printf("[Storage] Failed to open %s for reading\n", filename.c_str());
        return nullptr;
    }

    size_t fileSize = file.size();
    if (fileSize == 0) {
        file.close();
        return nullptr;
    }

    uint8_t* buf = (uint8_t*)ps_malloc(fileSize);
    if (!buf) {
        Serial.printf("[Storage] PSRAM alloc failed for %u bytes\n", fileSize);
        file.close();
        return nullptr;
    }

    size_t bytesRead = file.read(buf, fileSize);
    file.close();

    if (bytesRead != fileSize) {
        Serial.printf("[Storage] Read incomplete: %u/%u bytes from %s\n",
                      bytesRead, fileSize, filename.c_str());
        free(buf);
        return nullptr;
    }

    *outLen = fileSize;
    return buf;
}

bool storageDeleteFile(const String& filename) {
    if (!sdReady) return false;

    if (SD_MMC.remove(filename.c_str())) {
        Serial.printf("[Storage] Deleted %s\n", filename.c_str());
        return true;
    }
    Serial.printf("[Storage] Failed to delete %s\n", filename.c_str());
    return false;
}

int storagePendingCount() {
    if (!sdReady) return 0;

    File root = SD_MMC.open("/");
    if (!root || !root.isDirectory()) return 0;

    int count = 0;
    File entry;
    while ((entry = root.openNextFile())) {
        String name = String("/") + entry.name();
        if (name.startsWith("/fc_") && name.endsWith(".jpg")) {
            count++;
        }
        entry.close();
    }
    root.close();
    return count;
}

void storageGetInfo(uint64_t* totalBytes, uint64_t* usedBytes) {
    if (!sdReady) {
        *totalBytes = 0;
        *usedBytes = 0;
        return;
    }
    *totalBytes = SD_MMC.totalBytes();
    *usedBytes = SD_MMC.usedBytes();
}
