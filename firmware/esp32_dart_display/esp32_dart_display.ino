/*
 * ======================================================================================
 * DART Announce - Standalone Kitchen Commute Display
 * Target: Adafruit ESP32-S3 Feather + Waveshare 2.13" E-Paper V2 (250x122)
 * Power:  Lithium-Ion / LiPo Battery (3.7V 2500mAh) via onboard Feather JST jack
 *
 * Features:
 *   1. Wakes from deep sleep (~20uA).
 *   2. Measures battery voltage via onboard 2:1 divider on GPIO 1 (A13).
 *   3. Reads Wi-Fi credentials & station settings from non-volatile flash (Preferences).
 *   4. AUTO CAPTIVE PORTAL PROVISIONING:
 *      - If Wi-Fi fails to connect (15s timeout), OR
 *      - If no credentials exist, OR
 *      - If the BOOT button (GPIO 0) is held for 3s on wake-up:
 *      -> Enters Setup Mode!
 *      -> Displays "WIFI SETUP MODE" on the Waveshare 2.13" e-paper screen.
 *      -> Broadcasts "DART-Frame-Setup" Wi-Fi hotspot with a Captive Portal.
 *      -> Lets user pick home Wi-Fi, enter password, and select station on their phone!
 *      -> Saves permanently to flash memory (NVS) so you never need USB cables again.
 *   5. Connects to home Wi-Fi and queries Irish Rail API directly:
 *      http://api.irishrail.ie/realtime/realtime.asmx/getStationDataByCodeXML_WithNumMins
 *   6. Shuts down Wi-Fi modem immediately to save power (~100mA savings).
 *   7. Renders 250x122 layout and updates Waveshare 2.13" V2 screen over SPI.
 *   8. Puts display controller into hardware deep sleep (0x10, 0x01).
 *   9. Enters ESP32-S3 deep sleep (90s commute peak, 5m off-peak, overnight sleep).
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
 *   BOOT -> GPIO 0 (Onboard button for manual Wi-Fi setup trigger)
 * ======================================================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <SPI.h>
#include <Wire.h>
#include <base64.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <time.h>
#include <vector>
#include <algorithm>

#include "dart_types.hpp"
#include "dart_parser.hpp"
#include "dart_canvas.hpp"
#include "ota_html.hpp"

// ======================== HARDWARE PINS (Waveshare HAT Rev 2.3) ========================
const int PIN_EPD_CS   = 9;   // Feather D9  (Orange) - Chip Select
const int PIN_EPD_DC   = 10;  // Feather D10 (Green)  - Data/Command
const int PIN_EPD_RST  = 11;  // Feather D11 (White)  - Hardware Reset
const int PIN_EPD_BUSY = 12;  // Feather D12 (Purple) - Status Output
const int PIN_EPD_PWR  = 13;  // Feather D13 (Brown)  - Power Switch Enable (HAT Rev 2.3)
const int PIN_VBAT     = 1;   // GPIO 1 (A13) on Adafruit ESP32-S3 Feather
const int PIN_BOOT_BTN = 0;   // BOOT button on Adafruit ESP32-S3 Feather

#ifndef PIN_I2C_POWER
#define PIN_I2C_POWER 7
#endif

// ======================== DEFAULT FALLBACKS ====================
const char* DEFAULT_STATION_CODE  = "SUTTN";
const char* DEFAULT_DIRECTION     = "Southbound";
const int   DEFAULT_NUM_MINS      = 90;
const uint32_t COMMUTE_REFRESH_SECONDS = 90;
const uint32_t OFFPEAK_REFRESH_SECONDS = 300; // 5 minutes

// Configuration loaded from NVS
Preferences prefs;
String g_wifi_ssid     = "";
String g_wifi_password = "";
String g_station_code  = DEFAULT_STATION_CODE;
String g_direction     = DEFAULT_DIRECTION;
int    g_num_mins      = DEFAULT_NUM_MINS;
bool   g_reverse_mode  = false;
bool   g_schedule_enabled   = true;
int    g_schedule_start_hour = 6;   // 6 AM
int    g_schedule_end_hour   = 21;  // 9 PM

// Dublin Bus configuration (Stop 917 Admiral Park -> Route H1)
String g_bus_route     = "H1";
String g_bus_stop_id   = "8240DB000917";
String g_bus_stop_name = "Stop 917 (Admiral Park)";

// Set to false for Battery Wall Frame Mode (default: ultra-low power deep sleep).
// Set to true in /config web portal for Desk / Test Mode (stays awake with live web preview).
bool   g_test_mode = false;
uint8_t g_bmp_buffer[48062];
size_t g_bmp_size = 0;
dart::BoardData g_last_board;
uint32_t g_last_refresh_ms = 0;

// Battery Telemetry (MAX17048 I2C fuel gauge / USB fallback)
float g_battery_voltage = 4.2f;
int   g_battery_percent = 100;
bool  g_battery_is_lipo = false;

// Weather Telemetry (Open-Meteo live rain & temperature)
int      g_rain_chance     = -1;
float    g_temperature     = -99.0f;
uint32_t g_last_weather_ms = 0;

// ==================== DART STATIONS (DYNAMIC & FALLBACK) ============
struct StationItem {
    String code;
    String name;
    float  lat;
    float  lon;
};

const StationItem FALLBACK_STATIONS[] = {
    {"MHIDE", "Malahide", 53.4509f, -6.1565f},
    {"PMNCK", "Portmarnock", 53.4169f, -6.1512f},
    {"GRGRD", "Clongriffin", 53.4032f, -6.1484f},
    {"HOWTH", "Howth", 53.3891f, -6.0740f},
    {"SUTTN", "Sutton", 53.3925f, -6.1082f},
    {"BYSDE", "Bayside", 53.3916f, -6.1264f},
    {"HWTHJ", "Howth Junction & Donaghmede", 53.3909f, -6.1537f},
    {"KBRCK", "Kilbarrack", 53.3857f, -6.1666f},
    {"RAHNY", "Raheny", 53.3815f, -6.1769f},
    {"HTOWN", "Harmonstown", 53.3786f, -6.1913f},
    {"KLSTR", "Killester", 53.3730f, -6.2045f},
    {"CTARF", "Clontarf Road", 53.3629f, -6.2275f},
    {"CNLLY", "Dublin Connolly", 53.3531f, -6.2459f},
    {"TARA",  "Tara Street", 53.3470f, -6.2543f},
    {"PERSE", "Dublin Pearse", 53.3433f, -6.2476f},
    {"GCDK",  "Grand Canal Dock", 53.3397f, -6.2376f},
    {"LDWNE", "Lansdowne Road", 53.3344f, -6.2297f},
    {"SMONT", "Sandymount", 53.3282f, -6.2207f},
    {"SIDNY", "Sydney Parade", 53.3204f, -6.2111f},
    {"BTSTN", "Booterstown", 53.3105f, -6.2001f},
    {"BROCK", "Blackrock", 53.3006f, -6.1784f},
    {"SEAPT", "Seapoint", 53.2944f, -6.1601f},
    {"SHILL", "Salthill & Monkstown", 53.2944f, -6.1528f},
    {"DLERY", "Dun Laoghaire", 53.2954f, -6.1347f},
    {"SCOVE", "Sandycove & Glasthule", 53.2875f, -6.1260f},
    {"GLGRY", "Glenageary", 53.2811f, -6.1229f},
    {"DLKEY", "Dalkey", 53.2755f, -6.1033f},
    {"KILNY", "Killiney", 53.2557f, -6.1132f},
    {"SKILL", "Shankill", 53.2364f, -6.1165f},
    {"WBROK", "Woodbrook", 53.2185f, -6.1122f},
    {"BRAY",  "Bray", 53.2043f, -6.1082f},
    {"GSTNS", "Greystones", 53.1444f, -6.0632f},
    {"KCOOL", "Kilcoole", 53.1065f, -6.0441f}
};

std::vector<StationItem> g_stations;

void fetchDartStationsList() {
    if (WiFi.status() != WL_CONNECTED) {
        if (g_stations.empty()) {
            for (const auto& s : FALLBACK_STATIONS) g_stations.push_back(s);
        }
        return;
    }

    Serial.println("[Stations] Querying Irish Rail API for live DART stations (StationType=D)...");
    HTTPClient http;
    http.begin("http://api.irishrail.ie/realtime/realtime.asmx/getAllStationsXML_WithStationType?StationType=D");
    http.setTimeout(4000);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        String xml = http.getString();
        std::vector<StationItem> parsed;
        int pos = 0;
        while (true) {
            int stStart = xml.indexOf("<objStation>", pos);
            if (stStart < 0) break;
            int stEnd = xml.indexOf("</objStation>", stStart);
            if (stEnd < 0) break;
            String chunk = xml.substring(stStart, stEnd);
            pos = stEnd + 13;

            StationItem item;
            int descS = chunk.indexOf("<StationDesc>");
            int descE = chunk.indexOf("</StationDesc>");
            if (descS >= 0 && descE > descS) {
                item.name = chunk.substring(descS + 13, descE);
                item.name.trim();
            }

            int codeS = chunk.indexOf("<StationCode>");
            int codeE = chunk.indexOf("</StationCode>");
            if (codeS >= 0 && codeE > codeS) {
                item.code = chunk.substring(codeS + 13, codeE);
                item.code.trim();
            }

            int latS = chunk.indexOf("<StationLatitude>");
            int latE = chunk.indexOf("</StationLatitude>");
            if (latS >= 0 && latE > latS) {
                item.lat = chunk.substring(latS + 17, latE).toFloat();
            }

            int lonS = chunk.indexOf("<StationLongitude>");
            int lonE = chunk.indexOf("</StationLongitude>");
            if (lonS >= 0 && lonE > lonS) {
                item.lon = chunk.substring(lonS + 18, lonE).toFloat();
            }

            if (item.code.length() > 0 && item.name.length() > 0) {
                parsed.push_back(item);
            }
        }

        if (!parsed.empty()) {
            // Sort geographically North to South by latitude
            std::sort(parsed.begin(), parsed.end(), [](const StationItem& a, const StationItem& b) {
                return a.lat > b.lat;
            });
            g_stations = parsed;
            Serial.printf("[Stations] Successfully loaded %d dynamic DART stations from Irish Rail.\n", (int)g_stations.size());
        }
    } else {
        Serial.printf("[Stations] Live stations fetch returned code %d. Using fallback.\n", httpCode);
    }
    http.end();

    if (g_stations.empty()) {
        for (const auto& s : FALLBACK_STATIONS) g_stations.push_back(s);
    }
}

void getStationCoordinates(const String& code, float& outLat, float& outLon) {
    if (g_stations.empty()) {
        fetchDartStationsList();
    }
    for (const auto& s : g_stations) {
        if (s.code.equalsIgnoreCase(code)) {
            outLat = s.lat;
            outLon = s.lon;
            return;
        }
    }
    outLat = 53.387f;
    outLon = -6.120f;
}

// Static working buffer in BSS — avoids FreeRTOS 8KB stack overflow!
static dart::Canvas g_canvas;

// Onboard NeoPixel status indicator for Adafruit Feather ESP32-S3
void setStatusLed(uint8_t r, uint8_t g, uint8_t b) {
#ifdef RGB_BUILTIN
    rgbLedWrite(RGB_BUILTIN, r, g, b);
#endif
}

// Forward declarations
void fetchAndRender();
void redrawCanvas();
void readBatteryStatus();
void fetchWeather();
void fetchDublinBusDepartures();
void fetchDartStationsList();

// ================== WAVESHARE 7.5" V2 (UC8179) DRIVER =============
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

void epdWaitBusy(const char* label = "Operation") {
    uint32_t start = millis();
    int initialPin = digitalRead(PIN_EPD_BUSY);
    Serial.printf("[EPD] %s: waiting for BUSY release (BUSY pin %d is %d)...\n",
                  label, PIN_EPD_BUSY, initialPin);

    // Waveshare 7.5" V2 (UC8179): BUSY pin is 0 (LOW) when BUSY, 1 (HIGH) when IDLE/READY.
    // Do NOT send SPI command 0x71 here, as sending SPI traffic during refresh interrupts UC8179.
    while (digitalRead(PIN_EPD_BUSY) == LOW) {
        if (millis() - start > 15000) { // 15-second safety timeout (full refresh takes ~3-5s)
            Serial.printf("[EPD] %s: BUSY timeout after %u ms!\n", label, millis() - start);
            return;
        }
        delay(10);
    }
    Serial.printf("[EPD] %s: ready in %u ms.\n", label, millis() - start);
    delay(10);
}

void epdReset() {
    digitalWrite(PIN_EPD_RST, HIGH);
    delay(20);
    digitalWrite(PIN_EPD_RST, LOW);
    delay(20); // 20ms held LOW ensures clean wake up from UC8179 Deep Sleep! (was 2ms)
    digitalWrite(PIN_EPD_RST, HIGH);
    delay(20);
}

void epdInit() {
    static bool s_spi_initialized = false;

    // Enable display power switch on Waveshare HAT Rev 2.3
    pinMode(PIN_EPD_PWR, OUTPUT);
    digitalWrite(PIN_EPD_PWR, HIGH);
    delay(20);

    pinMode(PIN_EPD_CS, OUTPUT);
    digitalWrite(PIN_EPD_CS, HIGH);
    pinMode(PIN_EPD_DC, OUTPUT);
    pinMode(PIN_EPD_RST, OUTPUT);
    pinMode(PIN_EPD_BUSY, INPUT_PULLUP);

    if (!s_spi_initialized) {
        SPI.begin(36, -1, 35, PIN_EPD_CS); // SCK=36, MISO=none, MOSI=35, CS=9
        s_spi_initialized = true;
    }
    // Safety: ensure any previous transaction was ended before opening a new one
    SPI.endTransaction();
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));

    Serial.printf("[EPD] Initializing Waveshare 7.5\" V2... (BUSY pin %d = %d)\n",
                  PIN_EPD_BUSY, digitalRead(PIN_EPD_BUSY));

    epdReset();

    // Power Setting
    epdSendCommand(0x01);
    epdSendData(0x07);
    epdSendData(0x07); // VGH=20V, VGL=-20V
    epdSendData(0x3f); // VDH=15V
    epdSendData(0x3f); // VDL=-15V

    // Enhanced display drive (Booster Soft Start - required for UC8179 high-voltage rails)
    epdSendCommand(0x06);
    epdSendData(0x17);
    epdSendData(0x17);
    epdSendData(0x28);
    epdSendData(0x17);

    // Power ON
    epdSendCommand(0x04);
    delay(150); // Required delay for controller to start charge pump and assert BUSY LOW
    epdWaitBusy("Power ON");

    // Panel Setting (800x480, B/W mode)
    epdSendCommand(0x00);
    epdSendData(0x1F); // KW-3f   KWR-2F  BWROTP 0f  BWOTP 1f

    // Resolution: 800 x 480
    epdSendCommand(0x61);
    epdSendData(0x03); // 800 = 0x0320
    epdSendData(0x20);
    epdSendData(0x01); // 480 = 0x01E0
    epdSendData(0xE0);

    // Dual SPI: Disable
    epdSendCommand(0x15);
    epdSendData(0x00);

    // VCOM and data interval setting
    epdSendCommand(0x50);
    epdSendData(0x10);
    epdSendData(0x07);

    // TCON setting
    epdSendCommand(0x60);
    epdSendData(0x22);
}

void epdDisplay(const uint8_t* buffer) {
    const size_t total = dart::Canvas::EPD_BUFFER_SIZE; // exactly 48,000 bytes (800x480/8)
    const size_t chunkSize = 1024;

    Serial.printf("[EPD] Transmitting %u bytes to DTM1 (0x10)...\n", (unsigned int)total);
    // 0x10 = Black/White transmission (DTM1)
    epdSendCommand(0x10);
    digitalWrite(PIN_EPD_DC, HIGH);
    digitalWrite(PIN_EPD_CS, LOW);
    for (size_t offset = 0; offset < total; offset += chunkSize) {
        size_t len = std::min(chunkSize, total - offset);
        SPI.writeBytes(buffer + offset, len);
    }
    digitalWrite(PIN_EPD_CS, HIGH);

    Serial.printf("[EPD] Transmitting %u bytes inverted to DTM2 (0x13)...\n", (unsigned int)total);
    // 0x13 = Inverted data transmission (DTM2)
    epdSendCommand(0x13);
    digitalWrite(PIN_EPD_DC, HIGH);
    digitalWrite(PIN_EPD_CS, LOW);
    static uint8_t s_inv_chunk[1024]; // Static buffer in BSS — avoids 2KB stack hit in FreeRTOS!
    for (size_t offset = 0; offset < total; offset += chunkSize) {
        size_t len = std::min(chunkSize, total - offset);
        for (size_t b = 0; b < len; ++b) {
            s_inv_chunk[b] = ~buffer[offset + b];
        }
        SPI.writeBytes(s_inv_chunk, len);
    }
    digitalWrite(PIN_EPD_CS, HIGH);

    // Display Refresh
    Serial.println("[EPD] Refreshing display panel (0x12)...");
    epdSendCommand(0x12);
    delay(200); // Controller needs ~100-200ms to engage refresh engine and assert BUSY LOW
    epdWaitBusy("Display Refresh");
}

void epdSleep() {
    Serial.println("[EPD] Putting display controller to sleep...");
    epdSendCommand(0x50);
    epdSendData(0xF7);
    epdSendCommand(0x02); // Power OFF
    delay(100);
    epdWaitBusy("Power OFF");
    epdSendCommand(0x07); // Deep sleep
    epdSendData(0xA5);
    digitalWrite(PIN_EPD_CS, HIGH);
    SPI.endTransaction(); // Release SPI bus lock
    Serial.println("[EPD] Display in deep sleep.");
}

void updatePhysicalDisplay(const uint8_t* buffer) {
    epdInit();
    epdDisplay(buffer);
    epdSleep();
}

// ======================== BATTERY TELEMETRY ========================
void readBatteryStatus() {
    static bool s_i2c_initialized = false;
    if (!s_i2c_initialized) {
        pinMode(PIN_I2C_POWER, OUTPUT);
        digitalWrite(PIN_I2C_POWER, HIGH);
        delay(5);
        Wire.begin(SDA, SCL);
        s_i2c_initialized = true;
    }

    // Try MAX17048 I2C fuel gauge at 0x36
    Wire.beginTransmission(0x36);
    Wire.write(0x02); // VCELL register
    if (Wire.endTransmission() == 0) {
        if (Wire.requestFrom((uint8_t)0x36, (size_t)2) == 2) {
            uint16_t raw_v = (Wire.read() << 8) | Wire.read();
            g_battery_voltage = (raw_v * 78.125f) / 1000000.0f;
            g_battery_is_lipo = true;
        }
        Wire.beginTransmission(0x36);
        Wire.write(0x04); // SOC register
        if (Wire.endTransmission() == 0) {
            if (Wire.requestFrom((uint8_t)0x36, (size_t)2) == 2) {
                uint16_t raw_soc = (Wire.read() << 8) | Wire.read();
                g_battery_percent = constrain((int)(raw_soc / 256), 0, 100);
            }
        }
    } else {
        // Fallback: Read onboard 2:1 voltage divider on Adafruit Feather ESP32-S3 (GPIO 1 / A13)
        pinMode(PIN_VBAT, INPUT);
        uint32_t raw_mv = analogReadMilliVolts(PIN_VBAT);
        float v = (raw_mv * 2.0f) / 1000.0f;
        if (v > 2.8f && v < 4.5f) {
            g_battery_voltage = v;
            g_battery_is_lipo = true;
            // Linear approximation for 3.7V LiPo: 3.30V (0%) to 4.15V (100%)
            g_battery_percent = constrain((int)((v - 3.30f) / (4.15f - 3.30f) * 100.0f), 0, 100);
        } else {
            g_battery_is_lipo = false;
            g_battery_voltage = 5.0f;
            g_battery_percent = 100;
        }
    }
}

float readBatteryVoltage() {
    readBatteryStatus();
    return g_battery_voltage;
}

int calculateBatteryPercentage(float voltage) {
    return g_battery_percent;
}

// ======================== WEATHER TELEMETRY ========================
void fetchWeather() {
    // Only re-fetch weather every 15 minutes (or on initial boot)
    if (g_rain_chance >= 0 && millis() - g_last_weather_ms < 900000) {
        return;
    }

    float lat = 53.387f, lon = -6.120f;
    getStationCoordinates(g_station_code, lat, lon);

    char url[180];
    snprintf(url, sizeof(url),
        "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,precipitation_probability",
        lat, lon);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(3000);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        int currentIdx = payload.indexOf("\"current\":");
        if (currentIdx >= 0) {
            int rainIdx = payload.indexOf("\"precipitation_probability\":", currentIdx);
            if (rainIdx >= 0) {
                int endIdx = payload.indexOf("}", rainIdx);
                int commaIdx = payload.indexOf(",", rainIdx);
                int splitIdx = (commaIdx >= 0 && commaIdx < endIdx) ? commaIdx : endIdx;
                g_rain_chance = payload.substring(rainIdx + 28, splitIdx).toInt();
                g_last_weather_ms = millis();
                Serial.printf("[Weather] Precipitation probability: %d%%\n", g_rain_chance);
            }
            int tempIdx = payload.indexOf("\"temperature_2m\":", currentIdx);
            if (tempIdx >= 0) {
                int commaIdx = payload.indexOf(",", tempIdx);
                int braceIdx = payload.indexOf("}", tempIdx);
                int splitIdx = (commaIdx >= 0 && commaIdx < braceIdx) ? commaIdx : braceIdx;
                g_temperature = payload.substring(tempIdx + 17, splitIdx).toFloat();
                Serial.printf("[Weather] Station %s (%.4f, %.4f) Temp: %.1fC\n", g_station_code.c_str(), lat, lon, g_temperature);
            }
        }
    } else {
        Serial.printf("[Weather] Open-Meteo fetch failed (code %d)\n", httpCode);
    }
    http.end();
}

// ======================== DUBLIN BUS REAL-TIME =====================
void fetchDublinBusDepartures() {
    if (WiFi.status() != WL_CONNECTED) {
        g_last_board.bus_success = false;
        g_last_board.bus_error_message = "Wi-Fi Disconnected";
        return;
    }

    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 35000) {
        Serial.printf("[Bus API] Memory warning (%u bytes free), skipping SSL to protect display refresh.\n", freeHeap);
        g_last_board.bus_success = false;
        g_last_board.bus_error_message = "Low Memory";
        return;
    }

    WiFiClientSecure client;
    client.setInsecure(); // Disable SSL certificate verification for speed & minimal RAM usage
    client.setTimeout(6000);

    HTTPClient https;
    String url = "https://bustimes.org/stops/" + g_bus_stop_id;
    Serial.printf("\n[Bus API] Fetching Dublin Bus timetable: %s (Free Heap: %u)\n", url.c_str(), freeHeap);

    yield();
    if (https.begin(client, url)) {
        https.setUserAgent("Mozilla/5.0 (ESP32-S3 Commute Board)");
        https.setTimeout(6000);
        int httpCode = https.GET();
        yield();
        if (httpCode == HTTP_CODE_OK) {
            String payload = https.getString();
            Serial.printf("[Bus API] Received %d bytes HTML.\n", (int)payload.length());
            g_last_board.bus_departures = dart::Parser::parse_bus_html(payload.c_str(), g_bus_route.c_str(), g_last_board.query_time);
            g_last_board.bus_success = true;
            g_last_board.bus_error_message = "";
            Serial.printf("[Bus API] Successfully parsed %d departures for route %s\n",
                          (int)g_last_board.bus_departures.size(), g_bus_route.c_str());
        } else {
            g_last_board.bus_success = false;
            g_last_board.bus_error_message = "HTTP " + std::to_string(httpCode);
            Serial.printf("[Bus API] HTTP GET failed (code %d)\n", httpCode);
        }
        https.end();
    } else {
        g_last_board.bus_success = false;
        g_last_board.bus_error_message = "SSL Init Failed";
        Serial.println("[Bus API] Failed to initialize HTTPS connection to bustimes.org");
    }
    yield();
}

String getLocalTimeString(int* outHour = nullptr) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 1500)) {
        char buf[10];
        snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        if (outHour) *outHour = timeinfo.tm_hour;
        return String(buf);
    }
    return "";
}

uint32_t calculateSleepSeconds(const std::string& query_time) {
    int hour = -1;
    getLocalTimeString(&hour);
    if (hour < 0 && query_time.length() >= 2 && query_time != "--:--") {
        hour = std::atoi(query_time.substr(0, 2).c_str());
    }
    if (hour >= 0) {
        if (g_schedule_enabled && (hour < g_schedule_start_hour || hour >= g_schedule_end_hour)) {
            return 1800; // 30 mins standby sleep during scheduled down hours
        }
        if (hour >= 6 && hour < 10) {
            return COMMUTE_REFRESH_SECONDS;
        }
        if (hour >= 16 && hour < 20) {
            return COMMUTE_REFRESH_SECONDS;
        }
        if (hour >= 23 || hour < 6) {
            return 1800; // 30 mins
        }
    }
    return OFFPEAK_REFRESH_SECONDS;
}

// ======================== NVS CONFIG STORAGE =======================
void loadConfig() {
    prefs.begin("dart_cfg", true); // Read-only mode
    g_wifi_ssid           = prefs.getString("ssid", "");
    g_wifi_password       = prefs.getString("pass", "");
    g_station_code        = prefs.getString("station", DEFAULT_STATION_CODE);
    g_direction           = prefs.getString("direction", DEFAULT_DIRECTION);
    g_num_mins            = prefs.getInt("num_mins", DEFAULT_NUM_MINS);
    g_reverse_mode        = prefs.getBool("reverse", false);
    g_schedule_enabled    = prefs.getBool("sched_en", true);
    g_schedule_start_hour = prefs.getInt("sched_start", 6);
    g_schedule_end_hour   = prefs.getInt("sched_end", 21);
    g_test_mode           = prefs.getBool("test_mode", false); // Default to Battery Wall Frame (Deep Sleep)
    g_bus_route           = prefs.getString("bus_rt", "H1");
    g_bus_stop_id         = prefs.getString("bus_stop", "8240DB000917");
    g_bus_stop_name       = prefs.getString("bus_name", "Stop 917 (Admiral Park)");
    prefs.end();
}

void saveConfig(const String& ssid, const String& pass, const String& station, const String& dir, bool reverse, bool sched_en, int sched_start, int sched_end, bool test_mode, const String& bus_rt = "H1", const String& bus_stop = "8240DB000917", const String& bus_name = "Stop 917 (Admiral Park)") {
    prefs.begin("dart_cfg", false); // Read-write mode
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.putString("station", station);
    prefs.putString("direction", dir);
    prefs.putBool("reverse", reverse);
    prefs.putBool("sched_en", sched_en);
    prefs.putInt("sched_start", sched_start);
    prefs.putInt("sched_end", sched_end);
    prefs.putBool("test_mode", test_mode);
    prefs.putString("bus_rt", bus_rt);
    prefs.putString("bus_stop", bus_stop);
    prefs.putString("bus_name", bus_name);
    prefs.end();
}

// ======================== CAPTIVE PORTAL ===========================
const char* AP_SSID = "DART-Frame-Setup";
const byte DNS_PORT = 53;
DNSServer dnsServer;
WebServer webServer(80);

void showSetupScreenOnEPD() {
    Serial.println("[EPD] Drawing Wi-Fi Setup Mode screen on e-paper...");
    g_canvas.clear(false);
    g_canvas.render_wifi_setup_screen(AP_SSID);

    Serial.println("\n[SETUP SCREEN PREVIEW]");
    g_canvas.print_ascii_screen(Serial);

    updatePhysicalDisplay(g_canvas.get_buffer());
}

String buildPortalHtml() {
    int n = WiFi.scanNetworks();
    String wifiOptions = "";
    for (int i = 0; i < n; ++i) {
        String ssid = WiFi.SSID(i);
        int rssi = WiFi.RSSI(i);
        int quality = constrain(2 * (rssi + 100), 0, 100);
        wifiOptions += "<option value=\"" + ssid + "\">" + ssid + " (" + String(quality) + "%)</option>";
    }
    if (n == 0) {
        wifiOptions = "<option value=\"\">No networks found (type below)</option>";
    }

    fetchDartStationsList();

    String stationHtml = "";
    for (const auto& s : g_stations) {
        stationHtml += "<option value=\"" + s.code + "\"" + (g_station_code.equalsIgnoreCase(s.code) ? " selected" : "") + ">" + s.name + " (" + s.code + ")</option>\n";
    }

    String dirHtml = "";
    dirHtml += "<option value=\"Southbound\"" + String(g_direction == "Southbound" ? " selected" : "") + ">Southbound (towards Bray/Greystones)</option>\n";
    dirHtml += "<option value=\"Northbound\"" + String(g_direction == "Northbound" ? " selected" : "") + ">Northbound (towards Howth/Malahide)</option>\n";
    dirHtml += "<option value=\"Both\"" + String(g_direction == "Both" ? " selected" : "") + ">Both Directions</option>\n";

    String themeHtml = "";
    themeHtml += "<option value=\"0\"" + String(!g_reverse_mode ? " selected" : "") + ">Standard (Black on White)</option>\n";
    themeHtml += "<option value=\"1\"" + String(g_reverse_mode ? " selected" : "") + ">Reverse (White on Black)</option>\n";

    String schedHtml = "";
    schedHtml += "<option value=\"1\"" + String(g_schedule_enabled ? " selected" : "") + ">Enabled (Clock & Weather 9pm - 6am)</option>\n";
    schedHtml += "<option value=\"0\"" + String(!g_schedule_enabled ? " selected" : "") + ">Disabled (Always Live Trains)</option>\n";

    String modeHtml = "";
    modeHtml += "<option value=\"1\"" + String(g_test_mode ? " selected" : "") + ">Desk / Test Mode (Web Preview + Serial ASCII)</option>\n";
    modeHtml += "<option value=\"0\"" + String(!g_test_mode ? " selected" : "") + ">Battery Wall Frame (Deep Sleep)</option>\n";

    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>DART Frame Setup</title>
<style>
  :root { --bg: #0f172a; --card: #1e293b; --text: #f8fafc; --accent: #22c55e; }
  body { background: var(--bg); color: var(--text); font-family: -apple-system, system-ui, sans-serif; padding: 20px; margin: 0; display: flex; justify-content: center; }
  .card { background: var(--card); border-radius: 12px; padding: 24px; max-width: 400px; width: 100%; box-shadow: 0 10px 25px rgba(0,0,0,0.4); }
  h1 { font-size: 22px; margin-top: 0; display: flex; align-items: center; gap: 8px; }
  p { color: #94a3b8; font-size: 14px; margin-bottom: 20px; }
  label { display: block; font-size: 13px; font-weight: 600; margin-bottom: 6px; color: #cbd5e1; }
  input, select { width: 100%; box-sizing: border-box; padding: 12px; margin-bottom: 16px; border-radius: 6px; border: 1px solid #334155; background: #0f172a; color: #fff; font-size: 15px; }
  input:focus, select:focus { outline: 2px solid var(--accent); }
  button { width: 100%; padding: 14px; border: none; border-radius: 6px; background: var(--accent); color: #000; font-size: 16px; font-weight: bold; cursor: pointer; margin-top: 8px; }
  button:hover { background: #16a34a; }
  .footer { text-align: center; font-size: 12px; color: #64748b; margin-top: 20px; }
</style>
</head>
<body>
<div class="card">
  <h1>🚆 DART Frame Setup</h1>
  <p>Connect your wall display to your home Wi-Fi and choose your commuting station.</p>
  <form method="POST" action="/save">
    <label for="ssid">Select Wi-Fi Network</label>
    <select name="ssid" id="ssid" onchange="if(this.value!='') document.getElementById('manual_ssid').value=this.value;">
)rawliteral";
    html += wifiOptions;
    html += R"rawliteral(
      </select>
      <label for="manual_ssid">Or Enter Hidden SSID</label>
      <input type="text" name="manual_ssid" id="manual_ssid" placeholder="Leave blank to keep current network">

      <label for="password">Wi-Fi Password</label>
      <input type="password" name="password" id="password" placeholder="Leave blank to keep current password">

      <label for="station">DART Station</label>
      <select name="station" id="station">
)rawliteral";
    html += stationHtml;
    html += R"rawliteral(
      </select>

      <label for="direction">Direction Filter</label>
      <select name="direction" id="direction">
)rawliteral";
    html += dirHtml;
    html += R"rawliteral(
      </select>

      <label for="bus_route">Dublin Bus Route</label>
      <input type="text" name="bus_route" id="bus_route" value=")rawliteral";
    html += g_bus_route;
    html += R"rawliteral(" placeholder="e.g. H1">

      <label for="bus_stop">Dublin Bus Stop GTFS ID</label>
      <input type="text" name="bus_stop" id="bus_stop" value=")rawliteral";
    html += g_bus_stop_id;
    html += R"rawliteral(" placeholder="e.g. 8240DB000917">

      <label for="bus_name">Dublin Bus Stop Name</label>
      <input type="text" name="bus_name" id="bus_name" value=")rawliteral";
    html += g_bus_stop_name;
    html += R"rawliteral(" placeholder="e.g. Stop 917 (Admiral Park)">

      <label for="theme">Display Theme</label>
      <select name="theme" id="theme">
)rawliteral";
    html += themeHtml;
    html += R"rawliteral(
      </select>

      <label for="sched_en">Smart Scheduler</label>
      <select name="sched_en" id="sched_en">
)rawliteral";
    html += schedHtml;
    html += R"rawliteral(
      </select>

      <label for="op_mode">Operating Mode</label>
      <select name="op_mode" id="op_mode">
)rawliteral";
    html += modeHtml;
    html += R"rawliteral(
      </select>

      <button type="submit">💾 Save Settings</button>
    </form>
    <a href="/" style="display:block; text-align:center; color:#38bdf8; font-size:14px; margin-top:16px; text-decoration:none; font-weight:600;">← Return to Live Preview</a>
    <div style="margin-top:16px; padding-top:14px; border-top:1px solid #334155; text-align:center;">
      <a href="/update" style="display:inline-block; color:#22c55e; font-size:14px; text-decoration:none; font-weight:600;">⬆️ Over-The-Air (OTA) Firmware Update →</a>
    </div>
    <div class="footer">DART Kitchen Display &bull; Weather data by <a href="https://open-meteo.com/" target="_blank" style="color:#94a3b8; text-decoration:underline;">Open-Meteo</a> &bull; Adafruit ESP32-S3</div>
  </div>
  </body>
  </html>
)rawliteral";
    return html;
}

void handleSaveConfig() {
    String ssid = webServer.arg("manual_ssid");
    if (ssid.length() == 0) {
        ssid = webServer.arg("ssid");
    }
    String pass = webServer.arg("password");
    String station = webServer.arg("station");
    String dir = webServer.arg("direction");
    bool reverse = (webServer.arg("theme") == "1");
    bool sched_en = (webServer.arg("sched_en") != "0");
    bool test_mode = (webServer.arg("op_mode") != "0");
    String bus_rt = webServer.arg("bus_route");
    String bus_stop = webServer.arg("bus_stop");
    String bus_name = webServer.arg("bus_name");

    if (bus_rt.length() == 0) bus_rt = g_bus_route;
    if (bus_stop.length() == 0) bus_stop = g_bus_stop_id;
    if (bus_name.length() == 0) bus_name = g_bus_stop_name;

    // Preserve existing credentials if omitted on web dashboard
    if (ssid.length() == 0) {
        ssid = g_wifi_ssid;
    }
    if (pass.length() == 0 && ssid == g_wifi_ssid) {
        pass = g_wifi_password;
    }

    bool wifiChanged = (ssid != g_wifi_ssid || (pass.length() > 0 && pass != g_wifi_password));

    Serial.printf("[CONFIG] Saved: SSID='%s', Station='%s', Dir='%s', Bus='%s' (%s), Reverse=%d, Sched=%d, TestMode=%d\n",
        ssid.c_str(), station.c_str(), dir.c_str(), bus_rt.c_str(), bus_stop.c_str(), reverse ? 1 : 0, sched_en ? 1 : 0, test_mode ? 1 : 0);

    saveConfig(ssid, pass, station, dir, reverse, sched_en, 6, 21, test_mode, bus_rt, bus_stop, bus_name);
    loadConfig();

    if (wifiChanged) {
        String response = R"rawliteral(
<!DOCTYPE html>
<html>
<head><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Saved</title>
<style>body{background:#0f172a;color:#f8fafc;font-family:sans-serif;text-align:center;padding:40px;}</style>
</head>
<body>
  <h2>✓ Wi-Fi Settings Saved!</h2>
  <p>The display is connecting to your new Wi-Fi network...</p>
</body>
</html>
)rawliteral";
        webServer.send(200, "text/html", response);
        delay(1500);
        Serial.println("[SETUP] Restarting ESP32 into new Wi-Fi network...");
        ESP.restart();
    } else {
        // Station/display settings updated: respond immediately to prevent browser timeout!
        String redirectHtml = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta http-equiv="refresh" content="2; url=/">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Settings Saved</title>
  <style>
    body { background: #0f172a; color: #f8fafc; font-family: system-ui, -apple-system, sans-serif; text-align: center; padding: 40px 16px; margin: 0; }
    .box { background: #1e293b; max-width: 440px; margin: 0 auto; border-radius: 12px; padding: 30px; box-shadow: 0 10px 25px rgba(0,0,0,0.5); }
    h2 { color: #22c55e; margin-top: 0; font-size: 22px; }
    p { color: #cbd5e1; font-size: 15px; }
    .spinner { display: inline-block; width: 34px; height: 34px; border: 4px solid #334155; border-top-color: #22c55e; border-radius: 50%; animation: spin 0.8s linear infinite; margin: 16px 0; }
    @keyframes spin { to { transform: rotate(360deg); } }
  </style>
</head>
<body>
  <div class="box">
    <h2>✓ Settings Saved!</h2>
    <p>Updating departures board for new station...</p>
    <div class="spinner"></div>
    <p style="font-size: 13px; color: #94a3b8;">Redirecting to live preview in 2 seconds...</p>
  </div>
</body>
</html>
)rawliteral";
        webServer.send(200, "text/html", redirectHtml);
        webServer.client().stop();
        delay(50);
        fetchAndRender();
    }
}

// OTA web portal HTML is defined in ota_html.hpp

void handleOtaPost() {
    if (Update.hasError()) {
        const esp_partition_t* nextPart = esp_ota_get_next_update_partition(NULL);
        String err = Update.errorString();
        if (!nextPart) {
            err = "Partition table on flash has no OTA partition! In Arduino IDE, change Tools -> Partition Scheme to 'Minimal SPIFFS (1.9MB APP with OTA)' or 'Default 4MB with spiffs' and re-flash via USB once.";
        }
        Serial.printf("[OTA] Update failed: %s\n", err.c_str());
        setStatusLed(32, 0, 0); // Red: Failed
        webServer.sendHeader("Connection", "close");
        webServer.send(500, "text/plain", err);
    } else {
        Serial.println("\n[OTA] Update succeeded! Rebooting ESP32 in 1s...");
        setStatusLed(0, 32, 0); // Green: Success
        webServer.sendHeader("Connection", "close");
        webServer.send(200, "text/plain", "OK");
        delay(1000);
        ESP.restart();
    }
}

void handleOtaUpload() {
    HTTPUpload& upload = webServer.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("\n[OTA] Firmware upload started: %s\n", upload.filename.c_str());
        setStatusLed(32, 0, 32); // Purple: Flashing in progress
        
        // Start OTA with unknown size (or max available partition size)
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            Serial.printf("[OTA] Error Update.begin: %s\n", Update.errorString());
            setStatusLed(32, 0, 0);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Serial.printf("[OTA] Error Update.write: %s\n", Update.errorString());
            setStatusLed(32, 0, 0);
        } else {
            Serial.print(".");
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) { // true = set the size to current progress
            Serial.printf("\n[OTA] Firmware flash complete: %u bytes written.\n", (unsigned int)upload.totalSize);
        } else {
            Serial.printf("\n[OTA] Error Update.end: %s\n", Update.errorString());
            setStatusLed(32, 0, 0);
        }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.end();
        Serial.println("\n[OTA] Firmware upload aborted by client.");
        setStatusLed(48, 24, 0);
    }
}

void startCaptivePortal() {
    setStatusLed(48, 24, 0); // Amber NeoPixel: Captive Portal active
    Serial.println("\n[SETUP] Entering Captive Portal Configuration Mode!");
    showSetupScreenOnEPD();

    if (g_wifi_ssid.length() > 0) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.begin(g_wifi_ssid.c_str(), g_wifi_password.c_str());
        Serial.printf("[SETUP] AP+STA Mode: Connecting to '%s' while broadcasting '%s'...\n", g_wifi_ssid.c_str(), AP_SSID);
    } else {
        WiFi.disconnect(true);
        delay(100);
        WiFi.mode(WIFI_AP);
    }
    IPAddress apIP(192, 168, 4, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_SSID);

    Serial.printf("[SETUP] SoftAP Started: %s\n", AP_SSID);
    Serial.println("[SETUP] Connect your phone/laptop to http://192.168.4.1 (or http://dart-display.local/)");

    dnsServer.start(DNS_PORT, "*", apIP);
    MDNS.begin("dart-display");
    MDNS.addService("http", "tcp", 80);

    webServer.on("/", HTTP_GET, []() {
        webServer.send(200, "text/html", buildPortalHtml());
    });
    webServer.on("/config", HTTP_GET, []() {
        webServer.send(200, "text/html", buildPortalHtml());
    });
    webServer.on("/update", HTTP_GET, []() {
        webServer.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        webServer.send(200, "text/html", buildOtaHtml());
    });
    webServer.on("/update", HTTP_POST, handleOtaPost, handleOtaUpload);

    webServer.on("/save", HTTP_POST, handleSaveConfig);

    // Captive Portal redirection endpoints (iOS, Android, Windows)
    webServer.on("/hotspot-detect.html", []() {
        webServer.sendHeader("Location", "/", true);
        webServer.send(302, "text/plain", "");
    });
    webServer.on("/generate_204", []() {
        webServer.sendHeader("Location", "/", true);
        webServer.send(302, "text/plain", "");
    });
    webServer.onNotFound([]() {
        webServer.sendHeader("Location", "/", true);
        webServer.send(302, "text/plain", "");
    });

    webServer.begin();

    // Run portal loop indefinitely until user configures and saves
    uint32_t lastBlink = millis();
    while (true) {
        dnsServer.processNextRequest();
        webServer.handleClient();
        if (millis() - lastBlink > 2000) {
            Serial.print(".");
            lastBlink = millis();
        }
        delay(5);
    }
}

String buildPreviewHtml() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta http-equiv="refresh" content="60">
  <title>ESP32 DART Display - Live Preview (800x480)</title>
  <style>
    :root { --bg: #0f172a; --card: #1e293b; --accent: #22c55e; }
    body { background: var(--bg); color: #f8fafc; font-family: system-ui, -apple-system, sans-serif; text-align: center; padding: 24px 16px; margin: 0; }
    .card { max-width: 860px; margin: 0 auto; background: var(--card); border-radius: 12px; padding: 24px; box-shadow: 0 10px 25px rgba(0,0,0,0.5); }
    h1 { margin-top: 0; font-size: 22px; display: flex; align-items: center; justify-content: center; gap: 8px; flex-wrap: wrap; }
    p.sub { color: #94a3b8; font-size: 14px; margin-top: -6px; margin-bottom: 20px; }
    .screen-frame { background: #334155; padding: 14px; border-radius: 8px; display: inline-block; margin-bottom: 16px; width: 100%; max-width: 820px; box-sizing: border-box; }
    img.preview { width: 100%; max-width: 800px; height: auto; aspect-ratio: 800 / 480; image-rendering: pixelated; border: 2px solid #000; border-radius: 4px; display: block; margin: 0 auto; }
    .meta { display: grid; grid-template-columns: repeat(auto-fit, minmax(130px, 1fr)); gap: 8px; font-size: 13px; color: #cbd5e1; margin-bottom: 16px; background: #0f172a; padding: 10px; border-radius: 6px; }
    .meta b { color: #38bdf8; }
    .actions { display: flex; gap: 10px; justify-content: center; flex-wrap: wrap; margin-top: 16px; }
    a.btn { background: var(--accent); color: #000; font-weight: bold; padding: 10px 18px; border-radius: 6px; text-decoration: none; font-size: 14px; }
    a.btn:hover { background: #16a34a; }
    a.btn-sec { background: #475569; color: #fff; }
    a.btn-sec:hover { background: #334155; }
    .badge { background: #3b82f6; color: #fff; padding: 3px 8px; border-radius: 4px; font-size: 11px; vertical-align: middle; }
    .footer { font-size: 11px; color: #64748b; margin-top: 20px; }
  </style>
</head>
<body>
<div class="card">
  <h1>🚆 DART Kitchen Display <span class="badge">WAVESHARE 7.5" PREVIEW (800×480)</span></h1>
  <p class="sub">Planned hardware e-paper output rendered on-chip by Adafruit ESP32-S3 for Waveshare 7.5" HAT</p>

  <div class="screen-frame">
)rawliteral";

    html += "<img class=\"preview\" src=\"/screen.bmp?t=" + String(millis()) + "\" alt=\"Waveshare 7.5 E-Paper Display Preview\">";

    html += R"rawliteral(
  </div>

  <div class="meta">
)rawliteral";

    String batInfo;
    if (g_battery_is_lipo) {
        batInfo = String(g_battery_percent) + "% (" + String(g_battery_voltage, 2) + "V)";
    } else {
        batInfo = "USB Powered (Full)";
    }

    String rainInfo = (g_rain_chance >= 0) ? (String(g_rain_chance) + "%") : "--%";
    if (g_temperature > -50.0f) {
        rainInfo += " (" + String(g_temperature, 1) + "°C)";
    }

    html += "<div>Station: <b>" + String(g_last_board.station_name.c_str()) + "</b></div>";
    html += "<div>Dublin Bus: <b>" + String(g_last_board.bus_route.c_str()) + " (" + String(g_last_board.bus_stop_name.c_str()) + ")</b></div>";
    html += "<div>Query Time: <b>" + String(g_last_board.query_time.c_str()) + "</b></div>";
    html += "<div>Rain Chance: <b>" + rainInfo + "</b></div>";
    html += "<div>Battery: <b>" + batInfo + "</b></div>";
    html += "<div>Theme: <b>" + String(g_reverse_mode ? "Reverse (Dark)" : "Standard") + "</b></div>";
    html += "<div>Resolution: <b>800×480</b></div>";

    html += R"rawliteral(
  </div>

  <div class="actions">
    <a href="/refresh" class="btn">🔄 Refresh Trains Now</a>
    <a href="/toggle-theme" class="btn btn-sec">🌓 Toggle Theme</a>
    <a href="/config" class="btn btn-sec">⚙️ Settings & Station</a>
    <a href="/update" class="btn btn-sec">⬆️ Firmware OTA</a>
    <a href="/screen.bmp" target="_blank" class="btn btn-sec">🖼️ View BMP</a>
  </div>

  <div class="footer">
    Auto-refreshes every 60s &bull; Train data by Irish Rail &bull; Weather data by <a href="https://open-meteo.com/" target="_blank" style="color:#94a3b8; text-decoration:underline;">Open-Meteo</a> (CC BY 4.0) &bull; http://dart-display.local/
  </div>
</div>
</body>
</html>
)rawliteral";
    return html;
}

void redrawCanvas() {
    g_canvas.clear(false);

    int curHour = -1;
    String ntpTime = getLocalTimeString(&curHour);
    if (curHour >= 0) {
        Serial.printf("[Clock] True local time: %s (hour %d)\n", ntpTime.c_str(), curHour);
        if (g_last_board.query_time.empty() || g_last_board.query_time == "--:--") {
            g_last_board.query_time = ntpTime.c_str();
        }
    } else {
        curHour = (g_last_board.query_time.length() >= 2 && g_last_board.query_time != "--:--") ?
            std::atoi(g_last_board.query_time.substr(0, 2).c_str()) : 12;
        Serial.printf("[Clock] NTP not ready, using query_time: %s (hour %d)\n", g_last_board.query_time.c_str(), curHour);
    }

    if (g_schedule_enabled && (curHour < g_schedule_start_hour || curHour >= g_schedule_end_hour)) {
        Serial.printf("[Schedule] Standby down period active (%02d:xx): Rendering Standby Clock & Tomorrow's Weather.\n", curHour);
        int rain = (g_rain_chance >= 0) ? g_rain_chance : 50;
        int tmax = (g_temperature > -50.0f) ? (int)(g_temperature + 2) : 15;
        int tmin = (g_temperature > -50.0f) ? (int)(g_temperature - 3) : 9;
        g_canvas.render_clock_weather_screen(g_last_board.station_name, g_last_board.query_time, "NIGHT STANDBY", tmax, tmin, rain, g_battery_percent, g_schedule_start_hour);
    } else {
        g_canvas.render_commute_board(g_last_board, g_battery_percent, g_direction.c_str(), g_rain_chance, g_temperature);
    }
    if (g_reverse_mode) {
        g_canvas.invert_canvas();
    }

    // Export 800x480 1-bit BMP for live browser preview
    g_bmp_size = g_canvas.export_bmp_buffer(g_bmp_buffer, sizeof(g_bmp_buffer));
}

void fetchAndRender() {
    readBatteryStatus();
    fetchWeather();

    char url[200];
    snprintf(url, sizeof(url),
        "http://api.irishrail.ie/realtime/realtime.asmx/getStationDataByCodeXML_WithNumMins?StationCode=%s&NumMins=%d",
        g_station_code.c_str(), g_num_mins);
    Serial.printf("\n[API] Fetching: %s\n", url);

    std::string xmlResponse = "";
    HTTPClient http;
    http.begin(url);
    http.setTimeout(4000); // 4-second timeout to prevent server hanging
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        xmlResponse = http.getString().c_str();
        Serial.printf("[API] Received %d bytes XML.\n", (int)xmlResponse.length());
    } else {
        Serial.printf("[ERROR] HTTP GET failed (code %d)\n", httpCode);
    }
    http.end();

    if (!xmlResponse.empty()) {
        g_last_board = dart::Parser::parse_station_xml(xmlResponse, g_direction.c_str());
        Serial.printf("[Data] Parsed %s: %d trains at %s\n",
            g_last_board.station_name.c_str(), (int)g_last_board.departures.size(), g_last_board.query_time.c_str());
    } else {
        g_last_board.success = false;
        g_last_board.error_message = "Irish Rail API Offline";
    }

    // Resolve station name if Irish Rail returned empty station name (e.g. night hours with no trains)
    if (g_last_board.station_name.empty() || g_last_board.station_name == g_station_code.c_str()) {
        for (const auto& s : g_stations) {
            if (s.code.equalsIgnoreCase(g_station_code)) {
                g_last_board.station_name = s.name.c_str();
                break;
            }
        }
        if (g_last_board.station_name.empty()) {
            g_last_board.station_name = g_station_code.c_str();
        }
    }

    // Resolve time via NTP if Irish Rail returned no trains
    int ntpHour = -1;
    String ntpTime = getLocalTimeString(&ntpHour);
    if (g_last_board.query_time.empty() || g_last_board.query_time == "--:--") {
        if (ntpTime.length() > 0) {
            g_last_board.query_time = ntpTime.c_str();
        } else {
            g_last_board.query_time = "--:--";
        }
    }

    // Fetch Dublin Bus departures for configured stop
    g_last_board.bus_route = g_bus_route.c_str();
    g_last_board.bus_stop_name = g_bus_stop_name.c_str();
    fetchDublinBusDepartures();

    redrawCanvas();

    // 2. Telemetry log (avoid 6KB ASCII dump over USB-CDC which blocks when no terminal is connected)
    Serial.printf("[Screen] 800x480 BMP buffer ready (%d bytes).\n", (int)g_bmp_size);

    // 3. Update physical Waveshare 7.5" HAT Display (safe timeout if disconnected)
    Serial.println("Updating Waveshare 7.5\" HAT e-paper (safe timeout if disconnected)...");
    updatePhysicalDisplay(g_canvas.get_buffer());
    g_last_refresh_ms = millis(); // Ensure at least 60s for web server before next fetch
}

void setupPreviewServer() {
    webServer.on("/", HTTP_GET, []() {
        webServer.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        webServer.sendHeader("Pragma", "no-cache");
        webServer.sendHeader("Expires", "0");
        webServer.sendHeader("Connection", "close");
        webServer.send(200, "text/html", buildPreviewHtml());
    });
    webServer.on("/screen.bmp", HTTP_GET, []() {
        if (g_bmp_size > 0) {
            webServer.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
            webServer.sendHeader("Pragma", "no-cache");
            webServer.sendHeader("Expires", "0");
            webServer.sendHeader("Connection", "close");
            webServer.setContentLength(g_bmp_size);
            webServer.send(200, "image/bmp", "");

            const size_t CHUNK = 2048;
            for (size_t offset = 0; offset < g_bmp_size; offset += CHUNK) {
                size_t len = std::min(CHUNK, g_bmp_size - offset);
                webServer.sendContent((const char*)(g_bmp_buffer + offset), len);
            }
        } else {
            webServer.send(404, "text/plain", "BMP not ready");
        }
    });
    webServer.on("/refresh", HTTP_GET, []() {
        fetchAndRender();
        webServer.sendHeader("Location", "/");
        webServer.sendHeader("Connection", "close");
        webServer.send(302, "text/plain", "");
    });
    webServer.on("/toggle-theme", HTTP_GET, []() {
        g_reverse_mode = !g_reverse_mode;
        prefs.begin("dart_cfg", false);
        prefs.putBool("reverse", g_reverse_mode);
        prefs.end();
        redrawCanvas(); // Instant: redraws buffer in RAM in under 1ms!
        updatePhysicalDisplay(g_canvas.get_buffer());
        webServer.sendHeader("Location", "/");
        webServer.sendHeader("Connection", "close");
        webServer.send(302, "text/plain", "");
    });
    webServer.on("/config", HTTP_GET, []() {
        webServer.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        webServer.sendHeader("Connection", "close");
        webServer.send(200, "text/html", buildPortalHtml());
    });
    webServer.on("/setup", HTTP_GET, []() {
        webServer.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        webServer.sendHeader("Connection", "close");
        webServer.send(200, "text/html", buildPortalHtml());
    });
    webServer.on("/update", HTTP_GET, []() {
        webServer.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        webServer.sendHeader("Connection", "close");
        webServer.send(200, "text/html", buildOtaHtml());
    });
    webServer.on("/update", HTTP_POST, handleOtaPost, handleOtaUpload);
    webServer.on("/save", HTTP_POST, handleSaveConfig);
    webServer.begin();
}

// ======================== POWER & DEEP SLEEP MANAGEMENT ========================
void prepareForDeepSleep(uint32_t sleepSeconds) {
    Serial.printf("\n[Power] Preparing for deep sleep (%u seconds)...\n", sleepSeconds);

    // 1. Put e-paper display controller into deep sleep
    epdSleep();

    // 2. Cut power to Waveshare HAT Rev 2.3 MOSFET power switch (GPIO 13)
    digitalWrite(PIN_EPD_PWR, LOW);
    pinMode(PIN_EPD_PWR, INPUT);

    // 3. Isolate SPI and display control pins to prevent parasitic leakage
    SPI.end();
    pinMode(PIN_EPD_CS, INPUT_PULLUP);
    pinMode(PIN_EPD_DC, INPUT_PULLDOWN);
    pinMode(PIN_EPD_RST, INPUT_PULLDOWN);
    pinMode(PIN_EPD_BUSY, INPUT_PULLDOWN);

    // 4. Turn off onboard NeoPixel LED & cut power
    setStatusLed(0, 0, 0);
#ifdef NEOPIXEL_POWER
    pinMode(NEOPIXEL_POWER, OUTPUT);
    digitalWrite(NEOPIXEL_POWER, !NEOPIXEL_POWER_ON);
#endif

    // 5. Cut I2C bus power
    digitalWrite(PIN_I2C_POWER, LOW);
    pinMode(PIN_I2C_POWER, INPUT);

    // 6. Completely power down Wi-Fi and Bluetooth radios
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    btStop();
    esp_wifi_stop();

    // 7. Flush serial output before sleep
    Serial.println("[Power] Peripherals isolated. Entering ESP32-S3 deep sleep.");
    Serial.flush();

    // 8. Configure wakeups: Timer + BOOT Button (GPIO 0)
    esp_sleep_enable_timer_wakeup((uint64_t)sleepSeconds * 1000000ULL);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BOOT_BTN, 0); // Wake immediately on BOOT button press (LOW)

    // 9. Enter deep sleep
    esp_deep_sleep_start();
}

// ======================== MAIN EXECUTION ========================
void setup() {
    Serial.begin(115200);
    delay(200);

    // Initialize onboard NeoPixel LED on Adafruit Feather ESP32-S3
#ifdef NEOPIXEL_POWER
    pinMode(NEOPIXEL_POWER, OUTPUT);
    digitalWrite(NEOPIXEL_POWER, NEOPIXEL_POWER_ON);
#endif
    setStatusLed(0, 16, 32); // Cyan: booting up

    // Power on Waveshare HAT Rev 2.3 via D13
    pinMode(PIN_EPD_PWR, OUTPUT);
    digitalWrite(PIN_EPD_PWR, HIGH);
    delay(20);

    Serial.println("\n=======================================================");
    Serial.println("  🚆 DART Announce - Adafruit Feather ESP32-S3 Display");
    Serial.println("=======================================================");

    const esp_partition_t* runPart = esp_ota_get_running_partition();
    const esp_partition_t* nextPart = esp_ota_get_next_update_partition(NULL);
    Serial.printf("[OTA] Active Partition: %s, Next OTA Target: %s\n",
        runPart ? runPart->label : "Unknown",
        nextPart ? nextPart->label : "NONE (Board flashed with 'No OTA' partition scheme!)");

    pinMode(PIN_BOOT_BTN, INPUT_PULLUP);

    // 1. Load saved configuration from NVS
    loadConfig();
    Serial.printf("[Config] SSID: '%s', Station: '%s', Dir: '%s', TestMode: %d\n",
        g_wifi_ssid.c_str(), g_station_code.c_str(), g_direction.c_str(), g_test_mode ? 1 : 0);

    // 2. Measure Battery
    float vbat = readBatteryVoltage();
    int batPct = calculateBatteryPercentage(vbat);
    Serial.printf("[Battery] %.2fV (%d%%, LiPo: %d)\n", vbat, batPct, g_battery_is_lipo ? 1 : 0);

    // Low battery protection: prevent LiPo over-discharge damage (< 3.25V)
    if (g_battery_is_lipo && vbat < 3.25f && vbat > 2.0f) {
        Serial.printf("[CRITICAL] Battery voltage %.2fV is critically low! Halting to protect LiPo.\n", vbat);
        g_canvas.clear(false);
        std::string msg1 = "BATTERY CRITICALLY LOW";
        std::string msg2 = "PLEASE RECHARGE VIA USB-C";
        int w1 = g_canvas.get_string_width(msg1, 3);
        int w2 = g_canvas.get_string_width(msg2, 3);
        g_canvas.draw_string((dart::Canvas::WIDTH - w1) / 2, 220, msg1, true, 3);
        g_canvas.draw_string((dart::Canvas::WIDTH - w2) / 2, 255, msg2, true, 3);
        updatePhysicalDisplay(g_canvas.get_buffer());
        prepareForDeepSleep(43200); // Sleep 12 hours
    }

    // 3. Check for manual setup button trigger (holding BOOT button for >2 seconds)
    if (digitalRead(PIN_BOOT_BTN) == LOW) {
        Serial.println("[Trigger] BOOT button pressed! Checking hold duration...");
        delay(2000);
        if (digitalRead(PIN_BOOT_BTN) == LOW) {
            Serial.println("[Trigger] BOOT button held! Forcing Captive Portal Setup.");
            startCaptivePortal();
        }
    }

    // If no SSID configured, enter Captive Portal immediately
    if (g_wifi_ssid.length() == 0) {
        Serial.println("[Setup] No Wi-Fi credentials configured. Launching Captive Portal.");
        startCaptivePortal();
    }

    // 4. Connect Wi-Fi
    setStatusLed(0, 0, 32); // Blue: connecting to Wi-Fi
    Serial.printf("Connecting to Wi-Fi: %s", g_wifi_ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("dart-display");
    WiFi.begin(g_wifi_ssid.c_str(), g_wifi_password.c_str());
    uint32_t wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
        Serial.print(".");
        delay(250);
    }

    // If Wi-Fi fails to connect after 15 seconds:
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\n[ERROR] Failed to connect to saved Wi-Fi network.");
        if (g_test_mode || g_wifi_ssid.length() == 0) {
            Serial.println("[Fallback] Starting Captive Portal for Wi-Fi reconfiguration...");
            startCaptivePortal();
        } else {
            Serial.println("[Battery Mode] Temporary Wi-Fi failure. Entering 2-minute sleep before retry.");
            prepareForDeepSleep(120);
        }
    }

    setStatusLed(0, 32, 0); // Green: connected and running!
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());

    // Start mDNS responder so http://dart-display.local/ works seamlessly
    if (MDNS.begin("dart-display")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("[mDNS] Responder active: http://dart-display.local/");
    }

    // Synchronize local time via SNTP for accurate schedule switching
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    setenv("TZ", "GMT0IST,M3.5.0/1,M10.5.0", 1);
    tzset();
    Serial.println("[NTP] Time synchronization initialized (Europe/Dublin).");

    // 5. Initial Fetch and Render (load dynamic DART stations first for coordinates)
    fetchDartStationsList();
    fetchAndRender();
    g_last_refresh_ms = millis();

    // 6. Test Mode vs Battery Deep Sleep Mode
    if (g_test_mode) {
        Serial.println("\n=================================================================");
        Serial.println("🎉 STANDALONE PREVIEW SERVER ACTIVE ON ESP32!");
        Serial.printf("👉 Open http://dart-display.local/ (or http://%s/) in your browser!\n", WiFi.localIP().toString().c_str());
        Serial.println("Auto-refreshes every 60 seconds over Wi-Fi (Desk / Test Mode).");
        Serial.println("To switch to Battery Deep Sleep Mode, change setting in http://dart-display.local/config");
        Serial.println("=================================================================\n");
        setupPreviewServer();
    } else {
        uint32_t sleepSeconds = calculateSleepSeconds(g_last_board.query_time);
        prepareForDeepSleep(sleepSeconds);
    }
}

void loop() {
    webServer.handleClient();
    if (millis() - g_last_refresh_ms > 60000) {
        g_last_refresh_ms = millis();
        Serial.println("\n[Auto-Refresh] Fetching live DART departures...");
        fetchAndRender();
    }
    delay(10);
}
