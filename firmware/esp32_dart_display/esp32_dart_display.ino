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
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <SPI.h>
#include <Wire.h>
#include <base64.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <WebServer.h>

#include "dart_types.hpp"
#include "dart_parser.hpp"
#include "dart_canvas.hpp"

// ======================== HARDWARE PINS ========================
const int PIN_EPD_CS   = 10;
const int PIN_EPD_DC   = 9;
const int PIN_EPD_RST  = 6;
const int PIN_EPD_BUSY = 5;
const int PIN_VBAT     = 1;  // GPIO 1 (A13) on Adafruit ESP32-S3 Feather
const int PIN_BOOT_BTN = 0;  // BOOT button on Adafruit ESP32-S3 Feather

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
int    g_schedule_end_hour   = 19;  // 7 PM

// ================== STANDALONE PREVIEW MODE =====================
// Set to true to test the ESP32 without a physical screen or Raspberry Pi:
//   1. Prints exact ASCII e-paper art directly to Serial Monitor.
//   2. Hosts a live Web Server at http://<ESP32-IP>/ with a pixel-perfect 250x122 display preview!
//   3. Stays awake over USB without resetting connection.
bool   g_test_mode = true;
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

// Static working buffers in BSS — avoids FreeRTOS 8KB stack overflow!
static dart::Canvas g_canvas;
static uint8_t      g_epd_buffer[dart::Canvas::EPD_BUFFER_SIZE];

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

void epdWaitBusy() {
    uint32_t start = millis();
    // Waveshare 7.5" V2 (UC8179): poll 0x71, busy pin is 0 (LOW) when BUSY, 1 (HIGH) when IDLE.
    // With INPUT_PULLUP, if no display is connected, BUSY reads HIGH immediately (non-blocking).
    while (true) {
        epdSendCommand(0x71);
        if (digitalRead(PIN_EPD_BUSY) == HIGH) {
            break; // Ready / Idle
        }
        if (millis() - start > 6000) { // 6 second safety timeout
            Serial.println("[Display] Busy timeout. Continuing.");
            break;
        }
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
    static bool s_spi_initialized = false;
    pinMode(PIN_EPD_CS, OUTPUT);
    digitalWrite(PIN_EPD_CS, HIGH);
    pinMode(PIN_EPD_DC, OUTPUT);
    pinMode(PIN_EPD_RST, OUTPUT);
    pinMode(PIN_EPD_BUSY, INPUT_PULLUP);

    if (!s_spi_initialized) {
        SPI.begin(36, -1, 35, PIN_EPD_CS); // SCK=36, MISO=none, MOSI=35, CS=10
        s_spi_initialized = true;
    }
    // Safety: ensure any previous transaction was ended before opening a new one
    SPI.endTransaction();
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));

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

    // VCOM and data interval
    epdSendCommand(0x50);
    epdSendData(0x10);
    epdSendData(0x07);

    // TCON setting
    epdSendCommand(0x60);
    epdSendData(0x22);
}

void epdDisplay(const uint8_t* buffer) {
    // 0x10 = Old data (transmit white)
    epdSendCommand(0x10);
    digitalWrite(PIN_EPD_DC, HIGH);
    digitalWrite(PIN_EPD_CS, LOW);
    uint8_t ff_block[256];
    memset(ff_block, 0xFF, sizeof(ff_block));
    for (int i = 0; i < dart::Canvas::EPD_BUFFER_SIZE / 256; i++) {
        SPI.writeBytes(ff_block, sizeof(ff_block));
    }
    digitalWrite(PIN_EPD_CS, HIGH);

    // 0x13 = New data (black/white bitmap)
    epdSendCommand(0x13);
    digitalWrite(PIN_EPD_DC, HIGH);
    digitalWrite(PIN_EPD_CS, LOW);
    SPI.writeBytes(buffer, dart::Canvas::EPD_BUFFER_SIZE);
    digitalWrite(PIN_EPD_CS, HIGH);

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
    digitalWrite(PIN_EPD_CS, HIGH);
    SPI.endTransaction(); // Release SPI bus lock
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
        // Fallback: USB powered without LiPo battery
        g_battery_is_lipo = false;
        g_battery_voltage = 5.0f;
        g_battery_percent = 100;
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

    HTTPClient http;
    // Open-Meteo free keyless API for Dublin DART corridor (Sutton: 53.387N, -6.120W)
    http.begin("http://api.open-meteo.com/v1/forecast?latitude=53.387&longitude=-6.120&current=temperature_2m,precipitation_probability");
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
                Serial.printf("[Weather] Temperature: %.1fC\n", g_temperature);
            }
        }
    } else {
        Serial.printf("[Weather] Open-Meteo fetch failed (code %d)\n", httpCode);
    }
    http.end();
}

uint32_t calculateSleepSeconds(const std::string& query_time) {
    if (query_time.length() >= 2) {
        int hour = std::atoi(query_time.substr(0, 2).c_str());
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
    g_schedule_end_hour   = prefs.getInt("sched_end", 19);
    g_test_mode           = prefs.getBool("test_mode", true); // Default to test mode for prototyping
    prefs.end();
}

void saveConfig(const String& ssid, const String& pass, const String& station, const String& dir, bool reverse, bool sched_en, int sched_start, int sched_end, bool test_mode) {
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

    g_canvas.export_waveshare_v2_buffer(g_epd_buffer);
    updatePhysicalDisplay(g_epd_buffer);
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

    struct StationItem { const char* code; const char* name; };
    const StationItem stList[] = {
        {"MHIDE", "Malahide"},
        {"PMNCK", "Portmarnock"},
        {"GRGRD", "Clongriffin"},
        {"HOWTH", "Howth"},
        {"SUTTN", "Sutton"},
        {"BYSDE", "Bayside"},
        {"HWTHJ", "Howth Junction & Donaghmede"},
        {"KBRCK", "Kilbarrack"},
        {"RAHNY", "Raheny"},
        {"HTOWN", "Harmonstown"},
        {"KLSTR", "Killester"},
        {"CTARF", "Clontarf Road"},
        {"CNLLY", "Dublin Connolly"},
        {"TARA",  "Tara Street"},
        {"PERSE", "Dublin Pearse"},
        {"GCDK",  "Grand Canal Dock"},
        {"LDWNE", "Lansdowne Road"},
        {"SMONT", "Sandymount"},
        {"SIDNY", "Sydney Parade"},
        {"BTSTN", "Booterstown"},
        {"BROCK", "Blackrock"},
        {"SEAPT", "Seapoint"},
        {"SHILL", "Salthill & Monkstown"},
        {"DLERY", "Dun Laoghaire"},
        {"SCOVE", "Sandycove & Glasthule"},
        {"GLGRY", "Glenageary"},
        {"DLKEY", "Dalkey"},
        {"KILNY", "Killiney"},
        {"SKILL", "Shankill"},
        {"WBROK", "Woodbrook"},
        {"BRAY",  "Bray"},
        {"GSTNS", "Greystones"},
        {"KCOOL", "Kilcoole"}
    };

    String stationHtml = "";
    for (const auto& s : stList) {
        stationHtml += "<option value=\"" + String(s.code) + "\"" + (g_station_code == s.code ? " selected" : "") + ">" + String(s.name) + " (" + String(s.code) + ")</option>\n";
    }

    String dirHtml = "";
    dirHtml += "<option value=\"Southbound\"" + String(g_direction == "Southbound" ? " selected" : "") + ">Southbound (towards Bray/Greystones)</option>\n";
    dirHtml += "<option value=\"Northbound\"" + String(g_direction == "Northbound" ? " selected" : "") + ">Northbound (towards Howth/Malahide)</option>\n";
    dirHtml += "<option value=\"Both\"" + String(g_direction == "Both" ? " selected" : "") + ">Both Directions</option>\n";

    String themeHtml = "";
    themeHtml += "<option value=\"0\"" + String(!g_reverse_mode ? " selected" : "") + ">Standard (Black on White)</option>\n";
    themeHtml += "<option value=\"1\"" + String(g_reverse_mode ? " selected" : "") + ">Reverse (White on Black)</option>\n";

    String schedHtml = "";
    schedHtml += "<option value=\"1\"" + String(g_schedule_enabled ? " selected" : "") + ">Enabled (Clock & Weather 7pm - 6am)</option>\n";
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
    <div class="footer">DART Kitchen Display &bull; Adafruit ESP32-S3</div>
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

    // Preserve existing credentials if omitted on web dashboard
    if (ssid.length() == 0) {
        ssid = g_wifi_ssid;
    }
    if (pass.length() == 0 && ssid == g_wifi_ssid) {
        pass = g_wifi_password;
    }

    bool wifiChanged = (ssid != g_wifi_ssid || (pass.length() > 0 && pass != g_wifi_password));

    Serial.printf("[CONFIG] Saved: SSID='%s', Station='%s', Dir='%s', Reverse=%d, Sched=%d, TestMode=%d\n",
        ssid.c_str(), station.c_str(), dir.c_str(), reverse ? 1 : 0, sched_en ? 1 : 0, test_mode ? 1 : 0);

    saveConfig(ssid, pass, station, dir, reverse, sched_en, 6, 19, test_mode);
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

void startCaptivePortal() {
    setStatusLed(48, 24, 0); // Amber NeoPixel: Captive Portal active
    Serial.println("\n[SETUP] Entering Captive Portal Configuration Mode!");
    showSetupScreenOnEPD();

    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_AP);
    IPAddress apIP(192, 168, 4, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_SSID);

    Serial.printf("[SETUP] SoftAP Started: %s\n", AP_SSID);
    Serial.println("[SETUP] Connect your phone and browse to http://192.168.4.1");

    dnsServer.start(DNS_PORT, "*", apIP);

    webServer.on("/", HTTP_GET, []() {
        webServer.send(200, "text/html", buildPortalHtml());
    });

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
    <a href="/screen.bmp" target="_blank" class="btn btn-sec">🖼️ View BMP</a>
  </div>

  <div class="footer">
    Auto-refreshes every 60s &bull; http://dart-display.local/ &bull; Waveshare 7.5" HAT (800x480)
  </div>
</div>
</body>
</html>
)rawliteral";
    return html;
}

void redrawCanvas() {
    g_canvas.clear(false);
    int curHour = g_last_board.query_time.length() >= 2 ? std::atoi(g_last_board.query_time.substr(0, 2).c_str()) : 12;
    if (g_schedule_enabled && (curHour < g_schedule_start_hour || curHour >= g_schedule_end_hour)) {
        Serial.println("[Schedule] Down period active: Rendering Standby Clock & Tomorrow's Weather.");
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
    } else if (g_last_board.station_name.empty()) {
        g_last_board.success = false;
        g_last_board.station_name = g_station_code.c_str();
        g_last_board.query_time = "--:--";
        g_last_board.error_message = "Irish Rail API Offline";
    }

    redrawCanvas();

    // 2. Telemetry log (avoid 6KB ASCII dump over USB-CDC which blocks when no terminal is connected)
    Serial.printf("[Screen] 800x480 BMP buffer ready (%d bytes).\n", (int)g_bmp_size);

    // 3. Update physical Waveshare 7.5" HAT Display (safe timeout if disconnected)
    g_canvas.export_waveshare_v2_buffer(g_epd_buffer);
    Serial.println("Updating Waveshare 7.5\" HAT e-paper (safe timeout if disconnected)...");
    updatePhysicalDisplay(g_epd_buffer);
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
        g_canvas.export_waveshare_v2_buffer(g_epd_buffer);
        updatePhysicalDisplay(g_epd_buffer);
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
    webServer.on("/save", HTTP_POST, handleSaveConfig);
    webServer.begin();
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

    Serial.println("\n=======================================================");
    Serial.println("  🚆 DART Announce - Adafruit Feather ESP32-S3 Display");
    Serial.println("=======================================================");

    pinMode(PIN_BOOT_BTN, INPUT_PULLUP);

    // 1. Load saved configuration from NVS
    loadConfig();
    g_test_mode = true; // Always stay awake in desk prototyping mode over USB
    Serial.printf("[Config] SSID: '%s', Station: '%s', Dir: '%s', TestMode: %d\n",
        g_wifi_ssid.c_str(), g_station_code.c_str(), g_direction.c_str(), g_test_mode ? 1 : 0);

    // 2. Measure Battery (never sleep in test mode)
    float vbat = readBatteryVoltage();
    int batPct = calculateBatteryPercentage(vbat);
    Serial.printf("[Battery] %.2fV (%d%%)\n", vbat, batPct);

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

    // If Wi-Fi fails to connect after 15 seconds, trigger Captive Portal!
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\n[ERROR] Failed to connect to saved Wi-Fi network.");
        Serial.println("[Fallback] Starting Captive Portal for Wi-Fi reconfiguration...");
        startCaptivePortal();
    }

    setStatusLed(0, 32, 0); // Green: connected and running!
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());

    // Start mDNS responder so http://dart-display.local/ works seamlessly
    if (MDNS.begin("dart-display")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("[mDNS] Responder active: http://dart-display.local/");
    }

    // 5. Initial Fetch and Render
    fetchAndRender();
    g_last_refresh_ms = millis();

    // 6. Test Mode vs Battery Deep Sleep Mode
    if (g_test_mode) {
        Serial.println("\n=================================================================");
        Serial.println("🎉 STANDALONE PREVIEW SERVER ACTIVE ON ESP32!");
        Serial.printf("👉 Open http://dart-display.local/ (or http://%s/) in your browser!\n", WiFi.localIP().toString().c_str());
        Serial.println("Auto-refreshes every 60 seconds over Wi-Fi (no deep sleep).");
        Serial.println("=================================================================\n");
        setupPreviewServer();
    } else {
        // Battery Wall Frame Mode: shut down Wi-Fi immediately to save power
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        Serial.println("Wi-Fi radio powered off.");

        uint32_t sleepSeconds = calculateSleepSeconds(g_last_board.query_time);
        Serial.printf("Entering deep sleep for %u seconds...\n", sleepSeconds);
        esp_sleep_enable_timer_wakeup(sleepSeconds * 1000000ULL);
        esp_deep_sleep_start();
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
