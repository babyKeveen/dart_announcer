# Firmware: Adafruit ESP32-S3 Feather + Waveshare 7.5" E-Paper (800x480)
## Production Battery-Powered Setup (3.7V 2500mAh LiPo)

This firmware runs on an **Adafruit ESP32-S3 Feather** (4MB Flash, 2MB PSRAM) powered by a **3.7V 2500mAh Lithium Ion Polymer battery** and drives a **Waveshare 7.5inch E-Paper HAT (800x480)**.

---

## 1. Battery Life & Power Architecture

### Power Consumption Breakdown
* **Active Wi-Fi Download**: ~110 mA for ~1.5 seconds (connect to AP, HTTP GET 48KB buffer).
* **Wi-Fi Radio Shutdown**: Radio is turned off immediately after receiving the 48KB buffer.
* **E-Paper Refresh**: ~30 mA for ~2.5 seconds (display controller refresh).
* **Display Sleep Mode**: ~2 µA (command `0x07, 0xA5` puts the UC8179 into hardware sleep).
* **ESP32-S3 Deep Sleep**: ~20 µA.
* **Total Sleep Draw**: ~50 µA (including on-board regulator quiescent current).

### Estimated Battery Runtime (3.7V 2500mAh Battery)

| Refresh Interval | Active Cycles / Hour | Average Current | Estimated Battery Life |
| :--- | :--- | :--- | :--- |
| **60 seconds** | 60 | ~4.2 mA | **~24 days** |
| **90 seconds** (default) | 40 | ~2.8 mA | **~37 days** |
| **120 seconds** | 30 | ~2.1 mA | **~49 days** |
| **Schedule-Aware (sleep 00:00–06:00)** | 40 (18 hrs/day) | ~2.1 mA | **~50–60 days** |

---

## 2. Onboard Battery Telemetry

The Adafruit ESP32-S3 Feather features an onboard 200k/200k (2:1) voltage divider connected to **GPIO 1 (A13)** to measure LiPo battery voltage.

* **Voltage Range**: `4.2V` (100% full) down to `3.3V` (0% empty).
* **Automatic Screen Reporting**: The firmware passes `?battery=XX` in the HTTP GET request. The Raspberry Pi server draws a battery indicator icon and percentage directly on the 800x480 e-paper screen.
* **Low-Voltage Cutoff Protection**: If battery voltage drops below `3.25V`, the ESP32 aborts Wi-Fi transmission and display refresh and enters deep sleep for 1 hour to prevent over-discharging and permanently damaging the LiPo cell.

---

## 3. Wiring Diagram

Connect the 8-pin connector of the Waveshare 7.5" E-Paper HAT to the Adafruit ESP32-S3 Feather:

| Waveshare 7.5" Pin | Cable Color | Adafruit ESP32-S3 Feather Pin | Description |
| :--- | :--- | :--- | :--- |
| **VCC** | Red | **3V** (3.3V) | Power |
| **GND** | Black | **GND** | Ground |
| **DIN** | Blue | **MOSI** (GPIO 35) | SPI Data Out |
| **CLK** | Yellow | **SCK** (GPIO 36) | SPI Clock |
| **CS** | Orange | **D10** (GPIO 10) | Chip Select |
| **DC** | Green | **D9** (GPIO 9) | Data / Command |
| **RST** | White | **D6** (GPIO 6) | Hardware Reset |
| **BUSY** | Purple | **D5** (GPIO 5) | Busy Status |
| *Internal* | - | **A13** (GPIO 1) | Onboard VBAT Divider (Monitored internally) |

---

## 4. Arduino IDE Setup

1. **Install ESP32 Board Support**:
   - `Tools -> Board -> Boards Manager...`
   - Install `esp32` by Espressif Systems (version `2.0.x` or `3.x`).
2. **Select Board**:
   - `Tools -> Board -> ESP32 -> Adafruit Feather ESP32-S3 2MB PSRAM`
3. **Board Settings**:
   - **Flash Size**: `4MB (32Mb)`
   - **PSRAM**: `OPI PSRAM` (or `Enabled`)
   - **Upload Speed**: `921600`
   - **USB CDC On Boot**: `Enabled`
4. **Configure Code**:
   - Open `esp32_s3_waveshare_7in5.ino`
   - Set `WIFI_SSID` and `WIFI_PASSWORD`.
   - Set `SERVER_BASE_URL` to your Raspberry Pi's local hostname (or fallback IP):
     ```cpp
     const char* SERVER_BASE_URL = "http://raspi2modelb2014.local:8000/screen.bin";
     // Fallback if mDNS is not supported: "http://192.168.86.28:8000/screen.bin"
     ```
5. **Flash & Run**:
   - Connect the Feather via USB-C and click **Upload**.
   - Connect the 3.7V 2500mAh LiPo battery to the Feather's JST battery jack.
   - The on-board battery charger will automatically recharge the battery whenever USB-C is plugged in!
