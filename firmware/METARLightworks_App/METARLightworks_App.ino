// ============================================================
// North METAR Lamp + Flight Pulse (Tail OR Hex UI) — APP Firmware (ESP32 + ESP32-C3)
// ============================================================
// - Reads LittleFS /config.json (written by FACTORY firmware)
// - SoftAP is ALWAYS ON so web UI is always reachable
// - Also attempts STA Wi-Fi using config.wifi creds (AP+STA)
// - mDNS: airportcode.local (ktix.local, etc) updates live on airport change
// - Brightness slider: Preview + Save
// - OTA in app: Check Now + Install Update
// - Auto-update default OFF, interval is DAYS
//
// NOTE (ESP32-C3): Tools → USB CDC On Boot → Enabled
// ============================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <time.h>
#include <ESPmDNS.h>
#include <math.h>
#include <Update.h>
#include <ESP.h>

#include "version.h"
#include "AppTypes.h"
#include "AdminUI.h"

// ================= WEB (GLOBAL so AdminUI.h can extern it) =================
WebServer server(80);

// ================= Config (GLOBAL so AdminUI.h can extern it) =================
AppConfig cfg;

// ================= FS =================
static const char* CONFIG_PATH = "/config.json";

// ================= LED runtime (dynamic so cfg pin/count/order can apply) =================
Adafruit_NeoPixel* strip = nullptr;

// ================= UI/METAR state =================
String flight_category, metar_station, metar_time, metar_wind,
       metar_visibility, metar_temp, metar_dewpoint, metar_pressure, metar_gust;

// ================= Runtime =================
bool connected = false;
unsigned long lastMetarFetch = 0;
const unsigned long fetchInterval = 20UL * 60UL * 1000UL;
bool lastScheduleOn = false;

// ================= Display Mode =================
enum DisplayMode {
  MODE_AUTO = 0,
  MODE_VFR,
  MODE_MVFR,
  MODE_IFR,
  MODE_LIFR,
  MODE_CYCLE
};

DisplayMode displayMode = MODE_AUTO;
unsigned long lastCycleSwitch = 0;
int cycleIndex = 0;

// ================= Flight Pulse =================
bool fpIsFlying = false;
unsigned long fpLastCheckMs = 0;
const unsigned long fpCheckIntervalMs = 45000; // 45s
int fpFlyingStreak = 0;

bool fpPulseActive = false;
unsigned long fpPulseStartMs = 0;
int fpBaseBrightness = 100;

// pulse tuning
const float FP_PERIOD_MS = 3500.0f;
const float FP_MIN_FRACTION = 0.15f;

// ================= Chip helper (portable) =================
static String chipModelStr() {
  // On your core, ESP.getChipModel() returns const char*
  // Wrapping into String makes it portable and easy to search.
  return String(ESP.getChipModel());
}

// ================= OTA constants =================
static const char* OTA_OWNER = "METARlightworks";
static const char* OTA_REPO  = "metar-lamp-firmware";

static const char* OTA_ASSET_ESP32      = "METARLightworks_App_ESP32.bin";
static const char* OTA_ASSET_ESP32C3    = "METARLightworks_App_ESP32C3.bin";
static const char* OTA_ASSET_ESP32S3M   = "METARLightworks_App_ESP32S3_MATRIX.bin";

static const char* otaAssetNameForThisChip() {
  String m = chipModelStr();
  m.toUpperCase();

  if (m.indexOf("S3") >= 0) return OTA_ASSET_ESP32S3M;
  if (m.indexOf("C3") >= 0) return OTA_ASSET_ESP32C3;
  return OTA_ASSET_ESP32; // default OG ESP32
}


// ================= OTA runtime status =================
String otaLatestTag = "";
String otaLatestUrl = "";
int    otaLatestSize = 0;
String otaStatusLine = "Not checked yet";
bool   otaUpdateAvailable = false;
unsigned long otaLastCheckMs = 0;

// ================= Forward declarations =================
void setupWebServer();
void applyModeColor();
void fetchAndDisplayMETAR();
void fpUpdatePulseOverlay();
void restartMDNSForAirport();

bool loadConfig();     // NOT static (AdminUI.h calls saveConfig via extern)
bool saveConfig();     // NOT static (AdminUI.h calls saveConfig via extern)

// ================= helpers =================
static void serialAttachSafe() {
  Serial.setTxBufferSize(512);
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 5000) delay(10);
  Serial.println();
}

static int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static String sanitizeAirportHost(String in) {
  in.trim();
  in.toLowerCase();
  String out;
  out.reserve(in.length());
  for (int i = 0; i < (int)in.length(); i++) {
    char c = in[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-');
    if (ok) out += c;
  }
  if (out.length() == 0) out = "metarlightworks";
  return out;
}

static uint16_t neoOrderFlagFromString(const String& sIn) {
  String s = sIn;
  s.trim(); s.toUpperCase();
  if (s == "RGB") return NEO_RGB;
  if (s == "RBG") return NEO_RBG;
  if (s == "GRB") return NEO_GRB;
  if (s == "GBR") return NEO_GBR;
  if (s == "BRG") return NEO_BRG;
  if (s == "BGR") return NEO_BGR;

  return NEO_RGB; // default
}

static void initStripFromConfig() {
  if (strip) {
    delete strip;
    strip = nullptr;
  }

  cfg.led_count = clampInt(cfg.led_count, 1, 300);
  if (!(cfg.led_order == "RGB" || cfg.led_order == "RBG" || cfg.led_order == "GRB" || cfg.led_order == "GBR" || cfg.led_order == "BRG" || cfg.led_order == "BGR")) cfg.led_order = "RGB";

  uint16_t order = neoOrderFlagFromString(cfg.led_order);
  strip = new Adafruit_NeoPixel(cfg.led_count, cfg.led_pin, order + NEO_KHZ800);
  strip->begin();
  strip->setBrightness(100);
  strip->show();
}

void waitForTimeSync() {
  Serial.print("[NTP] waiting for sync");
  time_t nowSecs = time(nullptr);
  unsigned long start = millis();
  while (nowSecs < 8 * 3600 * 2 && millis() - start < 5000) {
    delay(500);
    Serial.print(".");
    nowSecs = time(nullptr);
  }
  Serial.println(nowSecs < 8 * 3600 * 2 ? " ❌ timed out" : " ✅ synced");
}

void clearLED() {
  if (!strip) return;
  strip->clear();
  strip->show();
}

// NEO_* expects Color(r,g,b)
void setLEDColor(uint8_t r, uint8_t g, uint8_t b) {
  if (!strip) return;
  strip->setBrightness((uint8_t)cfg.brightness);
  for (int i = 0; i < cfg.led_count; i++) {
    strip->setPixelColor(i, strip->Color(r, g, b));
  }
  strip->show();
}

String modeToString() {
  switch (displayMode) {
    case MODE_AUTO:  return "Auto (METAR)";
    case MODE_VFR:   return "Manual VFR";
    case MODE_MVFR:  return "Manual MVFR";
    case MODE_IFR:   return "Manual IFR";
    case MODE_LIFR:  return "Manual LIFR";
    case MODE_CYCLE: return "Cycle Demo";
    default:         return "Unknown";
  }
}

void applyModeColor() {
  if (displayMode == MODE_VFR)  { setLEDColor(0,   255, 0);   return; }
  if (displayMode == MODE_MVFR) { setLEDColor(0,   0,   255); return; }
  if (displayMode == MODE_IFR)  { setLEDColor(255, 0,   0);   return; }
  if (displayMode == MODE_LIFR) { setLEDColor(255, 0,   255); return; }

  if (displayMode == MODE_CYCLE) {
    switch (cycleIndex) {
      case 0: setLEDColor(0,   255, 0);   break;
      case 1: setLEDColor(0,   0,   255); break;
      case 2: setLEDColor(255, 0,   0);   break;
      default:setLEDColor(255, 0,   255); break;
    }
    return;
  }

  if      (flight_category == "VFR")  setLEDColor(0,   255, 0);
  else if (flight_category == "MVFR") setLEDColor(0,   0,   255);
  else if (flight_category == "IFR")  setLEDColor(255, 0,   0);
  else if (flight_category == "LIFR") setLEDColor(255, 0,   255);
  else                                setLEDColor(255, 255, 0);
}

// ================= LittleFS config =================
bool loadConfig() {
  if (!LittleFS.begin(true)) return false;
  if (!LittleFS.exists(CONFIG_PATH)) return false;

  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return false;

  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  cfg.device_ssid = String(doc["device_ssid"] | "METARLightworks");
  cfg.avwx_token  = String(doc["avwx_token"]  | "");

  JsonObject wifi = doc["wifi"].as<JsonObject>();
  if (!wifi.isNull()) {
    cfg.wifi_ssid = String(wifi["ssid"] | "");
    cfg.wifi_pass = String(wifi["pass"] | "");
  }

  cfg.airport_code = String(doc["airport"] | "KTIX");
  cfg.brightness   = (int)(doc["brightness"] | 100);

  JsonObject sched = doc["schedule"].as<JsonObject>();
  if (!sched.isNull()) {
    cfg.scheduleEnabled = (bool)(sched["enabled"] | false);
    cfg.timezonePref    = String(sched["tz"] | "UTC0");
    cfg.startHour       = (int)(sched["starth"] | 0);
    cfg.startMinute     = (int)(sched["startm"] | 0);
    cfg.endHour         = (int)(sched["endh"] | 23);
    cfg.endMinute       = (int)(sched["endm"] | 59);
  }

  cfg.displayMode = (int)(doc["mode"] | 0);
  if (cfg.displayMode < 0 || cfg.displayMode > 5) cfg.displayMode = 0;

  JsonObject fp = doc["flightpulse"].as<JsonObject>();
  if (!fp.isNull()) {
    cfg.fpEnabled = (bool)(fp["enabled"] | false);
    cfg.fpIcao    = String(fp["icao"] | "");
    cfg.fpTail    = String(fp["tail"] | "");
  }

  JsonObject ota = doc["ota"].as<JsonObject>();
  if (!ota.isNull()) {
    cfg.otaCheckOnBoot  = (bool)(ota["check_on_boot"] | true);
    cfg.otaAutoUpdate   = (bool)(ota["auto_update"] | false);
    cfg.otaIntervalDays = (int)(ota["interval_days"] | 7);
  }

  // LED advanced
  JsonObject led = doc["led"].as<JsonObject>();
  if (!led.isNull()) {
    cfg.led_pin   = (int)(led["pin"] | 5);
    cfg.led_count = (int)(led["count"] | 1);
    cfg.led_order = String(led["order"] | "RGB");
  } else {
    cfg.led_pin = 5;
    cfg.led_count = 1;
    cfg.led_order = "RGB";
  }

  // sanitize
  cfg.device_ssid.trim();
  if (cfg.device_ssid.length() == 0) cfg.device_ssid = "METARLightworks";

  cfg.brightness = clampInt(cfg.brightness, 3, 100);
  cfg.otaIntervalDays = clampInt(cfg.otaIntervalDays, 1, 60);

  cfg.airport_code.trim(); cfg.airport_code.toUpperCase();
  cfg.fpIcao.trim(); cfg.fpIcao.toUpperCase();
  cfg.fpTail.trim(); cfg.fpTail.toUpperCase();

  cfg.led_count = clampInt(cfg.led_count, 1, 300);
  cfg.led_order.trim(); cfg.led_order.toUpperCase();
  if (!(cfg.led_order == "RGB" || cfg.led_order == "RBG" || cfg.led_order == "GRB" || cfg.led_order == "GBR" || cfg.led_order == "BRG" || cfg.led_order == "BGR")) cfg.led_order = "RGB";

  return true;
}

bool saveConfig() {
  StaticJsonDocument<4096> doc;

  doc["device_ssid"] = cfg.device_ssid;
  doc["avwx_token"]  = cfg.avwx_token;
  doc["airport"]     = cfg.airport_code;
  doc["brightness"]  = cfg.brightness;
  doc["mode"]        = cfg.displayMode;

  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["ssid"] = cfg.wifi_ssid;
  wifi["pass"] = cfg.wifi_pass;

  JsonObject sched = doc.createNestedObject("schedule");
  sched["enabled"] = cfg.scheduleEnabled;
  sched["tz"]      = cfg.timezonePref;
  sched["starth"]  = cfg.startHour;
  sched["startm"]  = cfg.startMinute;
  sched["endh"]    = cfg.endHour;
  sched["endm"]    = cfg.endMinute;

  JsonObject fp = doc.createNestedObject("flightpulse");
  fp["enabled"] = cfg.fpEnabled;
  fp["icao"]    = cfg.fpIcao;
  fp["tail"]    = cfg.fpTail;

  JsonObject ota = doc.createNestedObject("ota");
  ota["check_on_boot"] = cfg.otaCheckOnBoot;
  ota["auto_update"]   = cfg.otaAutoUpdate;
  ota["interval_days"] = cfg.otaIntervalDays;

  JsonObject led = doc.createNestedObject("led");
  led["pin"]   = cfg.led_pin;
  led["count"] = cfg.led_count;
  led["order"] = cfg.led_order;

  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) return false;
  if (serializeJson(doc, f) == 0) { f.close(); return false; }
  f.close();
  return true;
}

// ================= Wi-Fi =================
static void startSoftAPAlways() {
  WiFi.softAPdisconnect(true);

  const char* apName = cfg.device_ssid.length() ? cfg.device_ssid.c_str() : "METARLightworks";
  bool ok = WiFi.softAP(apName);
  Serial.printf("[WiFi] SoftAP %s: %s\n", apName, ok ? "ON" : "FAILED");

  Serial.print("[WiFi] AP IP: ");
  Serial.println(WiFi.softAPIP());
}

static void connectToWiFi() {
  connected = false;

  if (cfg.wifi_ssid.length()) {
    Serial.printf("[WiFi] Connecting STA to %s...\n", cfg.wifi_ssid.c_str());
    WiFi.begin(cfg.wifi_ssid.c_str(), cfg.wifi_pass.c_str());

    for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
      delay(500);
      Serial.print(".");
    }

    connected = (WiFi.status() == WL_CONNECTED);
    Serial.println(connected ? "\n[WiFi] STA Connected!" : "\n[WiFi] STA Failed.");
    if (connected) {
      Serial.print("[WiFi] STA IP: ");
      Serial.println(WiFi.localIP());
    }
  } else {
    Serial.println("[WiFi] No saved STA creds");
  }
}

// ================= mDNS =================
void restartMDNSForAirport() {
  MDNS.end();

  String host = sanitizeAirportHost(cfg.airport_code);
  if (!MDNS.begin(host.c_str())) {
    Serial.println("[mDNS] start/restart failed");
  } else {
    MDNS.addService("http", "tcp", 80);
    Serial.print("[mDNS] http://");
    Serial.print(host);
    Serial.println(".local");
  }
}

// ================= METAR =================
static void parseAndDisplayMETAR(const String& json) {
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.printf("[METAR] JSON parse failed: %s\n", err.c_str());
    return;
  }

  metar_station   = doc["station"].as<String>();
  metar_time      = doc["time"]["dt"].as<String>();
  flight_category = doc["flight_rules"].as<String>();

  int wind_dir = doc["wind_direction"]["value"] | 0;
  int wind_kt  = doc["wind_speed"]["value"]      | 0;
  float wind_mph = wind_kt * 1.15078f;
  metar_wind = String(wind_dir) + "° at " + wind_kt + " kt / " + String(wind_mph, 1) + " mph";

  int gust_kt = doc["wind_gust"]["value"].isNull() ? 0 : doc["wind_gust"]["value"].as<int>();
  float gust_mph = gust_kt * 1.15078f;
  metar_gust = String(gust_kt) + " kt / " + String(gust_mph, 1) + " mph";

  metar_visibility = doc["visibility"]["value"].isNull()
                     ? "N/A"
                     : String(doc["visibility"]["value"].as<int>()) + " sm";

  int c_temp = doc["temperature"]["value"] | 0;
  int c_dew  = doc["dewpoint"]["value"]    | 0;
  float f_temp = c_temp * 9.0f/5.0f + 32.0f;
  float f_dew  = c_dew  * 9.0f/5.0f + 32.0f;
  metar_temp     = String(c_temp) + " °C / " + String(f_temp, 1) + " °F";
  metar_dewpoint = String(c_dew)  + " °C / " + String(f_dew,  1) + " °F";

  metar_pressure = doc["altimeter"]["value"].isNull()
                   ? "N/A"
                   : String(doc["altimeter"]["value"].as<float>(), 2) + " inHg";

  applyModeColor();
}

static int doOneGet(int attempt) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(7000);
  http.setTimeout(10000);
  http.setReuse(false);

  String url = "https://avwx.rest/api/metar/" + cfg.airport_code + "?format=json&onfail=cache";
  if (!http.begin(client, url)) {
    Serial.printf("[METAR] begin failed (try %d)\n", attempt);
    return -1;
  }

  http.addHeader("Authorization", "Bearer " + cfg.avwx_token);
  int code = http.GET();

  if (code == 200) {
    parseAndDisplayMETAR(http.getString());
    Serial.printf("[METAR] OK (try %d)\n", attempt);
  } else {
    Serial.printf("[METAR] HTTP %d (try %d)\n", code, attempt);
  }

  http.end();
  return code;
}

void fetchAndDisplayMETAR() {
  if (!connected) return;
  if (!cfg.avwx_token.length()) {
    Serial.println("[METAR] Missing AVWX token in /config.json");
    return;
  }

  int code = doOneGet(1);
  if (code == -1 || code == -4 || code == -5 || code == -11) {
    delay(250 + random(0, 250));
    doOneGet(2);
  }
}

// ================= Tail/Hex utilities =================
static bool isHex6(String s) {
  s.trim();
  if (s.length() != 6) return false;
  for (int i = 0; i < 6; i++) {
    char c = toupper(s[i]);
    bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
    if (!ok) return false;
  }
  return true;
}

static int usN_to_icao_int(String tail) {
  tail.trim();
  tail.toUpperCase();

  const String base9  = "123456789";
  const String base10 = "0123456789";
  const String base34 = "ABCDEFGHJKLMNPQRSTUVWXYZ0123456789"; // no I,O

  const int icaooffset = 0xA00001;
  const int b1 = 101711;
  const int b2 = 10111;

  if (tail.length() < 2) return -1;
  if (tail[0] != 'N') return -1;

  int d1 = base9.indexOf(tail[1]);
  if (d1 < 0) return -1;

  int icao = icaooffset + d1 * b1;
  if (tail.length() == 2) return icao;

  auto enc_suffix = [&](String suf) -> int {
    if (suf.length() == 0) return 0;
    suf.toUpperCase();
    int r0 = base34.indexOf(suf[0]);
    if (r0 < 0) return -9999;

    int r1;
    if (suf.length() == 1) r1 = 0;
    else {
      int idx = base34.indexOf(suf[1]);
      if (idx < 0) return -9999;
      r1 = idx + 1;
    }

    if (r0 < 24) return r0 * 25 + r1 + 1;
    return r0 * 35 + r1 - 239;
  };

  int d2 = base10.indexOf(tail[2]);
  if (d2 == -1) {
    String suf = tail.substring(2);
    if (suf.length() > 2) suf = suf.substring(0, 2);
    int enc = enc_suffix(suf);
    if (enc < 0) return -1;
    icao += enc;
    return icao;
  }

  icao += d2 * b2 + 601;
  if (tail.length() == 3) return icao;

  int d3 = base10.indexOf(tail[3]);
  if (d3 > -1) {
    icao += d3 * 951 + 601;
    String suf = "";
    if (tail.length() > 4) suf = tail.substring(4);
    if (suf.length() > 2) suf = suf.substring(0, 2);
    int enc = enc_suffix(suf);
    if (enc < 0) return -1;
    icao += enc;
    return icao;
  } else {
    String suf = tail.substring(3);
    if (suf.length() > 2) suf = suf.substring(0, 2);
    int enc = enc_suffix(suf);
    if (enc < 0) return -1;
    icao += enc;
    return icao;
  }
}

static String usN_to_hex6(String tail) {
  int icao = usN_to_icao_int(tail);
  if (icao < 0) return "";
  char out[7];
  snprintf(out, sizeof(out), "%06X", icao);
  return String(out);
}

static String resolveTailOrHex(String tailIn, String hexIn) {
  tailIn.trim(); tailIn.toUpperCase();
  hexIn.trim();  hexIn.toUpperCase();

  if (tailIn.length()) {
    String h = usN_to_hex6(tailIn);
    if (h.length() == 6) return h;
  }
  if (hexIn.length() && isHex6(hexIn)) return hexIn;
  return "";
}

// ================= Flight Pulse ADSB =================
static bool fpFetchIsFlying_ADSBlol(const String& icaoHex6) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!isHex6(icaoHex6)) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(7000);
  http.setTimeout(10000);
  http.setReuse(false);

  String icao = icaoHex6;
  icao.toLowerCase();
  String url = "https://api.adsb.lol/v2/icao/" + icao;

  if (!http.begin(client, url)) return false;

  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, body)) return false;

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull() || ac.size() == 0) return false;

  JsonObject a = ac[0].as<JsonObject>();

  float gs = a["gs"] | 0.0;
  float alt_baro = a["alt_baro"] | 0.0;
  float alt_geom = a["alt_geom"] | 0.0;
  int gnd = a["gnd"] | 0;

  float alt = (alt_baro > 0.0) ? alt_baro : alt_geom;

  if (gnd == 1) return false;
  if (alt > 500.0 || gs > 35.0) return true;
  return false;
}

static void fpStartPulse() {
  fpPulseActive = true;
  fpPulseStartMs = millis();
  fpBaseBrightness = cfg.brightness;
}

static void fpStopPulseRestore() {
  fpPulseActive = false;
  if (!strip) return;
  strip->setBrightness((uint8_t)cfg.brightness);
  strip->show();
}

void fpUpdatePulseOverlay() {
  if (!fpPulseActive) return;
  if (!strip) return;

  unsigned long now = millis();
  float t = fmod((float)(now - fpPulseStartMs), FP_PERIOD_MS) / FP_PERIOD_MS;
  float wave = 0.5f - 0.5f * cosf(2.0f * PI * t);

  int minB = (int)(fpBaseBrightness * FP_MIN_FRACTION);
  if (minB < 3) minB = 3;
  int maxB = fpBaseBrightness;
  if (maxB < 3) maxB = 3;

  int b = (int)(minB + (maxB - minB) * wave);
  strip->setBrightness((uint8_t)b);
  strip->show();
}

// ================= OTA helpers =================
static String stripLeadingV(const String& v) {
  if (v.length() > 0 && (v[0] == 'v' || v[0] == 'V')) return v.substring(1);
  return v;
}

static int semverCompare3(const String& aIn, const String& bIn) {
  String a = stripLeadingV(aIn);
  String b = stripLeadingV(bIn);

  int ai[3] = {0,0,0}, bi[3] = {0,0,0};

  auto parse3 = [](const String& s, int out[3]) {
    int p1 = s.indexOf('.');
    int p2 = (p1 >= 0) ? s.indexOf('.', p1+1) : -1;
    out[0] = s.substring(0, p1 >= 0 ? p1 : s.length()).toInt();
    if (p1 >= 0) out[1] = s.substring(p1+1, p2 >= 0 ? p2 : s.length()).toInt();
    if (p2 >= 0) out[2] = s.substring(p2+1).toInt();
  };

  parse3(a, ai);
  parse3(b, bi);

  for (int i=0; i<3; i++){
    if (ai[i] > bi[i]) return 1;
    if (ai[i] < bi[i]) return -1;
  }
  return 0;
}

static bool otaGetLatest(String &outTag, String &outUrl, int &outSize) {
  if (WiFi.status() != WL_CONNECTED) return false;

  const char* desired = otaAssetNameForThisChip();
  Serial.printf("[OTA] Desired asset: %s\n", desired);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(12000);

  String api = String("https://api.github.com/repos/")
             + OTA_OWNER + "/" + OTA_REPO + "/releases/latest";

  if (!http.begin(client, api)) return false;

  http.addHeader("User-Agent", "METARLightworks");
  http.addHeader("Accept", "application/vnd.github+json");

  int code = http.GET();
  Serial.printf("[OTA] latest HTTP %d\n", code);

  if (code != 200) {
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  StaticJsonDocument<16384> doc;
  if (deserializeJson(doc, body)) return false;

  outTag = String(doc["tag_name"] | "");

  JsonArray assets = doc["assets"].as<JsonArray>();
  if (assets.isNull()) return false;

  for (JsonObject a : assets) {
    String name = String(a["name"] | "");
    if (name == String(desired)) {
      outUrl  = String(a["browser_download_url"] | "");
      outSize = (int)(a["size"] | 0);
      return (outUrl.length() > 0 && outSize > 0);
    }
  }
  return false;
}

static bool otaCheckNow() {
  otaLatestTag = "";
  otaLatestUrl = "";
  otaLatestSize = 0;
  otaUpdateAvailable = false;

  if (WiFi.status() != WL_CONNECTED) {
    otaStatusLine = "No Wi-Fi";
    return false;
  }

  String tag, url;
  int size = 0;
  if (!otaGetLatest(tag, url, size)) {
    otaStatusLine = "Check failed";
    return false;
  }

  otaLatestTag = tag;
  otaLatestUrl = url;
  otaLatestSize = size;

  int cmp = semverCompare3(tag, FW_VERSION);
  if (cmp > 0) {
    otaUpdateAvailable = true;
    otaStatusLine = "Update available: " + tag;
  } else {
    otaUpdateAvailable = false;
    otaStatusLine = "Up to date (" + String(FW_VERSION) + ")";
  }

  otaLastCheckMs = millis();
  return true;
}

static bool otaInstallNow() {
  if (!otaUpdateAvailable || otaLatestUrl.length() == 0) {
    otaStatusLine = "No update available";
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    otaStatusLine = "No Wi-Fi";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(20000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  otaStatusLine = "Downloading...";
  if (!http.begin(client, otaLatestUrl)) {
    otaStatusLine = "HTTP begin failed";
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    http.end();
    otaStatusLine = "BIN HTTP " + String(code);
    return false;
  }

  int len = http.getSize();
  if (len <= 0) len = otaLatestSize;
  WiFiClient *stream = http.getStreamPtr();

  if (!Update.begin(len > 0 ? len : UPDATE_SIZE_UNKNOWN)) {
    http.end();
    otaStatusLine = "Update.begin failed";
    return false;
  }

  size_t written = Update.writeStream(*stream);
  if (written == 0) {
    Update.abort();
    http.end();
    otaStatusLine = "Write failed";
    return false;
  }

  if (!Update.end()) {
    http.end();
    otaStatusLine = "Update.end failed";
    return false;
  }

  if (!Update.isFinished()) {
    http.end();
    otaStatusLine = "Update not finished";
    return false;
  }

  http.end();
  otaStatusLine = "Update success, rebooting...";
  delay(600);
  ESP.restart();
  return true;
}

// ================= HTTP handlers =================
static void handleSaveWiFi() {
  String ssid = server.arg("ssid_manual").length() ? server.arg("ssid_manual") : server.arg("ssid");
  String pass = server.arg("password");

  if (ssid.length() && pass.length()) {
    cfg.wifi_ssid = ssid;
    cfg.wifi_pass = pass;
    saveConfig();
    server.send(200, "text/plain", "Saved! Rebooting...");
    delay(600);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing credentials");
  }
}

// Brightness with preview flag:
// - preview=1 : apply only (no flash write)
// - otherwise : apply + save to /config.json
static void handleBrightness() {
  if (!server.hasArg("value")) {
    server.send(400, "text/plain", "Missing value");
    return;
  }

  int b = clampInt(server.arg("value").toInt(), 3, 100);

  cfg.brightness = b;
  if (strip) {
    strip->setBrightness((uint8_t)cfg.brightness);
    strip->show();
  }

  bool preview = server.hasArg("preview");
  if (!preview) saveConfig();

  server.send(200, "text/plain", "OK");
}

static void handleAirport() {
  if (!server.hasArg("code")) {
    server.send(400, "text/plain", "Missing code");
    return;
  }

  cfg.airport_code = server.arg("code");
  cfg.airport_code.trim();
  cfg.airport_code.toUpperCase();

  saveConfig();

  restartMDNSForAirport();

  // --- FORCE METAR refresh if Wi-Fi is available ---
  if (WiFi.status() == WL_CONNECTED) {
    connected = true;                // ensure flag is correct
    fetchAndDisplayMETAR();
    lastMetarFetch = millis();
  }

  server.send(200, "text/plain", "OK");
}


static void handleSched() {
  if (!(server.hasArg("enabled") && server.hasArg("tz") && server.hasArg("start") && server.hasArg("end"))) {
    server.send(400, "text/plain", "Bad schedule request");
    return;
  }

  cfg.scheduleEnabled = (server.arg("enabled") == "on");
  cfg.timezonePref    = server.arg("tz");

  String s = server.arg("start"), e = server.arg("end");
  cfg.startHour   = s.substring(0, 2).toInt();
  cfg.startMinute = s.substring(3).toInt();
  cfg.endHour     = e.substring(0, 2).toInt();
  cfg.endMinute   = e.substring(3).toInt();

  saveConfig();

  setenv("TZ", cfg.timezonePref.c_str(), 1);
  tzset();

  if (cfg.scheduleEnabled && connected) {
    fetchAndDisplayMETAR();
    lastMetarFetch = millis();
  }

  server.send(200, "text/plain", "OK");
}

static void handleMode() {
  if (!server.hasArg("value")) {
    server.send(400, "text/plain", "Missing value");
    return;
  }

  String v = server.arg("value");
  if      (v == "auto")  displayMode = MODE_AUTO;
  else if (v == "vfr")   displayMode = MODE_VFR;
  else if (v == "mvfr")  displayMode = MODE_MVFR;
  else if (v == "ifr")   displayMode = MODE_IFR;
  else if (v == "lifr")  displayMode = MODE_LIFR;
  else if (v == "cycle") displayMode = MODE_CYCLE;
  else {
    server.send(400, "text/plain", "Invalid mode");
    return;
  }

  cfg.displayMode = (int)displayMode;
  saveConfig();

  applyModeColor();
  server.send(200, "text/plain", "OK");
}

// /flightpulse?enabled=on|off&tail=N247AP&hex=A24A0E
static void handleFlightPulse() {
  if (server.hasArg("enabled")) cfg.fpEnabled = (server.arg("enabled") == "on");

  String tailIn = server.hasArg("tail") ? server.arg("tail") : "";
  String hexIn  = server.hasArg("hex")  ? server.arg("hex")  : "";

  String resolved = resolveTailOrHex(tailIn, hexIn);
  if (resolved.length() != 6) {
    server.send(400, "text/plain", "ERR: Provide valid US Tail (N...) or valid 6-char ICAO hex");
    return;
  }

  tailIn.trim(); tailIn.toUpperCase();
  hexIn.trim();  hexIn.toUpperCase();

  if (tailIn.length()) cfg.fpTail = tailIn;
  else cfg.fpTail = "";

  cfg.fpIcao = resolved;
  saveConfig();

  server.send(200, "text/plain", "OK");
}

// OTA endpoints
static void handleOtaCheck() {
  bool ok = otaCheckNow();
  server.send(ok ? 200 : 500, "text/plain", otaStatusLine);
}

static void handleOtaInstall() {
  bool ok = otaCheckNow();
  if (!ok) { server.send(500, "text/plain", otaStatusLine); return; }
  if (!otaUpdateAvailable) { server.send(200, "text/plain", "No update available"); return; }
  server.send(200, "text/plain", "Starting update...");
  delay(100);
  otaInstallNow();
}

static void handleOtaSettings() {
  if (!server.hasArg("auto") || !server.hasArg("days")) {
    server.send(400, "text/plain", "Missing args");
    return;
  }
  cfg.otaAutoUpdate = (server.arg("auto") == "on");
  cfg.otaIntervalDays = clampInt(server.arg("days").toInt(), 1, 60);
  saveConfig();
  server.send(200, "text/plain", "OK");
}

// ================= Web UI =================
static void handleRoot() {
  struct tm tmnow;
  char timeBuf[9];
  if (getLocalTime(&tmnow)) sprintf(timeBuf, "%02d:%02d:%02d", tmnow.tm_hour, tmnow.tm_min, tmnow.tm_sec);
  else strcpy(timeBuf, "--:--:--");

  String startStr = (cfg.startHour < 10 ? "0" + String(cfg.startHour) : String(cfg.startHour));
  startStr += ":"; startStr += (cfg.startMinute < 10 ? "0" + String(cfg.startMinute) : String(cfg.startMinute));

  String endStr = (cfg.endHour < 10 ? "0" + String(cfg.endHour) : String(cfg.endHour));
  endStr += ":"; endStr += (cfg.endMinute < 10 ? "0" + String(cfg.endMinute) : String(cfg.endMinute));

  String fpStatus = "Disabled";
  if (cfg.fpEnabled) {
    fpStatus = fpIsFlying ? "Flying: YES" : "Flying: NO";
    if (!cfg.fpIcao.length()) fpStatus = "Enabled (no aircraft set)";
  }

  String mdnsHost = sanitizeAirportHost(cfg.airport_code);

  String page;
  page.reserve(24000);

  page += R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8"><title>North METAR Lamp</title>
<style>
body{font-family:Arial;background:#f2f2f2;color:#333;margin:0;}
header{background:#222;color:#fff;padding:1rem;text-align:center;position:relative;}
.container{padding:1rem;max-width:760px;margin:auto;}
.card{background:#fff;padding:1rem;margin-bottom:1rem;border-radius:8px;box-shadow:0 2px 6px rgba(0,0,0,0.1);}
label{font-weight:bold;display:block;margin-top:10px;}
input,select,button{width:100%;padding:8px;margin-top:5px;border:1px solid #ccc;border-radius:4px;}
.switch{position:relative;display:inline-block;width:50px;height:28px;}
.switch input{opacity:0;width:0;height:0;}
.slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#ccc;transition:.4s;border-radius:28px;}
.slider:before{position:absolute;content:"";height:22px;width:22px;left:3px;bottom:3px;background:#fff;transition:.4s;border-radius:50%;}
input:checked + .slider{background:#2196F3;}
input:checked + .slider:before{transform:translateX(22px);}
table td{padding:4px 8px;vertical-align:top;}
.mode-buttons{display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-top:10px;}
.mode-buttons button{font-size:0.95rem;}
.badge{display:inline-block;padding:3px 8px;border-radius:999px;background:#eee;font-size:12px;}
.small{font-size:0.9rem;color:#555;line-height:1.35;}
.row{display:flex;gap:10px;align-items:center;margin-top:5px;}
.row input[type=range]{flex:1;}
.row input[type=number]{width:110px;}
.row button{width:120px;}
.lock{position:absolute;right:12px;top:12px;font-size:20px;text-decoration:none;opacity:.9;}
</style>
</head><body>
<header>
  <h2>🛫 North METAR Lamp</h2>
  <a class="lock" href="/admin" target="_blank">🔒</a>
</header>
<div class="container">
)rawliteral";

  // METAR card
  page += R"rawliteral(<div class="card"><h3>📡 Current METAR & Time</h3><table>)rawliteral";
  page += "<tr><td>📶 AP SSID:</td><td>" + cfg.device_ssid + "</td></tr>";
  page += "<tr><td>🌐 mDNS:</td><td>http://" + mdnsHost + ".local</td></tr>";
  page += "<tr><td>⌚ Local Time:</td><td>" + String(timeBuf) + "</td></tr>";
  page += "<tr><td>📍 Station:</td><td>" + metar_station + "</td></tr>";
  page += "<tr><td>🕒 METAR Time:</td><td>" + metar_time + "</td></tr>";
  page += "<tr><td>🧭 Category:</td><td>" + flight_category + "</td></tr>";
  page += "<tr><td>💨 Wind:</td><td>" + metar_wind + "</td></tr>";
  page += "<tr><td>🌬️ Gusts:</td><td>" + metar_gust + "</td></tr>";
  page += "<tr><td>📏 Visibility:</td><td>" + metar_visibility + "</td></tr>";
  page += "<tr><td>🌡️ Temp:</td><td>" + metar_temp + "</td></tr>";
  page += "<tr><td>🧊 Dewpoint:</td><td>" + metar_dewpoint + "</td></tr>";
  page += "<tr><td>🧪 Pressure:</td><td>" + metar_pressure + "</td></tr>";
  page += R"rawliteral(</table></div>)rawliteral";

  // Station + Brightness
  page += R"rawliteral(<div class="card"><h3>Station & Brightness</h3>)rawliteral";
  page += R"rawliteral(<label>Airport Code:</label><input type="text" id="airportInput" value=")rawliteral";
  page += cfg.airport_code;
  page += R"rawliteral(">)rawliteral";
  page += R"rawliteral(<button id="airportBtn" type="button">✈️ Update Station</button>)rawliteral";

  page += R"rawliteral(<label>LED Brightness:</label>)rawliteral";
  page += R"rawliteral(<div class="row">)rawliteral";

  page += R"rawliteral(<input type="range" id="brightnessSlider" min="3" max="100" value=")rawliteral";
  page += String(cfg.brightness);
  page += R"rawliteral(">)rawliteral";

  page += R"rawliteral(<input type="number" id="brightnessNum" min="3" max="100" value=")rawliteral";
  page += String(cfg.brightness);
  page += R"rawliteral(">)rawliteral";

  page += R"rawliteral(<button id="brightnessSave" type="button">Save</button>)rawliteral";
  page += R"rawliteral(</div>)rawliteral";
  page += R"rawliteral(<div class="small">Drag/type previews brightness. Hit <b>Save</b> to store it.</div>)rawliteral";
  page += R"rawliteral(</div>)rawliteral";

  // Flight Pulse
  page += R"rawliteral(<div class="card"><h3>🛩️ Flight Pulse (Tail OR ICAO Hex)</h3>)rawliteral";
  page += "<p><span class='badge'>" + fpStatus + "</span></p>";

  page += R"rawliteral(<label>Enable Flight Pulse:</label>
<label class="switch"><input type="checkbox" id="fpToggle" )rawliteral";
  page += (cfg.fpEnabled ? "checked" : "");
  page += R"rawliteral(><span class="slider"></span></label>)rawliteral";

  page += R"rawliteral(<label>Tail Number (US N-number):</label>
<input type="text" id="fpTail" placeholder="e.g. N247AP" value=")rawliteral";
  page += cfg.fpTail;
  page += R"rawliteral(">)rawliteral";

  page += R"rawliteral(<label>OR ICAO Hex (6 chars):</label>
<input type="text" id="fpHex" maxlength="6" placeholder="e.g. A24A0E" value=")rawliteral";
  page += cfg.fpIcao;
  page += R"rawliteral(">)rawliteral";

  page += R"rawliteral(<div class="small">If Tail is filled, it converts to hex automatically. Otherwise Hex is used as-is.</div>
<button id="fpSaveBtn" type="button">💾 Save Flight Pulse</button>
</div>)rawliteral";

  // Schedule
  page += R"rawliteral(<div class="card"><h3>⏰ Schedule On/Off</h3>)rawliteral";
  page += R"rawliteral(<label>Enable Schedule:</label>)rawliteral";
  page += R"rawliteral(<label class="switch"><input type="checkbox" id="schedToggle" )rawliteral";
  page += (cfg.scheduleEnabled ? "checked" : "");
  page += R"rawliteral(><span class="slider"></span></label>)rawliteral";

  page += R"rawliteral(<label>Time Zone:</label>
<select id="tzSelect">
  <option value="UTC0">UTC</option>
  <option value="EST5EDT">America/New_York</option>
  <option value="CST6CDT">America/Chicago</option>
  <option value="MST7MDT">America/Denver</option>
  <option value="PST8PDT">America/Los_Angeles</option>
</select>)rawliteral";

  page += R"rawliteral(<label>Start Time:</label><input type="time" id="startTime" value=")rawliteral";
  page += startStr;
  page += R"rawliteral(">)rawliteral";

  page += R"rawliteral(<label>End Time:</label><input type="time" id="endTime" value=")rawliteral";
  page += endStr;
  page += R"rawliteral(">)rawliteral";

  page += R"rawliteral(<button id="scheduleBtn" type="button">💾 Save Schedule</button></div>)rawliteral";

  // Mode
  page += R"rawliteral(<div class="card"><h3>🎛️ Display Mode</h3>)rawliteral";
  page += "<p><strong>Current mode:</strong> <span class='badge'>" + modeToString() + "</span></p>";

  page += R"rawliteral(
<div class="mode-buttons">
  <button class="modeBtn" type="button" data-mode="auto">Auto (METAR)</button>
  <button class="modeBtn" type="button" data-mode="vfr">VFR</button>
  <button class="modeBtn" type="button" data-mode="mvfr">MVFR</button>
  <button class="modeBtn" type="button" data-mode="ifr">IFR</button>
  <button class="modeBtn" type="button" data-mode="lifr">LIFR</button>
  <button class="modeBtn" type="button" data-mode="cycle">Cycle Demo</button>
</div>
</div>
)rawliteral";

  // Wi-Fi setup
  page += R"rawliteral(<div class="card"><h3>Wi-Fi Setup</h3><form action="/save" method="POST">
<label>Select Wi-Fi Network:</label><select name="ssid">)rawliteral";

  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    page += "<option value='";
    page += WiFi.SSID(i);
    page += "'>";
    page += WiFi.SSID(i);
    page += "</option>";
  }

  page += R"rawliteral(</select>
<label>Or enter manually:</label>
<input type="text" name="ssid_manual" placeholder="SSID">
<label>Password:</label>
<input type="password" name="password" placeholder="Password">
<button type="submit">💾 Save Wi-Fi</button>
</form></div>)rawliteral";

  // OTA card
  page += R"rawliteral(<div class="card"><h3>⬆️ Firmware Update (OTA)</h3>)rawliteral";
  page += "<p><span class='badge'>" + otaStatusLine + "</span></p>";
  page += "<p class='small'>Current: " + String(FW_VERSION) + "</p>";
  page += "<p class='small'>Hardware: " + chipModelStr() + "</p>";
  page += "<p class='small'>Asset: " + String(otaAssetNameForThisChip()) + "</p>";
  page += "<p class='small'>Auto-update: " + (cfg.otaAutoUpdate ? String("ON") : String("OFF")) + "</p>";

  page += R"rawliteral(
<label>Enable Auto-Update:</label>
<label class="switch"><input type="checkbox" id="otaAuto" )rawliteral";
  page += (cfg.otaAutoUpdate ? "checked" : "");
  page += R"rawliteral(><span class="slider"></span></label>
<label>Check Interval (days):</label>
<input type="number" id="otaDays" min="1" max="60" value=")rawliteral";
  page += String(cfg.otaIntervalDays);
  page += R"rawliteral(">
<button id="otaSaveBtn" type="button">💾 Save OTA Settings</button>
<button id="otaCheckBtn" type="button">🔍 Check Now</button>
<button id="otaInstallBtn" type="button">⬇️ Install Update</button>
<p class="small">Install will reboot automatically.</p>
</div>)rawliteral";

  // JS
  page += R"rawliteral(</div>
<script>
(function(){
  // -------- brightness preview + save --------
  var slider = document.getElementById('brightnessSlider');
  var num    = document.getElementById('brightnessNum');
  var save   = document.getElementById('brightnessSave');
  var t;

  function clamp(v){
    v = parseInt(v, 10);
    if (isNaN(v)) v = 100;
    if (v < 3) v = 3;
    if (v > 100) v = 100;
    return v;
  }

  function preview(v){
    clearTimeout(t);
    t = setTimeout(function(){
      fetch('/brightness?value=' + encodeURIComponent(v) + '&preview=1');
    }, 150);
  }

  slider.oninput = function(){
    var v = clamp(this.value);
    num.value = v;
    preview(v);
  };

  num.oninput = function(){
    var v = clamp(this.value);
    slider.value = v;
    preview(v);
  };

  save.onclick = function(){
    var v = clamp(num.value);
    slider.value = v;
    num.value = v;
    fetch('/brightness?value=' + encodeURIComponent(v))
      .then(function(){ alert('Brightness saved'); });
  };

  // -------- airport --------
  document.getElementById('airportBtn').onclick = function() {
    var code = document.getElementById('airportInput').value.trim().toUpperCase();
    if (!code) return;
    fetch('/airport?code=' + encodeURIComponent(code)).then(function(){ location.reload(); });
  };

  // -------- schedule --------
  document.getElementById('tzSelect').value = ')rawliteral";
  page += cfg.timezonePref;
  page += R"rawliteral(';

  document.getElementById('scheduleBtn').onclick = function() {
    var enabled = document.getElementById('schedToggle').checked ? 'on':'off';
    var tz      = document.getElementById('tzSelect').value;
    var start   = document.getElementById('startTime').value;
    var end     = document.getElementById('endTime').value;

    var url = '/schedule?enabled=' + encodeURIComponent(enabled)
            + '&tz=' + encodeURIComponent(tz)
            + '&start=' + encodeURIComponent(start)
            + '&end=' + encodeURIComponent(end);

    fetch(url).then(function(){ alert('Schedule saved'); });
  };

  // -------- mode buttons --------
  var btns = document.querySelectorAll('.modeBtn');
  for (var i=0; i<btns.length; i++){
    btns[i].onclick = function(){
      var m = this.getAttribute('data-mode');
      fetch('/mode?value=' + encodeURIComponent(m)).then(function(){ location.reload(); });
    };
  }

  // -------- flight pulse --------
  var fpTailEl = document.getElementById('fpTail');
  var fpHexEl  = document.getElementById('fpHex');

  fpTailEl.oninput = function() {
    if (this.value.trim().length > 0) fpHexEl.value = '';
  };

  fpHexEl.oninput = function() {
    if (this.value.trim().length > 0) fpTailEl.value = '';
  };

  document.getElementById('fpSaveBtn').onclick = function(){
    var en = document.getElementById('fpToggle').checked ? 'on' : 'off';
    var tail = fpTailEl.value.trim().toUpperCase();
    var hex  = fpHexEl.value.trim().toUpperCase();

    var url = '/flightpulse?enabled=' + encodeURIComponent(en)
            + '&tail=' + encodeURIComponent(tail)
            + '&hex=' + encodeURIComponent(hex);

    fetch(url).then(function(resp){
      resp.text().then(function(txt){
        if (!resp.ok) alert(txt);
        else { alert('Flight Pulse saved'); location.reload(); }
      });
    });
  };

  // -------- OTA --------
  document.getElementById('otaCheckBtn').onclick = function(){
    fetch('/ota/check').then(function(){ location.reload(); });
  };

  document.getElementById('otaInstallBtn').onclick = function(){
    if (!confirm('Install update now? Device will reboot.')) return;
    fetch('/ota/install').then(function(resp){
      resp.text().then(function(t){ alert(t); });
    });
  };

  document.getElementById('otaSaveBtn').onclick = function(){
    var auto = document.getElementById('otaAuto').checked ? 'on':'off';
    var days = document.getElementById('otaDays').value;
    fetch('/ota/settings?auto=' + encodeURIComponent(auto) + '&days=' + encodeURIComponent(days))
      .then(function(){ alert('OTA settings saved'); location.reload(); });
  };
})();
</script>
</body></html>)rawliteral";

  server.send(200, "text/html", page);
}

// ================= Web server routes =================
void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSaveWiFi);

  server.on("/brightness", HTTP_GET, handleBrightness);
  server.on("/airport", HTTP_GET, handleAirport);
  server.on("/schedule", HTTP_GET, handleSched);
  server.on("/mode", HTTP_GET, handleMode);
  server.on("/flightpulse", HTTP_GET, handleFlightPulse);

  server.on("/ota/check", HTTP_GET, handleOtaCheck);
  server.on("/ota/install", HTTP_GET, handleOtaInstall);
  server.on("/ota/settings", HTTP_GET, handleOtaSettings);

  // Admin routes from AdminUI.h
  registerAdminRoutes();

  server.begin();
}

// ================= Arduino setup/loop =================
void setup() {
  serialAttachSafe();
  Serial.println("[APP] Boot");
  Serial.printf("[APP] FW_VERSION: %s\n", FW_VERSION);
  Serial.printf("[APP] CHIP: %s\n", ESP.getChipModel());          // <-- FIXED (no .c_str())
  Serial.printf("[APP] OTA ASSET: %s\n", otaAssetNameForThisChip());

  bool cfgOk = loadConfig();
  Serial.println(cfgOk ? "[APP] Config loaded" : "[APP] Config missing (defaults)");

  // clamp + normalize
  cfg.brightness = clampInt(cfg.brightness, 3, 100);
  cfg.airport_code.trim(); cfg.airport_code.toUpperCase();

  // LED order: normalize only (validity is handled in loadConfig() + initStripFromConfig())
  cfg.led_order.trim();
  cfg.led_order.toUpperCase();

  // init LED from cfg (pin/count/order)
  initStripFromConfig();
  if (strip) {
    strip->setBrightness((uint8_t)cfg.brightness);
    strip->show();
  }

  // restore mode
  int m = cfg.displayMode;
  if (m < (int)MODE_AUTO || m > (int)MODE_CYCLE) m = (int)MODE_AUTO;
  displayMode = (DisplayMode)m;

  // Wi-Fi AP+STA
  WiFi.mode(WIFI_MODE_APSTA);

  // Always-on SoftAP
  startSoftAPAlways();

  // STA connect (optional)
  connectToWiFi();

  // NTP/TZ
  configTime(0, 0, "pool.ntp.org");
  setenv("TZ", cfg.timezonePref.c_str(), 1);
  tzset();
  waitForTimeSync();

  // mDNS
  restartMDNSForAirport();

  // initial METAR
  if (WiFi.status() == WL_CONNECTED) {
    connected = true;
    fetchAndDisplayMETAR();
    lastMetarFetch = millis();
  } else {
    connected = false;
    applyModeColor();
  }

  setupWebServer();

  Serial.print("[SYS] STA IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("[SYS] AP  IP: ");
  Serial.println(WiFi.softAPIP());

  // OTA check on boot (no auto-install unless enabled)
  if (cfg.otaCheckOnBoot && connected) {
    otaCheckNow();
    Serial.printf("[OTA] %s\n", otaStatusLine.c_str());
    if (cfg.otaAutoUpdate && otaUpdateAvailable) {
      Serial.println("[OTA] Auto-update enabled: installing...");
      delay(300);
      otaInstallNow();
    }
  }
}

void loop() {
  server.handleClient();

  bool inSchedule = true;

  if (cfg.scheduleEnabled) {
    struct tm tmnow;
    if (getLocalTime(&tmnow)) {
      int cur  = tmnow.tm_hour * 60 + tmnow.tm_min;
      int smin = cfg.startHour * 60 + cfg.startMinute;
      int emin = cfg.endHour * 60 + cfg.endMinute;
      inSchedule = (smin <= emin) ? (cur >= smin && cur < emin) : (cur >= smin || cur < emin);
    }
  }

  // schedule / metar refresh
  if (cfg.scheduleEnabled) {
    if (!inSchedule) {
      clearLED();
      lastScheduleOn = false;
    } else {
      if (!lastScheduleOn || (connected && millis() - lastMetarFetch > fetchInterval)) {
        fetchAndDisplayMETAR();
        lastMetarFetch = millis();
      }
      lastScheduleOn = true;
    }
  } else if (connected && millis() - lastMetarFetch > fetchInterval) {
    fetchAndDisplayMETAR();
    lastMetarFetch = millis();
  }

  // cycle demo
  if ((!cfg.scheduleEnabled || inSchedule) && displayMode == MODE_CYCLE) {
    unsigned long now = millis();
    if (now - lastCycleSwitch > 3000) {
      lastCycleSwitch = now;
      cycleIndex = (cycleIndex + 1) % 4;
      applyModeColor();
    }
  }

  // flight pulse runtime
  if (cfg.fpEnabled && connected && cfg.fpIcao.length() == 6 && (!cfg.scheduleEnabled || inSchedule)) {
    unsigned long now = millis();

    if (now - fpLastCheckMs >= fpCheckIntervalMs) {
      fpLastCheckMs = now;

      bool flyingNow = fpFetchIsFlying_ADSBlol(cfg.fpIcao);

      if (flyingNow) fpFlyingStreak = min(fpFlyingStreak + 1, 3);
      else           fpFlyingStreak = max(fpFlyingStreak - 1, -3);

      bool flyingDebounced = (fpFlyingStreak >= 2);

      if (flyingDebounced != fpIsFlying) {
        fpIsFlying = flyingDebounced;
        if (fpIsFlying && !fpPulseActive) fpStartPulse();
        if (!fpIsFlying && fpPulseActive) fpStopPulseRestore();
      }
    }

    fpUpdatePulseOverlay();
  } else {
    if (fpPulseActive) fpStopPulseRestore();
  }

  // OTA periodic check in DAYS (only if auto-update enabled)
  if (connected && cfg.otaAutoUpdate) {
    unsigned long intervalMs = (unsigned long)cfg.otaIntervalDays * 24UL * 60UL * 60UL * 1000UL;
    if (intervalMs < 60000UL) intervalMs = 60000UL;

    if (otaLastCheckMs == 0) otaLastCheckMs = millis();

    if (millis() - otaLastCheckMs >= intervalMs) {
      otaCheckNow();
      Serial.printf("[OTA] Periodic check: %s\n", otaStatusLine.c_str());
      if (otaUpdateAvailable) {
        Serial.println("[OTA] Auto-update: installing...");
        delay(300);
        otaInstallNow();
      }
    }
  }

  delay(100);
}
