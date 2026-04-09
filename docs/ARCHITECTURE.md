# Firmware Architecture

How the ESP32 firmware works — state machine, capture pipeline, SD buffering, upload protocol, and power management.

## Design: Intake Scan

Single ceiling-mounted camera at the front of the fridge, looking down at the opening. Captures packaging labels as items go in/out — one product at a time, clean shots, well-lit (door is open).

**Why intake over inventory?**
- Cleaner captures: single item in frame vs cluttered shelves
- Labels face up/forward when placing items
- Vision LLM gets high-quality label shots
- Simpler hardware: 1 camera instead of 3
- IN vs OUT inferrable from frame sequence + context

## State Machine

```
┌───────────────────────────────────────────────────────────────┐
│                                                               │
│  ┌──────────┐     LDR interrupt      ┌────────────────────┐  │
│  │          │ ──────────────────────▶ │                    │  │
│  │  DEEP    │                        │  BOOT + INIT       │  │
│  │  SLEEP   │ ◀───── all done ─────  │  (~500ms)          │  │
│  │  ~10µA   │                        │  - Read battery    │  │
│  │          │                        │  - LDR debounce    │  │
│  └──────────┘                        └────────┬───────────┘  │
│       ▲                                       │              │
│       │                                       ▼              │
│       │              ┌────────────────────────────────────┐  │
│       │              │                                    │  │
│       │              │  CAPTURE PHASE                     │  │
│       │              │  (camera + SD active, WiFi off)    │  │
│       │              │                                    │  │
│       │              │  while (light on):                 │  │
│       │              │    capture frame → write to SD     │  │
│       │              │    ~500ms interval (2 fps)         │  │
│       │              │    max 120 frames safety cap       │  │
│       │              │                                    │  │
│       │              │  light off + grace period → stop   │  │
│       │              │  deinit camera (free GDMA)         │  │
│       │              │                                    │  │
│       │              └──────────────┬─────────────────────┘  │
│       │                             │                        │
│       │                             ▼                        │
│       │              ┌────────────────────────────────────┐  │
│       │              │                                    │  │
│       │              │  UPLOAD PHASE                      │  │
│       │              │  (WiFi active, camera off)         │  │
│       │              │                                    │  │
│       │              │  rebuild WiFi stack (clean DMA)    │  │
│       │              │  for each file on SD:              │  │
│       │              │    read → UDP blast → delete       │  │
│       │              │  max 60 files per batch            │  │
│       │              │                                    │  │
│       │              │  if light came back on → restart   │  │
│       │              │                                    │  │
│       └──────────────┴────────────────────────────────────┘  │
│                                                               │
│  BONUS: On false trigger (light off at boot), check for      │
│  orphaned SD files from interrupted previous sessions         │
│  and upload those before sleeping.                            │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

## Boot Sequence

1. **Wake from deep sleep** — ext0 wakeup on LDR GPIO (rising edge)
2. **Read battery voltage** (~10ms)
3. **LDR re-check** — confirm light is still on
   - If dark → check SD for orphaned files → upload if any → sleep
4. **Enter capture phase**

## Capture Phase

Camera and SD card are active. WiFi is OFF (GDMA conflict).

```
camera init → SD init → capture loop → camera deinit
```

### Continuous Capture Loop

```cpp
while (frameCount < MAX_FRAMES && lightIsOn()) {
    frame = captureJPEG();           // ~50ms at HD
    storageWriteFrame(frame, sd);    // ~30ms SPI write
    releaseFrame(frame);
    wait(CAPTURE_INTERVAL_MS);       // 500ms default
}
```

**Timing per frame:**
- Camera capture: ~50ms (HD JPEG, PSRAM framebuffer)
- SD SPI write: ~30-50ms (HD frame ~100-150KB at ~3MB/s)
- Overhead: ~10ms
- **Total: ~110ms per frame** — well within 500ms interval

### Light-Off Detection

LDR is checked every 250ms during the capture loop. When light goes off:
1. Start grace timer (1.5s default)
2. If light comes back within grace → ignore (hand blocked sensor)
3. If light stays off → end capture phase

### Frame Budget

At HD (1280×720), JPEG quality 10:
- ~100-150KB per frame
- At 2 fps over 30s door open = 60 frames = ~6-9MB
- Typical 8GB SD card = hundreds of sessions before full
- Safety cap: 120 frames max per session

## SD Card Buffering

**Why SD instead of upload-during-capture?**
The ESP32-S3 has a known GDMA conflict between camera and WiFi DMA.
Camera and WiFi cannot be active simultaneously. SD card uses SPI (no DMA conflict).

**File layout:**
```
/fc_000042_001.jpg    ← session 42, frame 1
/fc_000042_002.jpg    ← session 42, frame 2
...
```

**Persistence benefit:** If power is lost during upload or WiFi fails,
frames survive on SD and are uploaded on the next boot (orphan recovery).

## Upload Phase

Camera is deinitialized. WiFi stack is rebuilt from scratch (clean DMA state).

```
camera deinit → WiFi rebuild → connect → upload files → disconnect
```

### Upload Protocol (unchanged — UDP Blast)

Per file: read from SD → UDP blast with 3 redundancy rounds.
See `lib/network/` for the chunked UDP protocol with CRC16.

### Batch Limit

Max 60 files per upload phase. If more exist (from multiple sessions),
remaining files are uploaded on the next boot. This prevents
excessively long upload phases that drain battery.

### Post-Upload Light Check

After upload completes, LDR is checked one more time.
If light came back on (door reopened during upload) → restart the cycle.

## Power Budget

| Phase | Duration | Current | Energy |
|-------|----------|---------|--------|
| Deep sleep | ~hours | ~10µA | negligible |
| Boot + init | ~500ms | ~240mA | 0.033 mAh |
| Capture (30s) | ~30s | ~280mA | 2.33 mAh |
| SD write (incl.) | (overlaps capture) | +20mA | (included) |
| Upload (60 frames) | ~20-40s | ~240mA | 2.0 mAh |
| **Total per event** | **~60s active** | | **~4.4 mAh** |

With 6000mAh battery and ~10 door opens/day:
- Daily: 10 × 4.4 mAh + deep sleep ~2.4 mAh = **~46.4 mAh/day**
- Battery life: 6000 / 46.4 = **~129 days** (~4.3 months)

More active sessions use more power. For 20 opens/day: ~64 days.
Still very practical for USB-C rechargeable setup.

## GDMA Workaround (unchanged)

ESP32-S3 camera uses LCD_CAM peripheral GDMA which corrupts WiFi TX DMA buffers.
The two-phase architecture (capture→upload) avoids this entirely:

1. Camera phase: camera GDMA active, no WiFi
2. `captureDeinit()` → `periph_module_disable(PERIPH_LCD_CAM_MODULE)` + GPIO reset
3. WiFi phase: full WiFi stack rebuild on clean DMA state

This is actually cleaner than the old single-shot design because there's
never a moment where both peripherals compete for DMA resources.
| `wifi_pass` | string | WiFi password |
| `server_url` | string | Upload endpoint URL |
| `camera_id` | string | This unit's ID (cam1/cam2/cam3) |
| `zone` | string | fridge or freezer |
| `ldr_threshold` | uint16 | ADC threshold for light detection |
| `boot_count` | uint32 | Total boot count (diagnostic) |
| `last_upload_ok` | bool | Whether last upload succeeded |

## OTA Updates (Future)

Planned for v2: Over-the-air firmware updates via WiFi. The ESP32 checks a version endpoint on the server during boot and pulls a new binary if available. Not implemented in v1 — updates are done via USB.
