/*
 * ======================================================================================
 * DART Announce - Standalone Kitchen Commute Display
 * Target: Adafruit ESP32-S3 Feather + Waveshare 2.13" E-Paper V2 (250x122)
 * Power:  Lithium-Ion / LiPo Battery (3.7V 2500mAh) via onboard Feather JST jack
 *
 * Operation:
 *   1. Wakes from deep sleep (~20uA).
 *   2. Measures battery voltage via onboard 2:1 divider on GPIO 1 (A13).
 *   3. Connects to home Wi-Fi and queries Irish Rail realtime API directly:
 *      http://api.irishrail.ie/realtime/realtime.asmx/getStationDataByCodeXML_WithNumMins
 *   4. Stream-parses XML in memory (Destination, DueIn, Expected, QueryTime).
 *   5. Shuts down Wi-Fi modem immediately to save power (~100mA savings).
 *   6. Renders 250x122 layout and updates Waveshare 2.13" V2 screen over SPI.
 *   7. Puts display controller into hardware deep sleep (0x10, 0x01).
 *   8. Enters ESP32-S3 deep sleep (90s commute peak, 5m off-peak, overnight sleep).
 *
 * Pin Connections (Adafruit ESP32-S3 Feather -> Waveshare 2.13" V2):
 *   VCC  -> 3V (3.3V)
 *   GND  -> GND
 *   DIN  -> MOSI (GPIO 35)
 *   CLK  -> SCK  (GPIO 36)
 *   CS   -> D10  (GPIO 10)
 *   DC   -> D9   (GPIO 9)
 *   RST  -> D6   (GPIO 6)
 *   BUSY -> D5   (GPIO 5)
 *   VBAT -> A13  (GPIO 1, internal on-board 200k/200k divider)
 * ======================================================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>

#include "dart_types.hpp"
#include "dart_parser.hpp"
#include "dart_canvas.hpp"

// ======================== CONFIGURATION ========================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Station code (e.g. "SUTTN" for Sutton, "CNNLY" for Connolly, "HOWTH" for Howth)
const char* STATION_CODE  = "SUTTN";

// Direction filter: "Southbound", "Northbound", or "" (Both directions)
const char* DIRECTION_FILTER = "Southbound";

// Lookahead window in minutes (5 to 90)
const int NUM_MINS = 90;

// Refresh interval during active morning commute (seconds)
const uint32_t COMMUTE_REFRESH_SECONDS = 90;

// Refresh interval during off-peak daytime (seconds)
const uint32_t OFFPEAK_REFRESH_SECONDS = 300; // 5 minutes

// ======================== HARDWARE PINS ========================
const int PIN_EPD_CS   = 10;
const int PIN_EPD_DC   = 9;
const int PIN_EPD_RST  = 6;
const int PIN_EPD_BUSY = 5;
const int PIN_VBAT     = 1;  // GPIO 1 (A13) on Adafruit ESP32-S3 Feather

// ================== WAVESHARE 2.13" V2 LUT & DRIVER =============
const unsigned char lut_full_update[] = {
    0x80,0x60,0x40,0x00,0x00,0x00,0x00,
    0x10,0x60,0x20,0x00,0x00,0x00,0x00,
    0x80,0x60,0x40,0x00,0x00,0x00,0x00,
    0x10,0x60,0x20,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x03,0x03,0x00,0x00,0x02,
    0x09,0x09,0x00,0x00,0x02,
    0x03,0x03,0x00,0x00,0x02,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x15,0x41,0xA8,0x32,0x30,0x0A
};

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
    // 0 = Idle, 1 = Busy
    while (digitalRead(PIN_EPD_BUSY) == HIGH) {
        delay(10);
    }
}

void epdReset() {
    digitalWrite(PIN_EPD_RST, HIGH);
    delay(20);
    digitalWrite(PIN_EPD_RST, LOW);
    delay(10);
    digitalWrite(PIN_EPD_RST, HIGH);
    delay(20);
}

void epdInit() {
    pinMode(PIN_EPD_CS, OUTPUT);
    pinMode(PIN_EPD_DC, OUTPUT);
    pinMode(PIN_EPD_RST, OUTPUT);
    pinMode(PIN_EPD_BUSY, INPUT);

    SPI.begin(36, -1, 35, PIN_EPD_CS); // SCK=36, MISO=none, MOSI=35, CS=10
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));

    epdReset();
    epdWaitBusy();

    epdSendCommand(0x12); // Soft reset
    epdWaitBusy();

    epdSendCommand(0x74); // Analog block control
    epdSendData(0x54);
    epdSendCommand(0x7E); // Digital block control
    epdSendData(0x3B);

    epdSendCommand(0x01); // Driver output control (250 lines)
    epdSendData(0xF9);
    epdSendData(0x00);
    epdSendData(0x00);

    epdSendCommand(0x11); // Data entry mode
    epdSendData(0x01);

    epdSendCommand(0x44); // RAM-X address start/end (0x00 to 0x0F -> 16 bytes)
    epdSendData(0x00);
    epdSendData(0x0F);

    epdSendCommand(0x45); // RAM-Y address start/end (249 to 0)
    epdSendData(0xF9);
    epdSendData(0x00);
    epdSendData(0x00);
    epdSendData(0x00);

    epdSendCommand(0x3C); // Border waveform
    epdSendData(0x03);

    epdSendCommand(0x2C); // VCOM voltage
    epdSendData(0x55);

    epdSendCommand(0x03);
    epdSendData(lut_full_update[70]);

    epdSendCommand(0x04);
    epdSendData(lut_full_update[71]);
    epdSendData(lut_full_update[72]);
    epdSendData(lut_full_update[73]);

    epdSendCommand(0x3A); // Dummy line
    epdSendData(lut_full_update[74]);
    epdSendCommand(0x3B); // Gate time
    epdSendData(lut_full_update[75]);

    epdSendCommand(0x32); // Load LUT
    for (int i = 0; i < 70; i++) {
        epdSendData(lut_full_update[i]);
    }

    epdSendCommand(0x4E); // Set RAM X address count to 0
    epdSendData(0x00);
    epdSendCommand(0x4F); // Set RAM Y address count to 249
    epdSendData(0xF9);
    epdSendData(0x00);
    epdWaitBusy();
}

void epdDisplay(const uint8_t* buffer) {
    epdSendCommand(0x24);
    for (int i = 0; i < dart::Canvas::EPD_BUFFER_SIZE; i++) {
        epdSendData(buffer[i]);
    }

    // Refresh display
    epdSendCommand(0x22);
    epdSendData(0xC7);
    epdSendCommand(0x20);
    epdWaitBusy();
}

void epdSleep() {
    epdSendCommand(0x10); // Enter deep sleep
    epdSendData(0x01);
    delay(100);
    digitalWrite(PIN_EPD_RST, LOW);
}

// ======================== BATTERY TELEMETRY ========================
float readBatteryVoltage() {
    analogReadResolution(12);
    // Onboard 200k/200k divider on GPIO 1 (A13): 3.3V reference / 4095 * 2.0
    uint32_t raw = analogRead(PIN_VBAT);
    float voltage = (raw / 4095.0f) * 3.3f * 2.0f;
    return voltage;
}

int calculateBatteryPercentage(float voltage) {
    if (voltage >= 4.18f) return 100;
    if (voltage <= 3.30f) return 0;
    int pct = (int)((voltage - 3.30f) / (4.18f - 3.30f) * 100.0f);
    return constrain(pct, 0, 100);
}

// Determine sleep duration based on time of day (extracted from Irish Rail clock)
uint32_t calculateSleepSeconds(const std::string& query_time) {
    if (query_time.length() >= 2) {
        int hour = std::atoi(query_time.substr(0, 2).c_str());
        // Morning commute window (06:30 - 09:30): fast 90-second refresh
        if (hour >= 6 && hour < 10) {
            return COMMUTE_REFRESH_SECONDS;
        }
        // Evening commute window (16:30 - 19:30): fast 90-second refresh
        if (hour >= 16 && hour < 20) {
            return COMMUTE_REFRESH_SECONDS;
        }
        // Overnight (23:00 - 06:00): sleep 30 minutes to conserve battery
        if (hour >= 23 || hour < 6) {
            return 1800; // 30 mins
        }
    }
    return OFFPEAK_REFRESH_SECONDS;
}

// ======================== MAIN EXECUTION ========================
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[DART Announce] Standalone Feather Booting...");

    // 1. Measure Battery
    float vbat = readBatteryVoltage();
    int batPct = calculateBatteryPercentage(vbat);
    Serial.printf("[Battery] %.2fV (%d%%)\n", vbat, batPct);

    // Over-discharge cutoff protection: LiPo must not discharge below 3.25V
    if (vbat < 3.25f && vbat > 1.5f) {
        Serial.println("[CRITICAL] LiPo battery voltage too low! Deep sleeping 1 hour.");
        esp_sleep_enable_timer_wakeup(3600ULL * 1000000ULL);
        esp_deep_sleep_start();
    }

    // 2. Connect Wi-Fi
    Serial.printf("Connecting to Wi-Fi: %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 10000) {
        Serial.print(".");
        delay(200);
    }

    std::string xmlResponse = "";
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());

        // 3. Query Irish Rail API directly
        char url[180];
        snprintf(url, sizeof(url),
            "http://api.irishrail.ie/realtime/realtime.asmx/getStationDataByCodeXML_WithNumMins?StationCode=%s&NumMins=%d",
            STATION_CODE, NUM_MINS);
        Serial.printf("Fetching: %s\n", url);

        HTTPClient http;
        http.begin(url);
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            xmlResponse = http.getString().c_str();
            Serial.printf("Received %d bytes XML.\n", (int)xmlResponse.length());
        } else {
            Serial.printf("[ERROR] HTTP GET failed (code %d)\n", httpCode);
        }
        http.end();
    } else {
        Serial.println("\n[ERROR] Wi-Fi connection timeout.");
    }

    // 4. POWER OPTIMIZATION: Shut down Wi-Fi immediately!
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("Wi-Fi radio powered off.");

    // 5. Parse Data & Render 250x122 Framebuffer
    dart::BoardData board;
    if (!xmlResponse.empty()) {
        board = dart::Parser::parse_station_xml(xmlResponse, DIRECTION_FILTER);
        Serial.printf("Parsed: %s, Query Time: %s, Trains: %d\n",
            board.station_name.c_str(), board.query_time.c_str(), (int)board.departures.size());
    } else {
        board.success = false;
        board.error_message = (WiFi.status() == WL_CONNECTED) ? "HTTP API Error" : "Wi-Fi Timeout";
    }

    dart::Canvas canvas;
    canvas.render_commute_board(board, batPct, DIRECTION_FILTER);

    uint8_t epdBuffer[dart::Canvas::EPD_BUFFER_SIZE];
    canvas.export_waveshare_v2_buffer(epdBuffer);

    // 6. Update Waveshare 2.13" V2 Display
    Serial.println("Refreshing Waveshare 2.13\" V2 e-paper...");
    epdInit();
    epdDisplay(epdBuffer);
    epdSleep();
    Serial.println("E-paper refreshed and put into deep sleep (~2uA).");

    // 7. Calculate Sleep Time & Enter ESP32 Deep Sleep
    uint32_t sleepSeconds = calculateSleepSeconds(board.query_time);
    Serial.printf("Entering deep sleep for %u seconds...\n", sleepSeconds);
    esp_sleep_enable_timer_wakeup(sleepSeconds * 1000000ULL);
    esp_deep_sleep_start();
}

void loop() {
    // Unused: execution restarts in setup() on timer wakeup
}
