#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#include "version.h"

// ----------------------------
// User-configurable defaults
// ----------------------------
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000; // 15s then fall back to AP
static const char*    AP_PASSWORD = nullptr;           // set to "metarlightworks" if you want a password
static const char*    CONFIG_PATH = "/config.json";

// ----------------------------
// Config model
// ----------------------------
struct Config {
  String ssid;
  String pass;
  String airport = "KTIX";
  int brightness  = 120;             // 0-255
  int updateIntervalSec = 300;       // later
};

Config cfg;
WebServer server(80);

// ----------------------------
// Helpers
// ----------------------------
static String macSuffix() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[5];
  snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
  return String(buf);
}

static String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 10);
  for (char c : in) {
    switch (c) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default: out += c; break;
    }
  }
  return out;
}

static bool saveConfig() {
  StaticJsonDocument<512> doc;
  doc["ssid"] = cfg.ssid;
  doc["pass"] = cfg.pass;
  doc["airport"] = cfg.airport;
  doc["brightness"] = cfg.brightness;
  doc["update_interval_sec"] = cfg.updateIntervalSec;

  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) return false;
  if (serializeJson(doc, f) == 0) {
    f.close();
    return false;
  }
  f.close();
  return true;
}

static bool loadConfig() {
  if (!LittleFS.exists(CONFIG_PATH)) return false;

  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return false;

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

cfg.ssid    = String(doc["ssid"]    | "");
cfg.pass    = String(doc["pass"]    | "");
cfg.airport = String(doc["airport"] | "KTIX");
  cfg.brightness = (int)(doc["brightness"] | 120);
  cfg.updateIntervalSec = (int)(doc["update_interval_sec"] | 300);

  // Clamp brightness
  if (cfg.brightness < 0) cfg.brightness = 0;
  if (cfg.brightness > 255) cfg.brightness = 255;

  return true;
}

// ----------------------------
// Wi-Fi modes
// ----------------------------
static void startAP() {
  WiFi.mode(WIFI_AP);
  String apSsid = "METARLightworks-" + macSuffix();

  bool ok;
  if (AP_PASSWORD && strlen(AP_PASSWORD) >= 8) {
    ok = WiFi.softAP(apSsid.c_str(), AP_PASSWORD);
  } else {
    ok = WiFi.softAP(apSsid.c_str());
  }

  IPAddress ip = WiFi.softAPIP();

  Serial.println();
  Serial.println("=== AP MODE ===");
  Serial.print("SSID: "); Serial.println(apSsid);
  if (AP_PASSWORD && strlen(AP_PASSWORD) >= 8) {
    Serial.print("PASS: "); Serial.println(AP_PASSWORD);
  } else {
    Serial.println("PASS: (open)");
  }
  Serial.print("IP:   "); Serial.println(ip);
  Serial.println("================");
}

static bool connectSTA() {
  if (cfg.ssid.length() == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  Serial.println();
  Serial.println("=== STA CONNECT ===");
  Serial.print("SSID: "); Serial.println(cfg.ssid);

  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());

  uint32_t start = millis();
  while (millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
      Serial.println("Connected!");
      Serial.print("IP: "); Serial.println(WiFi.localIP());
      Serial.println("===================");
      return true;
    }
    delay(150);
  }

  Serial.println("STA connect timeout -> AP fallback");
  Serial.println("===================");
  return false;
}

// ----------------------------
// Web UI handlers
// ----------------------------
static String page() {
  String mode = (WiFi.getMode() == WIFI_AP) ? "AP" : "STA";
  String ip = (WiFi.getMode() == WIFI_AP) ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  String ssid = htmlEscape(cfg.ssid);
  String airport = htmlEscape(cfg.airport);

  String html;
  html.reserve(3000);

  html += F("<!doctype html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>");
  html += F("<title>METARLightworks</title>");
  html += F("<style>");
  html += F("body{font-family:-apple-system,system-ui,Segoe UI,Roboto,Helvetica,Arial;margin:20px;max-width:700px}");
  html += F(".card{border:1px solid #ddd;border-radius:14px;padding:16px;margin-bottom:14px}");
  html += F("label{display:block;margin-top:10px;font-weight:600}");
  html += F("input{width:100%;padding:10px;border:1px solid #ccc;border-radius:10px;margin-top:6px}");
  html += F("button{padding:12px 14px;border:0;border-radius:12px;font-weight:700;cursor:pointer}");
  html += F(".row{display:grid;grid-template-columns:1fr 1fr;gap:12px}");
  html += F(".small{color:#666;font-size:13px}");
  html += F("</style></head><body>");

  html += F("<h2>METARLightworks</h2>");
  html += F("<div class='small'>FW ");
  html += FW_VERSION;
  html += F(" • Mode ");
  html += mode;
  html += F(" • IP ");
  html += ip;
  html += F("</div>");

  html += F("<div class='card'>");
  html += F("<h3>Settings</h3>");
  html += F("<form method='POST' action='/save'>");

  html += F("<label>Wi-Fi SSID</label>");
  html += F("<input name='ssid' placeholder='Your Wi-Fi name' value='");
  html += ssid;
  html += F("'>");

  html += F("<label>Wi-Fi Password</label>");
  html += F("<input name='pass' type='password' placeholder='(leave blank to keep existing)' value=''>");

  html += F("<div class='row'>");
  html += F("<div><label>Airport ICAO</label>");
  html += F("<input name='airport' placeholder='KTIX' value='");
  html += airport;
  html += F("'></div>");

  html += F("<div><label>Brightness (0-255)</label>");
  html += F("<input name='brightness' type='number' min='0' max='255' value='");
  html += String(cfg.brightness);
  html += F("'></div>");
  html += F("</div>");

  html += F("<p style='margin-top:14px'>");
  html += F("<button type='submit'>Save & Reboot</button>");
  html += F("</p>");

  html += F("<div class='small'>If you change Wi-Fi password, enter it once here—it's saved to the device.</div>");
  html += F("</form></div>");

  html += F("<div class='card'>");
  html += F("<h3>Status</h3>");
  html += F("<div class='small'>This is the baseline firmware that will later handle OTA updates from GitHub Releases.</div>");
  html += F("</div>");

  html += F("</body></html>");
  return html;
}

static void handleRoot() {
  server.send(200, "text/html", page());
}

static void handleSave() {
  // Read fields
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  String airport = server.arg("airport");
  String brightnessStr = server.arg("brightness");

  ssid.trim();
  airport.trim();

  if (ssid.length() > 0) cfg.ssid = ssid;

  // Only overwrite password if user entered something
  if (pass.length() > 0) cfg.pass = pass;

  if (airport.length() == 4) cfg.airport = airport;
  int b = brightnessStr.toInt();
  if (b >= 0 && b <= 255) cfg.brightness = b;

  bool ok = saveConfig();

  server.send(200, "text/html",
              String("<html><body><h3>Saved: ") + (ok ? "OK" : "FAILED") +
              "</h3><p>Rebooting...</p></body></html>");

  delay(500);
  ESP.restart();
}

static void setupWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);

  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println("Web server started on port 80");
}

// ----------------------------
// Setup / Loop
// ----------------------------
void setup() {
  Serial.begin(115200);

  // Make serial reliable on Apple-silicon Macs / USB CDC
  unsigned long start = millis();
  while (!Serial && millis() - start < 5000) {
    delay(10);
  }

  Serial.println();
  Serial.println("BOOT OK");
  Serial.print("FW: ");
  Serial.println(FW_VERSION);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount FAILED");
  } else {
    Serial.println("LittleFS mounted");
    if (loadConfig()) {
      Serial.println("Config loaded");
    } else {
      Serial.println("No config found (first boot)");
      saveConfig(); // write defaults
    }
  }

  // Try STA, fall back to AP
  bool connected = connectSTA();
  if (!connected) startAP();

  setupWeb();

  // Heartbeat so you always know it's alive
  Serial.println("ALIVE (baseline running)");
}

void loop() {
  server.handleClient();

  static uint32_t last = 0;
  if (millis() - last > 1000) {
    last = millis();
    // Keep a tiny heartbeat without spamming too hard
    // Serial.println("tick");
  }
}