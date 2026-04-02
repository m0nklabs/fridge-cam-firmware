# Fridge Cam Firmware

ESP32-S3 firmware for the Smart Fridge Cam — a DIY camera system that photographs fridge contents and uploads them for AI-powered inventory tracking.

Part of [HungryFoodTool](https://github.com/m0nklabs/hungryfoodtool). See the full design doc: [SMART-FRIDGE-CAM.md](https://github.com/m0nklabs/hungryfoodtool/blob/main/docs/SMART-FRIDGE-CAM.md).

## What It Does

1. ESP32-S3 sleeps in deep sleep (~10µA)
2. Fridge door opens → fridge light turns on → LDR triggers GPIO interrupt
3. Wake up, capture burst (3 frames over 4 seconds)
4. Select sharpest frame (Laplacian variance)
5. HTTP POST JPEG + metadata to server over WiFi
6. Back to deep sleep

Zero inference on the ESP32. All AI processing happens on the server.

## Hardware

| Component | Spec |
|-----------|------|
| **Board** | ESP32-S3-N16R8 (16MB flash, 8MB PSRAM) |
| **Camera** | OV5640 5MP autofocus, 160° wide-angle, 24-pin DVP |
| **Trigger** | GL5528 LDR photoresistor + 10kΩ pull-down |
| **Power** | 2× 18650 via Type-C UPS module (separate enclosure) |
| **Mount** | 3D-printed PETG clip + DIN 71803 ball joint |

See [docs/HARDWARE.md](docs/HARDWARE.md) for wiring, pinout, and BOM.

## Quick Start

### Prerequisites
- PlatformIO Core (CLI) or PlatformIO IDE
- ESP32-S3 board connected via USB
- Python 3.x (for esptool)

### Build & Flash (on Raspberry Pi or any Linux box)

```bash
git clone https://github.com/m0nklabs/fridge-cam-firmware.git
cd fridge-cam-firmware
cp include/config.example.h include/config.h
# Edit config.h with your WiFi credentials and server IP

pio run --target upload
pio device monitor            # Serial debug output
```

### Configuration

Copy `include/config.example.h` to `include/config.h` and set:

```cpp
#define WIFI_SSID     "your-ssid"
#define WIFI_PASSWORD "your-password"
#define SERVER_URL    "http://192.168.1.35:8790/api/fridge/scan"
#define CAMERA_ID     "cam1"     // cam1, cam2, or cam3
#define ZONE          "fridge"   // fridge or freezer
```

### Cross-Compile (build on server, flash via Pi)

```bash
# On ai-kvm2 (fast compile)
pio run
scp .pio/build/esp32s3/firmware.bin flip@<pi-ip>:~/

# On Pi (just flash)
esptool.py --port /dev/ttyUSB0 write_flash 0x0 firmware.bin
```

## Project Structure

```
fridge-cam-firmware/
├── src/
│   └── main.cpp              # Entry point: setup + deep sleep loop
├── lib/
│   ├── capture/              # Camera init, burst capture, frame selection
│   ├── network/              # WiFi connect, HTTP upload, retry logic
│   ├── power/                # Battery ADC reading, deep sleep management
│   └── trigger/              # LDR light detection, GPIO interrupt config
├── include/
│   ├── config.example.h      # Template config (WiFi, server URL, camera ID)
│   └── pins.h                # GPIO pin assignments
├── docs/
│   ├── HARDWARE.md           # Wiring, pinout, BOM, assembly
│   └── ARCHITECTURE.md       # Firmware architecture, state machine, protocols
├── platformio.ini            # PlatformIO build config
└── README.md
```

## Camera Modes

| Camera | Mode | Behaviour |
|--------|------|-----------|
| CAM 1 (ceiling) | Burst | 3 frames over 4s, upload best |
| CAM 2 (door mid) | Burst | 3 frames over 4s, upload best |
| CAM 3 (door bottom) | Stream | 1 frame/2s for 30s, upload all |

## Upload Protocol

```
POST /api/fridge/scan
Content-Type: multipart/form-data

Fields:
  image        - JPEG binary
  camera_id    - "cam1" | "cam2" | "cam3"
  zone         - "fridge" | "freezer"
  frame_seq    - Frame sequence number (stream mode only)
  battery_mv   - Battery voltage in millivolts
  battery_pct  - Estimated battery percentage
  timestamp    - ISO 8601 capture time
```

## License

[MIT](LICENSE) — m0nklabs 2026
