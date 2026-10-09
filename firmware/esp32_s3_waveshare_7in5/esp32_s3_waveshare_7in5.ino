/*
 * DART Announce - Adafruit ESP32-S3 Feather + Waveshare 7.5" E-Paper (800x480)
 * Production Battery-Powered Firmware (3.7V 2500mAh LiPo)
 *
 * Downloads the pre-rendered 48,000-byte 1-bit monochrome framebuffer from your
 * Raspberry Pi server (/screen.bin?battery=XX) and displays it on the Waveshare 7.5" panel.
 * 
 * Target Board: Adafruit ESP32-S3 Feather (4MB Flash, 2MB PSRAM)
 * Target Panel: Waveshare 7.5inch E-Paper HAT (800x480 V2, UC8179 controller)
 * Battery:      Lithium Ion Polymer Battery - 3.7V 2500mAh
 *
 * Power Optimizations:
 *   - Wi-Fi radio is immediately powered OFF as soon as the buffer is downloaded,
 *     saving ~100mA during the 3-second e-paper refresh cycle.
 *   - E-paper panel enters hardware deep sleep (command 0x07, 0xA5) drawing ~2µA.
 *   - ESP32-S3 enters ultra-low-power deep sleep (~20µA).
 *   - Over-discharge cutoff protection: enters 1-hour sleep if LiPo drops below 3.25V.
 *
 * Pin Connections (Adafruit ESP32-S3 Feather -> Waveshare 7.5" HAT):
 *   VCC  -> 3V (3.3V)
 *   GND  -> GND
 *   DIN  -> MOSI (GPIO 35)
 *   CLK  -> SCK  (GPIO 36)
 *   CS   -> D10  (GPIO 10)
 *   DC   -> D9   (GPIO 9)
 *   RST  -> D6   (GPIO 6)
 *   BUSY -> D5   (GPIO 5)
 *   VBAT -> A13  (GPIO 1, internal on-board 2:1 divider)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <SPI.h>

// --- Configuration ---
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Local hostname of your Raspberry Pi (mDNS) or IP address fallback
// Both work: "http://raspi2modelb2014.local:8000/screen.bin" or "http://192.168.86.28:8000/screen.bin"
const char* SERVER_BASE_URL = "http://raspi2modelb2014.local:8000/screen.bin";

// Update interval (seconds). Recommended 60 to 120 seconds on battery.
const uint32_t SLEEP_SECONDS = 90;

// --- Pin Definitions for Adafruit ESP32-S3 Feather ---
const int PIN_EPD_CS   = 10;
const int PIN_EPD_DC   = 9;
const int PIN_EPD_RST  = 6;
const int PIN_EPD_BUSY = 5;
const int PIN_VBAT     = 1;  // GPIO 1 (A13) on Adafruit ESP32-S3 Feather

// Waveshare 7.5" V2 resolution
const int EPD_WIDTH   = 800;
const int EPD_HEIGHT  = 480;
const int BUFFER_SIZE = (EPD_WIDTH * EPD_HEIGHT) / 8; // exactly 48,000 bytes

// --- Battery Monitoring ---
float readBatteryVoltage() {
    analogReadResolution(12);
    // Adafruit Feather ESP32-S3 has a 200K / 200K (2:1) voltage divider on GPIO 1
    // 3.3V ADC reference / 4095 counts * 2.0 divider multiplier
    uint32_t raw = analogRead(PIN_VBAT);
    float voltage = (raw / 4095.0f) * 3.3f * 2.0f;
    return voltage;
}

int calculateBatteryPercentage(float voltage) {
    if (voltage >= 4.18f) return 100;
    if (voltage <= 3.30f) return 0;
    // Linear approximation across 3.3V (empty) to 4.2V (full)
    int pct = (int)((voltage - 3.30f) / (4.18f - 3.30f) * 100.0f);
    return constrain(pct, 0, 100);
}

// --- Waveshare 7.5" V2 (UC8179) Low-Level SPI Commands ---
void epdSendCommand(uint8_t command) {
    digitalWrite(PIN_EPD_DC, LOW);
    digitalWrite(PIN_EPD_CS, LOW);
    SPI.transfer(command);
    digitalWrite(PIN_EPD_CS, HIGH);
}

void epdSendData(uint8_t data) {
    digitalWrite(PIN_EPD_DC, HIGH);
    digitalWrite(PIN_EPD_CS, LOW);
    SPI.transfer(data);
    digitalWrite(PIN_EPD_CS, HIGH);
}

void epdWaitBusy() {
    // BUSY pin: 0 = Idle, 1 = Busy
    while (digitalRead(PIN_EPD_BUSY) == HIGH) {
        delay(10);
    }
}

void epdReset() {
    digitalWrite(PIN_EPD_RST, HIGH);
    delay(20);
    digitalWrite(PIN_EPD_RST, LOW);
    delay(2);
    digitalWrite(PIN_EPD_RST, HIGH);
    delay(20);
}

void epdInit() {
    pinMode(PIN_EPD_CS, OUTPUT);
    pinMode(PIN_EPD_DC, OUTPUT);
    pinMode(PIN_EPD_RST, OUTPUT);
    pinMode(PIN_EPD_BUSY, INPUT);

    SPI.begin(36, -1, 35, PIN_EPD_CS); // SCK=36, MISO=none, MOSI=35, SS=10
    SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));

    epdReset();
    epdWaitBusy();

    // Power Setting
    epdSendCommand(0x01);
    epdSendData(0x07);
    epdSendData(0x07);
    epdSendData(0x3f);
    epdSendData(0x3f);

    // Power ON
    epdSendCommand(0x04);
    delay(100);
    epdWaitBusy();

    // Panel Setting (800x480, B/W mode)
    epdSendCommand(0x00);
    epdSendData(0x1F); // KW-3f   KWR-2F	BWROTP 0f	BWOTP 1f

    // Resolution: 800 x 480
    epdSendCommand(0x61);
    epdSendData(0x03); // 800 = 0x0320
    epdSendData(0x20);
    epdSendData(0x01); // 480 = 0x01E0
    epdSendData(0xE0);

    // Dual SPI: Disable
    epdSendCommand(0x15);
    epdSendData(0x00);

    // VCOM and data interval
    epdSendCommand(0x50);
    epdSendData(0x10);
    epdSendData(0x07);

    // TCON setting
    epdSendCommand(0x60);
    epdSendData(0x22);
}

void epdDisplay(const uint8_t* buffer) {
    // 0x10 = Old data (transmit white or buffer)
    epdSendCommand(0x10);
    for (int i = 0; i < BUFFER_SIZE; i++) {
        epdSendData(0xFF);
    }

    // 0x13 = New data (black/white bitmap)
    epdSendCommand(0x13);
    for (int i = 0; i < BUFFER_SIZE; i++) {
        epdSendData(buffer[i]);
    }

    // Display Refresh
    epdSendCommand(0x12);
    delay(100);
    epdWaitBusy();
}

void epdSleep() {
    epdSendCommand(0x02); // Power OFF
    epdWaitBusy();
    epdSendCommand(0x07); // Deep sleep
    epdSendData(0xA5);
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[DART Announce] Production ESP32-S3 Feather Booting...");

    // 1. Measure Battery Voltage
    float vbat = readBatteryVoltage();
    int batPct = calculateBatteryPercentage(vbat);
    Serial.printf("[Battery] %.2fV (%d%%)\n", vbat, batPct);

    // Cutoff protection: LiPo must not discharge below 3.25V
    if (vbat < 3.25f && vbat > 1.5f) {
        Serial.println("[CRITICAL] LiPo battery voltage too low! Sleeping 1 hour to prevent damage.");
        esp_sleep_enable_timer_wakeup(3600ULL * 1000000ULL);
        esp_deep_sleep_start();
    }

    // 2. Connect Wi-Fi
    Serial.printf("Connecting to Wi-Fi: %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
        Serial.print(".");
        delay(250);
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\n[ERROR] Wi-Fi connection timeout. Deep sleeping...");
        esp_sleep_enable_timer_wakeup(SLEEP_SECONDS * 1000000ULL);
        esp_deep_sleep_start();
    }

    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());

    // Enable mDNS resolver for .local hostnames
    if (MDNS.begin("esp32-eink")) {
        Serial.println("mDNS responder initialized successfully.");
    }
    uint8_t* buffer = nullptr;
    if (psramFound()) {
        buffer = (uint8_t*)ps_malloc(BUFFER_SIZE);
        Serial.println("Allocated 48KB in PSRAM.");
    } else {
        buffer = (uint8_t*)malloc(BUFFER_SIZE);
        Serial.println("Allocated 48KB in SRAM.");
    }

    if (!buffer) {
        Serial.println("[ERROR] Failed to allocate framebuffer memory!");
        esp_sleep_enable_timer_wakeup(SLEEP_SECONDS * 1000000ULL);
        esp_deep_sleep_start();
    }

    // 4. Download Framebuffer with Battery Telemetry
    char requestUrl[256];
    snprintf(requestUrl, sizeof(requestUrl), "%s?battery=%d", SERVER_BASE_URL, batPct);
    Serial.printf("Fetching: %s\n", requestUrl);

    HTTPClient http;
    http.begin(requestUrl);
    int httpCode = http.GET();
    bool downloadSuccess = false;

    if (httpCode == HTTP_CODE_OK) {
        WiFiClient* stream = http.getStreamPtr();
        int bytesRead = 0;
        while (http.connected() && (bytesRead < BUFFER_SIZE)) {
            size_t available = stream->available();
            if (available) {
                int read = stream->readBytes(buffer + bytesRead, available);
                bytesRead += read;
            }
            delay(1);
        }
        Serial.printf("Downloaded %d / %d bytes.\n", bytesRead, BUFFER_SIZE);
        downloadSuccess = (bytesRead == BUFFER_SIZE);
    } else {
        Serial.printf("[ERROR] HTTP GET failed (code %d)\n", httpCode);
    }
    http.end();

    // 5. POWER OPTIMIZATION: Shut down Wi-Fi immediately!
    // Disconnecting and powering down the Wi-Fi modem saves ~100mA during the e-paper refresh.
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("Wi-Fi radio powered off to save battery.");

    // 6. Update E-Paper Display if download succeeded
    if (downloadSuccess) {
        Serial.println("Initializing Waveshare 7.5\" E-Paper display...");
        epdInit();

        Serial.println("Writing framebuffer and refreshing e-paper...");
        epdDisplay(buffer);

        Serial.println("Putting display controller into deep sleep (~2µA)...");
        epdSleep();
    }

    free(buffer);

    // 7. Enter ESP32-S3 Deep Sleep
    Serial.printf("Entering deep sleep for %u seconds...\n", SLEEP_SECONDS);
    esp_sleep_enable_timer_wakeup(SLEEP_SECONDS * 1000000ULL);
    esp_deep_sleep_start();
}

void loop() {
    // Never reached; execution restarts in setup() on timer wakeup
}
