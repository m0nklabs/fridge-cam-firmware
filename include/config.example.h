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

// Camera identity (single ceiling cam for intake scanning)
#define CAMERA_ID     "cam1"
#define ZONE          "fridge"

// LDR trigger threshold (12-bit ADC, 0-4095)
// ~620 = ~0.5V = typical light-on threshold for GL5528 + 10kΩ divider
// Set to 0 for testing without LDR (captures on every boot)
#define LDR_THRESHOLD 620

// Upload retry config
#define UPLOAD_RETRIES    3       // Number of retry attempts per file
#define UPLOAD_TIMEOUT_MS 10000   // Per-attempt timeout (ms)

// WiFi connection timeout (ms)
#define WIFI_TIMEOUT_MS   5000

// --- Capture Settings (Intake Scan Mode) ---
// Rapid continuous capture while door is open.
// Frames buffered to SD card, uploaded after door closes.

// Interval between captures (ms) — lower = more frames, more SD writes
// 500ms = 2 fps — good balance of coverage vs storage
#define CAPTURE_INTERVAL_MS  500

// Maximum frames per session (safety cap — prevents filling SD)
#define MAX_FRAMES_PER_SESSION 120  // 60 seconds at 2fps

// Maximum upload batch (files per upload phase)
// If more files exist, remaining are uploaded on next boot
#define MAX_UPLOAD_BATCH  60

// LDR re-check interval during capture (ms)
// How often to check if the fridge light turned off (door closed)
#define LDR_CHECK_INTERVAL_MS  250

// Grace period after light off before ending capture (ms)
// Prevents premature stop from hand blocking the light briefly
#define LIGHT_OFF_GRACE_MS  1500

// JPEG quality (0-63, lower = better quality, larger file)
// 10 = high quality (~50-80KB at VGA), good for label reading
#define JPEG_QUALITY      10

// Resolution
// FRAMESIZE_VGA     = 640×480   (fast SD write, ~40-60KB per frame)
// FRAMESIZE_HD      = 1280×720  (better label detail, ~100-150KB)
// FRAMESIZE_SXGA    = 1280×1024 (high detail, ~150-200KB)
// FRAMESIZE_UXGA    = 1600×1200 (max quality, ~200-300KB)
// For packaging label reading, HD is recommended.
#define CAMERA_RESOLUTION FRAMESIZE_HD

// --- Legacy Burst Settings (kept for reference) ---
#define BURST_FRAMES      3
#define BURST_INTERVAL_MS 2000
