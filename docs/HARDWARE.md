# Hardware Reference

Complete wiring, pinout, and BOM for the Smart Fridge Cam.

## Board: ESP32-S3-N16R8

- **SoC**: ESP32-S3 dual-core Xtensa LX7 @ 240MHz
- **Flash**: 16MB
- **PSRAM**: 8MB octal (required for 5MP framebuffer)
- **WiFi**: 802.11 b/g/n
- **BLE**: 5.0
- **GPIO**: 45 pins total
- **Deep sleep**: ~10-15µA

### Prototype Board
**Freenove ESP32-S3-WROOM** (full dev kit with OV2640, microSD, USB stick with tutorials)
AliExpress: [Freenove Official Store](https://nl.aliexpress.com/item/1005004960637276.html) — €18.89

### Production Board
**Advanced Tech ESP32-S3 N16R8** (factory OV5640 option, bare board)
AliExpress: [Advanced Tech Store](https://nl.aliexpress.com/item/1005007307965550.html) — ~€8/unit

## Camera: OV5640

- **Sensor**: OV5640 — 5MP, autofocus
- **Lens**: 160° wide-angle
- **Ribbon**: 21mm, 24-pin DVP, 0.5mm pitch
- **Interface**: DVP parallel (not MIPI)
- **Resolution**: Up to 2592×1944 (5MP), typically used at 1600×1200 or 1280×720
- **JPEG encoder**: Hardware on-chip — ESP32 receives compressed JPEG directly

Ordered from [Camera Module Factory Store](https://nl.aliexpress.com/item/1005004530278821.html) — €14.29 (160° variant, 21mm ribbon)

## GPIO Pin Assignment

### Camera (DVP) — Fixed by board design

| Pin | GPIO | Function |
|-----|------|----------|
| XCLK | GPIO15 | Camera clock |
| SIOD | GPIO4 | I2C data (SCCB) |
| SIOC | GPIO5 | I2C clock (SCCB) |
| D0-D7 | GPIO11,9,8,10,12,18,17,16 | Parallel data |
| VSYNC | GPIO6 | Vertical sync |
| HREF | GPIO7 | Horizontal ref |
| PCLK | GPIO13 | Pixel clock |
| PWDN | GPIO-1 | Power down (not used) |
| RESET | GPIO-1 | Reset (not used) |

> Note: Exact DVP pin mapping depends on the board variant. Verify against your board's schematic. The Freenove board uses slightly different pins — check `camera_pins.h` in their examples.

### Custom Wiring

| Function | GPIO | Notes |
|----------|------|-------|
| **LDR trigger** | GPIO1 | Analog read (ADC1_CH0). GL5528 + 10kΩ voltage divider. Also wired as ext0 wakeup source. |
| **Battery voltage** | GPIO2 | Analog read (ADC1_CH1). Voltage divider: 100kΩ + 100kΩ → reads 0-2.1V representing 0-4.2V cell voltage |
| **Status LED** | GPIO48 | Onboard RGB LED (Freenove) or wired LED. Brief flash on capture. |

### Wiring Diagram

```
                    ESP32-S3-CAM
                   ┌──────────────┐
                   │              │
  3.3V ──────┬─────┤ 3V3          │
             │     │              │
         ┌───┴───┐ │              │
         │ 10kΩ  │ │              │
         └───┬───┘ │              │
  LDR ───────┤     │              │
  GL5528     ├─────┤ GPIO1 (ADC)  │
             │     │              │
  GND ───────┘     │              │
                   │              │
  VBAT ──┬─────────┤              │
         │  100kΩ  │              │
         ├─────────┤ GPIO2 (ADC)  │
         │  100kΩ  │              │
  GND ───┘         │              │
                   │              │
  USB-C ───────────┤ 5V (from UPS)│
                   │              │
                   └──────────────┘
```

### LDR Behaviour

| Condition | LDR Resistance | Voltage at GPIO1 | Meaning |
|-----------|---------------|-------------------|---------|
| Door closed (dark) | ~1MΩ | ~0.03V | Sleep — no trigger |
| Door open (light on) | ~10-20kΩ | ~1.1-1.65V | **Wake trigger** |

Threshold: ~0.5V (ADC raw value ~620 on 12-bit). Configurable in `config.h`.

### Battery Monitoring

| Battery Voltage | ADC Reads | Charge | Action |
|-----------------|-----------|--------|--------|
| ≥4.1V | ~2.05V | >90% | Full |
| 3.7-4.1V | 1.85-2.05V | 50-90% | Good |
| 3.5-3.7V | 1.75-1.85V | 20-50% | Report "low" |
| 3.3-3.5V | 1.65-1.75V | 5-20% | Report "critical" |
| <3.3V | <1.65V | <5% | BMS cutoff |

Voltage divider ratio: 2:1 (100kΩ + 100kΩ). ESP32 ADC reads half the actual cell voltage.

## Bill of Materials (per unit)

| Component | Source | Price |
|-----------|--------|-------|
| ESP32-S3-N16R8 CAM (factory OV5640) | [Advanced Tech](https://nl.aliexpress.com/item/1005007307965550.html) | ~€8 |
| OV5640 160° module (proto, separate) | [Camera Module Factory](https://nl.aliexpress.com/item/1005004530278821.html) | ~€14 |
| Type-C 15W 3A 18650 UPS module | [KaiHang Store](https://nl.aliexpress.com/item/1005009864874228.html) | ~€2.50 |
| 2× 18650 Li-ion 3000mAh | on hand | ~€6 |
| GL5528 LDR photoresistor | [IRUWB Store](https://nl.aliexpress.com/item/1005007822412621.html) | ~€0.10 |
| 10kΩ resistor (LDR pull-down) | on hand | ~€0 |
| 100kΩ resistors ×2 (battery divider) | on hand | ~€0 |
| DIN 71803 C8-M5 ball stud | [SOLIDfy (Amazon)](https://www.amazon.nl/dp/B07K2LLFYP) | ~€3 |
| 0.7mm 304 SS torsion spring | [YILin4619 Store](https://nl.aliexpress.com/item/1005006856683143.html) | ~€0.70 |
| M2×30mm DIN912 screw | [SCREWHOME Store](https://nl.aliexpress.com/item/1005005879037174.html) | ~€0.10 |
| M2 DIN985 nylon locknut | [Canrich Store](https://nl.aliexpress.com/item/1005008314277777.html) | ~€0.05 |
| 3D-printed PETG (clip + enclosures) | self-printed | ~€3 |
| Dupont wires + heat shrink | on hand | ~€0 |
| **Total per unit** | | **~€22** |

## Assembly Notes

1. **LDR wiring**: Solder GL5528 between 3.3V and GPIO1. Solder 10kΩ between GPIO1 and GND. This creates a voltage divider — bright light pulls GPIO1 high.
2. **Battery divider**: Solder 100kΩ from VBAT to GPIO2, and 100kΩ from GPIO2 to GND. This halves the battery voltage for safe ADC reading.
3. **Camera ribbon**: Handle the 24-pin DVP ribbon carefully — the connector locks are fragile. Insert with contacts facing the board, lock the clip.
4. **USB-C power**: The UPS module's USB-C output goes directly to the ESP32 board's USB-C/5V input. Power only — no data.
5. **Enclosure**: LDR pokes through a ~3mm hole in the front. Camera lens aligns with a window. USB-C port accessible from the side/bottom.
