# Firmware Architecture

How the ESP32 firmware works — state machine, capture pipeline, upload protocol, and power management.

## State Machine

```
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  ┌──────────┐     LDR interrupt      ┌──────────────────┐  │
│  │          │ ──────────────────────▶ │                  │  │
│  │  DEEP    │                        │  BOOT + INIT     │  │
│  │  SLEEP   │ ◀────────────────────  │  (~200ms)        │  │
│  │  ~10µA   │     upload done /      │  - WiFi connect  │  │
│  │          │     timeout / error    │  - Camera init   │  │
│  └──────────┘                        │  - Read battery  │  │
│                                      └────────┬─────────┘  │
│                                               │             │
│                                               ▼             │
│                           ┌──────────────────────────────┐  │
│                           │                              │  │
│                           │  CAPTURE                     │  │
│                           │                              │  │
│                           │  Burst mode (CAM 1/2):       │  │
│                           │    3 frames @ t=1s,3s,5s     │  │
│                           │    Select sharpest            │  │
│                           │                              │  │
│                           │  Stream mode (CAM 3):        │  │
│                           │    1 frame/2s for 30s        │  │
│                           │    Upload each immediately   │  │
│                           │                              │  │
│                           └──────────────┬───────────────┘  │
│                                          │                  │
│                                          ▼                  │
│                           ┌──────────────────────────────┐  │
│                           │                              │  │
│                           │  UPLOAD                      │  │
│                           │                              │  │
│                           │  HTTP POST multipart/form    │  │
│                           │  to /api/fridge/scan         │  │
│                           │  Retry 3× with backoff       │  │
│                           │  Timeout: 10s per attempt    │  │
│                           │                              │  │
│                           └──────────────────────────────┘  │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

## Boot Sequence

1. **Wake from deep sleep** — ext0 wakeup on LDR GPIO (rising edge)
2. **Initialise peripherals** (~50ms):
   - Camera: configure OV5640 (JPEG mode, 1280×720 or 1600×1200)
   - ADC: read battery voltage
   - GPIO: set up LDR pin, status LED
3. **Connect WiFi** (~100-500ms):
   - Use saved credentials from NVS
   - Static IP preferred (faster than DHCP)
   - Timeout: 5 seconds → abort and sleep if no connection
4. **LDR re-check** — confirm light is still on (debounce false triggers)
   - If dark again → back to sleep immediately (door was opened briefly)
5. **Enter capture mode**

## Capture Logic

### Burst Mode (CAM 1 + CAM 2)

```cpp
// Capture 3 frames with 2-second intervals
for (int i = 0; i < 3; i++) {
    frames[i] = capture_jpeg();
    sharpness[i] = laplacian_variance(frames[i]);
    delay(2000);
}
// Select frame with highest sharpness score
best = argmax(sharpness);
upload(frames[best]);
```

**Why 3 frames?**
- Frame 1: Door just opened, things may be moving
- Frame 2: Stable view, items settled
- Frame 3: User may have grabbed/placed items
- Sharpest = least motion blur = best for vision LLM

### Stream Mode (CAM 3)

```cpp
// Continuous capture for 30 seconds
for (int seq = 0; seq < 15; seq++) {
    frame = capture_jpeg();
    upload(frame, seq);   // Upload immediately
    delay(2000);
}
```

**Why stream?** The freezer has drawers that open sequentially over 10-20 seconds. Continuous capture catches each drawer as it's pulled out.

### Frame Selection (Burst Mode)

Sharpness via Laplacian variance — done on the ESP32 before upload:

```cpp
float laplacian_variance(camera_fb_t* fb) {
    // Decode JPEG to grayscale (or use Y channel)
    // Apply 3×3 Laplacian kernel
    // Return variance of result — higher = sharper
}
```

This avoids uploading blurry frames and saves bandwidth + server processing.

## Upload Protocol

### HTTP Request

```
POST /api/fridge/scan HTTP/1.1
Host: 192.168.1.35:8790
Content-Type: multipart/form-data; boundary=----FridgeCam

------FridgeCam
Content-Disposition: form-data; name="image"; filename="cam1_20260402T143022.jpg"
Content-Type: image/jpeg

<JPEG binary data>
------FridgeCam
Content-Disposition: form-data; name="camera_id"

cam1
------FridgeCam
Content-Disposition: form-data; name="zone"

fridge
------FridgeCam
Content-Disposition: form-data; name="battery_mv"

3850
------FridgeCam
Content-Disposition: form-data; name="battery_pct"

72
------FridgeCam
Content-Disposition: form-data; name="timestamp"

2026-04-02T14:30:22Z
------FridgeCam--
```

### Server Response

```json
{
  "status": "ok",
  "scan_id": "abc123",
  "items_detected": 12
}
```

### Retry Logic

- 3 attempts with exponential backoff (1s, 2s, 4s)
- Timeout per attempt: 10 seconds
- If all 3 fail → log error, go to sleep (try again next door open)
- Never block indefinitely — battery life is sacred

## Power Management

### Deep Sleep Configuration

```cpp
esp_sleep_enable_ext0_wakeup(LDR_GPIO, 1);  // Wake on HIGH (light detected)
esp_deep_sleep_start();
```

### Power Budget

| Phase | Duration | Current | Energy |
|-------|----------|---------|--------|
| Deep sleep | ~hours | ~10µA | negligible |
| Boot + WiFi | ~500ms | ~240mA | 0.033 mAh |
| Capture (burst) | ~5s | ~240mA | 0.33 mAh |
| Upload | ~1-2s | ~240mA | 0.13 mAh |
| **Total per event** | **~6s active** | | **~0.4 mAh** |

With 6000mAh battery (2× 18650 parallel) and ~20 door opens/day:
- Daily: 20 × 0.4 mAh = 8 mAh + deep sleep ~2.4 mAh = **~10.4 mAh/day**
- Battery life: 6000 / 10.4 = **~577 days** (conservative estimate)

### Battery Reporting

Every upload includes `battery_mv` and `battery_pct`. The server stores this per camera and triggers warnings in the HFT frontend when batteries run low.

## NVS Storage

Non-volatile storage for persistent config:

| Key | Type | Purpose |
|-----|------|--------|
| `wifi_ssid` | string | WiFi network name |
| `wifi_pass` | string | WiFi password |
| `server_url` | string | Upload endpoint URL |
| `camera_id` | string | This unit's ID (cam1/cam2/cam3) |
| `zone` | string | fridge or freezer |
| `ldr_threshold` | uint16 | ADC threshold for light detection |
| `boot_count` | uint32 | Total boot count (diagnostic) |
| `last_upload_ok` | bool | Whether last upload succeeded |

## OTA Updates (Future)

Planned for v2: Over-the-air firmware updates via WiFi. The ESP32 checks a version endpoint on the server during boot and pulls a new binary if available. Not implemented in v1 — updates are done via USB.
