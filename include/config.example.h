#pragma once

// ============================================================
// Smart Fridge Cam — Configuration
// ============================================================
// Copy this file to config.h and fill in your values.
// config.h is gitignored — never commit credentials.
// ============================================================

// WiFi credentials (multiple APs — firmware picks strongest)
#define WIFI_AP_COUNT 2

// AP 1
#define WIFI_SSID_1     "your-ssid"
#define WIFI_PASSWORD_1 "your-password"

// AP 2 (optional — set to "" to disable)
#define WIFI_SSID_2     ""
#define WIFI_PASSWORD_2 ""

// Server endpoint (HungryFoodTool backend)
#define SERVER_HOST   "192.168.1.35"
#define SERVER_PORT   8790
#define SERVER_PATH   "/api/fridge/scan"
#define SERVER_URL    "http://" SERVER_HOST ":8790" SERVER_PATH
#define UDP_PORT      8791

// Camera identity
#define CAMERA_ID     "cam1"      // "cam1", "cam2", or "cam3"
#define ZONE          "fridge"    // "fridge" or "freezer"

// Camera mode
// BURST = capture 3 frames, upload sharpest (CAM 1 + CAM 2)
// STREAM = capture every 2s for 30s, upload all (CAM 3)
#define CAPTURE_MODE  BURST       // BURST or STREAM

// LDR trigger threshold (12-bit ADC, 0-4095)
// ~620 = ~0.5V = typical light-on threshold for GL5528 + 10kΩ divider
#define LDR_THRESHOLD 620

// Upload retry config
#define UPLOAD_RETRIES    3       // Number of retry attempts
#define UPLOAD_TIMEOUT_MS 10000   // Per-attempt timeout (ms)

// WiFi connection timeout (ms)
#define WIFI_TIMEOUT_MS   5000

// Burst mode config
#define BURST_FRAMES      3       // Number of frames to capture
#define BURST_INTERVAL_MS 2000    // Delay between frames (ms)

// Stream mode config (CAM 3 only)
#define STREAM_DURATION_S 30      // Total stream time (seconds)
#define STREAM_INTERVAL_MS 2000   // Delay between frames (ms)

// JPEG quality (0-63, lower = better quality, larger file)
#define JPEG_QUALITY      10

// Resolution
// FRAMESIZE_VGA     = 640×480   (fast upload, fewer chunks — best for UDP blast)
// FRAMESIZE_HD      = 1280×720  (good balance of quality/speed)
// FRAMESIZE_SXGA    = 1280×1024
// FRAMESIZE_UXGA    = 1600×1200 (max for reliable JPEG on 8MB PSRAM)
// FRAMESIZE_QSXGA   = 2560×1920 (5MP full res — may be slow)
#define CAMERA_RESOLUTION FRAMESIZE_VGA
