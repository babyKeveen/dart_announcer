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
#include <SPI.h>
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
    prefs.end();
}

void saveConfig(const String& ssid, const String& pass, const String& station, const String& dir, bool reverse, bool sched_en, int sched_start, int sched_end) {
    prefs.begin("dart_cfg", false); // Read-write mode
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.putString("station", station);
    prefs.putString("direction", dir);
    prefs.putBool("reverse", reverse);
    prefs.putBool("sched_en", sched_en);
    prefs.putInt("sched_start", sched_start);
    prefs.putInt("sched_end", sched_end);
    prefs.end();
}

// ======================== CAPTIVE PORTAL ===========================
const char* AP_SSID = "DART-Frame-Setup";
const byte DNS_PORT = 53;
DNSServer dnsServer;
WebServer webServer(80);

void showSetupScreenOnEPD() {
    Serial.println("[EPD] Drawing Wi-Fi Setup Mode screen on e-paper...");
    dart::Canvas canvas;
    canvas.render_wifi_setup_screen(AP_SSID);

    uint8_t epdBuffer[dart::Canvas::EPD_BUFFER_SIZE];
    canvas.export_waveshare_v2_buffer(epdBuffer);

    epdInit();
    epdDisplay(epdBuffer);
    epdSleep();
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

    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>DART Kitchen Frame Setup</title>
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
    <input type="text" name="manual_ssid" id="manual_ssid" placeholder="Network Name">

    <label for="password">Wi-Fi Password</label>
    <input type="password" name="password" id="password" placeholder="Password">

    <label for="station">DART Station</label>
    <select name="station" id="station">
      <option value="SUTTN">Sutton (SUTTN)</option>
      <option value="HOWTH">Howth (HOWTH)</option>
      <option value="BYSDE">Bayside (BYSDE)</option>
      <option value="KBRCK">Kilbarrack (KBRCK)</option>
      <option value="RAHNY">Raheny (RAHNY)</option>
      <option value="HMDWN">Harmonstown (HMDWN)</option>
      <option value="KLSTR">Killester (KLSTR)</option>
      <option value="CNNLY">Dublin Connolly (CNNLY)</option>
      <option value="TARA">Tara Street (TARA)</option>
      <option value="PERSE">Dublin Pearse (PERSE)</option>
      <option value="GCDK">Grand Canal Dock (GCDK)</option>
      <option value="LDWNE">Lansdowne Road (LDWNE)</option>
      <option value="DLGHY">Dun Laoghaire (DLGHY)</option>
      <option value="BRAY">Bray (BRAY)</option>
      <option value="GSTNS">Greystones (GSTNS)</option>
      <option value="MLHT">Malahide (MLHT)</option>
    </select>

    <label for="direction">Direction Filter</label>
    <select name="direction" id="direction">
      <option value="Southbound">Southbound (towards Bray/Greystones)</option>
      <option value="Northbound">Northbound (towards Howth/Malahide)</option>
      <option value="Both">Both Directions</option>
    </select>

    <label for="theme">Display Theme</label>
    <select name="theme" id="theme">
      <option value="0">Standard (Black on White)</option>
      <option value="1">Reverse (White on Black)</option>
    </select>

    <label for="sched_en">Smart Scheduler</label>
    <select name="sched_en" id="sched_en">
      <option value="1">Enabled (Clock & Weather 7pm - 6am)</option>
      <option value="0">Disabled (Always Live Trains)</option>
    </select>

    <button type="submit">💾 Save & Connect</button>
  </form>
  <div class="footer">DART Kitchen Display &bull; Adafruit ESP32-S3</div>
</div>
</body>
</html>
)rawliteral";
    return html;
}

void startCaptivePortal() {
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

    webServer.on("/save", HTTP_POST, []() {
        String ssid = webServer.arg("manual_ssid");
        if (ssid.length() == 0) {
            ssid = webServer.arg("ssid");
        }
        String pass = webServer.arg("password");
        String station = webServer.arg("station");
        String dir = webServer.arg("direction");
        bool reverse = (webServer.arg("theme") == "1");
        bool sched_en = (webServer.arg("sched_en") != "0");

        Serial.printf("[SETUP] Received: SSID='%s', Station='%s', Dir='%s', Reverse=%d, Sched=%d\n",
            ssid.c_str(), station.c_str(), dir.c_str(), reverse ? 1 : 0, sched_en ? 1 : 0);

        saveConfig(ssid, pass, station, dir, reverse, sched_en, 6, 19);

        String response = R"rawliteral(
<!DOCTYPE html>
<html>
<head><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Saved</title>
<style>body{background:#0f172a;color:#f8fafc;font-family:sans-serif;text-align:center;padding:40px;}</style>
</head>
<body>
  <h2>✓ Settings Saved!</h2>
  <p>The display is connecting to your Wi-Fi and loading live trains...</p>
</body>
</html>
)rawliteral";
        webServer.send(200, "text/html", response);
        delay(1500);

        Serial.println("[SETUP] Restarting ESP32 into normal mode...");
        ESP.restart();
    });

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

// ======================== MAIN EXECUTION ========================
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[DART Announce] Standalone Feather Booting...");

    pinMode(PIN_BOOT_BTN, INPUT_PULLUP);

    // 1. Measure Battery
    float vbat = readBatteryVoltage();
    int batPct = calculateBatteryPercentage(vbat);
    Serial.printf("[Battery] %.2fV (%d%%)\n", vbat, batPct);

    if (vbat < 3.25f && vbat > 1.5f) {
        Serial.println("[CRITICAL] LiPo battery voltage too low! Deep sleeping 1 hour.");
        esp_sleep_enable_timer_wakeup(3600ULL * 1000000ULL);
        esp_deep_sleep_start();
    }

    // 2. Check for manual setup button trigger (holding BOOT button for >2 seconds)
    if (digitalRead(PIN_BOOT_BTN) == LOW) {
        Serial.println("[Trigger] BOOT button pressed! Checking hold duration...");
        delay(2000);
        if (digitalRead(PIN_BOOT_BTN) == LOW) {
            Serial.println("[Trigger] BOOT button held! Forcing Captive Portal Setup.");
            startCaptivePortal();
        }
    }

    // 3. Load saved configuration from NVS
    loadConfig();
    Serial.printf("[Config] SSID: '%s', Station: '%s', Dir: '%s'\n",
        g_wifi_ssid.c_str(), g_station_code.c_str(), g_direction.c_str());

    // If no SSID configured, enter Captive Portal immediately
    if (g_wifi_ssid.length() == 0) {
        Serial.println("[Setup] No Wi-Fi credentials configured. Launching Captive Portal.");
        startCaptivePortal();
    }

    // 4. Connect Wi-Fi
    Serial.printf("Connecting to Wi-Fi: %s", g_wifi_ssid.c_str());
    WiFi.mode(WIFI_STA);
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

    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());

    // 5. Query Irish Rail API directly
    char url[200];
    snprintf(url, sizeof(url),
        "http://api.irishrail.ie/realtime/realtime.asmx/getStationDataByCodeXML_WithNumMins?StationCode=%s&NumMins=%d",
        g_station_code.c_str(), g_num_mins);
    Serial.printf("Fetching: %s\n", url);

    std::string xmlResponse = "";
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

    // 6. POWER OPTIMIZATION: Shut down Wi-Fi immediately!
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("Wi-Fi radio powered off.");

    // 7. Parse Data & Render 250x122 Framebuffer
    dart::BoardData board;
    if (!xmlResponse.empty()) {
        board = dart::Parser::parse_station_xml(xmlResponse, g_direction.c_str());
        Serial.printf("Parsed: %s, Query Time: %s, Trains: %d\n",
            board.station_name.c_str(), board.query_time.c_str(), (int)board.departures.size());
    } else {
        board.success = false;
        board.error_message = "Irish Rail API Offline";
    }

    dart::Canvas canvas;
    int curHour = board.query_time.length() >= 2 ? std::atoi(board.query_time.substr(0, 2).c_str()) : 12;
    if (g_schedule_enabled && (curHour < g_schedule_start_hour || curHour >= g_schedule_end_hour)) {
        Serial.println("[Schedule] Down period active! Rendering Standby Clock & Tomorrow's Weather.");
        canvas.render_clock_weather_screen(board.station_name, board.query_time, "NIGHT STANDBY", 15, 9, 50, batPct, g_schedule_start_hour);
    } else {
        canvas.render_commute_board(board, batPct, g_direction.c_str());
    }
    if (g_reverse_mode) {
        canvas.invert_canvas();
    }

    uint8_t epdBuffer[dart::Canvas::EPD_BUFFER_SIZE];
    canvas.export_waveshare_v2_buffer(epdBuffer);

    // 8. Update Waveshare 2.13" V2 Display
    Serial.println("Refreshing Waveshare 2.13\" V2 e-paper...");
    epdInit();
    epdDisplay(epdBuffer);
    epdSleep();
    Serial.println("E-paper refreshed and put into deep sleep (~2uA).");

    // 9. Calculate Sleep Time & Enter ESP32 Deep Sleep
    uint32_t sleepSeconds = calculateSleepSeconds(board.query_time);
    Serial.printf("Entering deep sleep for %u seconds...\n", sleepSeconds);
    esp_sleep_enable_timer_wakeup(sleepSeconds * 1000000ULL);
    esp_deep_sleep_start();
}

void loop() {
    // Unused: execution restarts in setup() on timer wakeup
}
