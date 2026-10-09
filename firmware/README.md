# DART Announce — Dual-Target C++ Embedded Architecture

This directory houses the C++ embedded software for the **DART Kitchen Commute Display**.

It uses a dual-target architecture with **100% shared core logic**:
* **`firmware/core/`**: Shared cross-platform C++ engine:
  - `dart_types.hpp`: Core data structures (`Departure`, `BoardData`).
  - `dart_parser.hpp`: Lightweight XML parser extracting live Irish Rail departures without external dependencies.
  - `dart_canvas.hpp`: 250×122 1-bit monochrome graphics engine, bitmap typography, and Waveshare 2.13" V2 buffer serializer.
  - `dart_font.hpp`: High-legibility 5x7 ASCII bitmap font with integer scaling.
* **`firmware/raspbi/`**: Fast local simulation & prototyping tool running on Linux / Raspberry Pi OS.
* **`firmware/esp32/`**: Production standalone battery-powered Arduino sketch for the **Adafruit ESP32-S3 Feather + Waveshare 2.13" V2**.

---

## Target 1: Raspberry Pi Simulation Tool (`firmware/raspbi`)

Allows rapid testing, layout adjustments, and verification over SSH on the Raspberry Pi without needing the physical e-paper display connected or flashing the Feather over USB.

### Quick Build & Run (on Raspberry Pi via SSH):
```bash
cd ~/dart_announce/firmware/raspbi
make run
```

### Options:
```bash
# Query a different station (e.g. Connolly):
./dart_sim --station CNNLY

# Filter by direction:
./dart_sim --direction Southbound

# Test with a local XML fixture instead of live network:
./dart_sim --file ../../fixtures/sutton_sample.xml

# Simulate specific battery percentage:
./dart_sim --battery 45
```

### Output:
1. **ASCII/Unicode Visual Screen**: Renders the exact 250×122 e-paper screen directly inside your SSH terminal using half-block characters (`█`, `▀`, `▄`).
2. **`preview_2in13.bmp`**: Saves a 1-bit uncompressed 250×122 Windows BMP image for inspection.
3. **`screen_2in13.bin`**: Saves the exact 4,000-byte raw hardware buffer sent to the Waveshare panel.

---

## Target 2: Standalone Kitchen Display (`firmware/esp32`)

The production wall-mounted device:
* **Microcontroller**: Adafruit ESP32-S3 Feather (4MB Flash, 2MB PSRAM)
* **Display**: Waveshare 2.13-inch E-Paper V2 (250×122, SSD1675 / SSD1680)
* **Power**: 3.7V 2500mAh LiPo / Li-ion battery (recharged via Feather's USB-C port)

### Pin Connections (Adafruit Feather &rarr; Waveshare 2.13" V2)

Connect the 8-pin connector of the Waveshare 2.13" HAT/cable to the Adafruit ESP32-S3 Feather:

| Waveshare 2.13" Pin | Cable Color | Adafruit ESP32-S3 Feather Pin | Description |
| :--- | :--- | :--- | :--- |
| **VCC** | Red | **3V** (3.3V) | Power |
| **GND** | Black | **GND** | Ground |
| **DIN** | Blue | **MOSI** (GPIO 35) | SPI Data Out |
| **CLK** | Yellow | **SCK** (GPIO 36) | SPI Clock |
| **CS** | Orange | **D10** (GPIO 10) | Chip Select |
| **DC** | Green | **D9** (GPIO 9) | Data / Command |
| **RST** | White | **D6** (GPIO 6) | Hardware Reset |
| **BUSY** | Purple | **D5** (GPIO 5) | Busy Status |
| *Internal* | - | **A13** (GPIO 1) | Onboard VBAT 200k/200k Divider (internal) |

---

### Flashing via Arduino IDE

1. Open `firmware/esp32/esp32_dart_display.ino` in **Arduino IDE**.
2. Select Board: **Tools &rarr; Board &rarr; esp32 &rarr; Adafruit Feather ESP32-S3 2MB PSRAM**
3. Select Port: **`COM7`** (or your active serial port)
4. Settings:
   - **Upload Speed**: `921600`
   - **USB CDC On Boot**: `Enabled`
5. Configure your Wi-Fi credentials in lines 39-40:
   ```cpp
   const char* WIFI_SSID     = "YOUR_WIFI_SSID";
   const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
   ```
6. Click **Upload**.

---

## Power & Battery Optimization Architecture

* **Active Cycle**: ~1.2 seconds total (Wi-Fi connect & XML download).
* **Wi-Fi Radio Shutdown**: Radio is turned off immediately after receiving the XML string, saving ~100mA during display refresh.
* **Display Sleep**: Panel enters hardware deep sleep (`0x10, 0x01`) drawing ~2µA.
* **ESP32 Deep Sleep**: Microcontroller draws ~20µA in deep sleep.
* **Commute-Aware Timing**:
  - **Morning Peak (06:30 – 09:30)**: 90-second refresh.
  - **Evening Peak (16:30 – 19:30)**: 90-second refresh.
  - **Off-Peak (Daytime)**: 5-minute refresh.
  - **Overnight (23:00 – 06:30)**: 30-minute sleep (zero screen flash while asleep).
* **Estimated Runtime**: **2 to 3+ months** on a single charge of a 2500mAh LiPo cell.
