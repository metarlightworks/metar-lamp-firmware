// ============================================================
// METARLightworks MAP — APP Firmware (ESP32 / ESP32-C3 / ESP32-S3)
// ============================================================
// - Reads LittleFS /config.json (written by FACTORY firmware)
// - SoftAP is ALWAYS ON so web UI is always reachable
// - Also attempts STA Wi-Fi using config.wifi creds (AP+STA)
// - mDNS: metarmap.local (fixed)
// - Simple Map UI: comma list of ICAO + SKIP + legend tokens
// - 1 LED per token, max 250
// - Data source: aviationweather.gov (AWC Data API) METAR JSON + stationinfo JSON
// - Refresh interval: 20 minutes
// - Fallback: nearest airport within 75nm among configured airports; else dim white
// - OTA in app: Check Now + Install Update + Auto-update (days) like your Lamp app
// ============================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <ESP.h>
#include <math.h>

#include "AppTypes.h"
#include "AdminUI.h"
#include "version.h"

// ================= WEB =================
WebServer server(80);

// ================= Config =================
AppConfig cfg;

// ================= FS =================
static const char* CONFIG_PATH = "/config.json";

// ================= LED runtime =================
Adafruit_NeoPixel* strip = nullptr;

// ================= AWC endpoints =================
static const char* AWC_METAR_ENDPOINT   = "https://aviationweather.gov/api/data/metar";
static const char* AWC_STATION_ENDPOINT = "https://aviationweather.gov/api/data/stationinfo";

// ================= Map limits/timing =================
static const int MAX_TOKENS = 250;
static const unsigned long METAR_INTERVAL_MS = 20UL * 60UL * 1000UL;
static const float FALLBACK_RADIUS_NM = 75.0f;
static const size_t MAX_URL_LEN = 1700;

// ================= Runtime =================
bool connected = false;
unsigned long lastMetarFetch = 0;

// ================= OTA constants (MAP assets) =================
static const char* OTA_OWNER = "METARlightworks";
static const char* OTA_REPO  = "metar-lamp-firmware";
static const char* OTA_TAG_PREFIX = "map-v";

static const char* OTA_ASSET_ESP32      = "METARLightworks_Map_ESP32.bin";
static const char* OTA_ASSET_ESP32C3    = "METARLightworks_Map_ESP32C3.bin";
static const char* OTA_ASSET_ESP32S3M   = "METARLightworks_Map_ESP32S3_MATRIX.bin";

// ================= OTA runtime status =================
String otaLatestTag = "";
String otaLatestUrl = "";
int    otaLatestSize = 0;
String otaStatusLine = "Not checked yet";
bool   otaUpdateAvailable = false;
unsigned long otaLastCheckMs = 0;

// ================= Map token model =================
enum TokenType : uint8_t { TOK_AIRPORT, TOK_SKIP, TOK_LEGEND, TOK_INVALID };

struct Token {
  TokenType type = TOK_INVALID;
  String raw;      // upper token
  String icao;     // ICAO if airport
  bool hasGeo = false;
  float lat = 0;
  float lon = 0;

  bool hasMetar = false;
  String fltCat = "UNKNOWN";
};

static Token tokens[MAX_TOKENS];
static int tokenCount = 0;

// ------------------ Helpers ------------------
static String toUpperTrim(String s) { s.trim(); s.toUpperCase(); return s; }

static bool isValidICAO(const String& s) {
  if (s.length() != 4) return false;
  for (int i=0;i<4;i++){
    char c = s[i];
    if (!((c>='A'&&c<='Z') || (c>='0'&&c<='9'))) return false;
  }
  return true;
}

static int clampInt(int v,int lo,int hi){ if(v<lo) return lo; if(v>hi) return hi; return v; }

static uint16_t neoOrderFlagFromString(const String& sIn) {
  String s = sIn; s.trim(); s.toUpperCase();
  if (s=="RGB") return NEO_RGB;
  if (s=="RBG") return NEO_RBG;
  if (s=="GRB") return NEO_GRB;
  if (s=="GBR") return NEO_GBR;
  if (s=="BRG") return NEO_BRG;
  if (s=="BGR") return NEO_BGR;
  return NEO_GRB;
}

static float deg2rad(float deg){ return deg*(float)M_PI/180.0f; }
static float haversineNm(float lat1,float lon1,float lat2,float lon2){
  const float R_km=6371.0f;
  float dLat=deg2rad(lat2-lat1), dLon=deg2rad(lon2-lon1);
  float a=sinf(dLat/2)*sinf(dLat/2)+cosf(deg2rad(lat1))*cosf(deg2rad(lat2))*sinf(dLon/2)*sinf(dLon/2);
  float c=2.0f*atan2f(sqrtf(a),sqrtf(1.0f-a));
  return (R_km*c)*0.539957f;
}

static void colorForCategory(const String& cat, uint8_t& r, uint8_t& g, uint8_t& b) {
  String c = cat; c.trim(); c.toUpperCase();
  if (c=="VFR")  { r=0;   g=255; b=0;   return; }
  if (c=="MVFR") { r=0;   g=0;   b=255; return; }
  if (c=="IFR")  { r=255; g=0;   b=0;   return; }
  if (c=="LIFR") { r=255; g=0;   b=255; return; }
  r=12; g=12; b=12; // unknown -> dim white
}

static bool isProvisionedForMap() {
  if (!cfg.provisioned) return false;
  String a = cfg.app_role; a.trim(); a.toLowerCase();
  return (a == "map");
}

// ------------------ LED ------------------
void clearLED() {
  if (!strip) return;
  strip->clear();
  strip->show();
}
void setLEDColor(uint8_t r,uint8_t g,uint8_t b) {
  if (!strip) return;
  for (int i=0;i<strip->numPixels();i++) strip->setPixelColor(i, strip->Color(r,g,b));
  strip->setBrightness((uint8_t)clampInt(cfg.brightness, 1, 255));
  strip->show();
}

void rebuildStripFromConfig() {
  // derive LED count from tokens
  cfg.led_count = clampInt(tokenCount, 1, MAX_TOKENS);

  if (strip) { delete strip; strip=nullptr; }
  uint16_t order = neoOrderFlagFromString(cfg.led_order);
  strip = new Adafruit_NeoPixel(cfg.led_count, cfg.led_pin, order + NEO_KHZ800);
  strip->begin();
  strip->setBrightness((uint8_t)clampInt(cfg.brightness,1,255));
  strip->clear();
  strip->show();
}

// ------------------ Wi-Fi + mDNS (like Lamp) ------------------
static void setupWiFi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(cfg.device_ssid.length()?cfg.device_ssid.c_str():"METARMap", "metarmap123"); // always on

  if (cfg.wifi_ssid.length() > 0) {
    WiFi.begin(cfg.wifi_ssid.c_str(), cfg.wifi_pass.c_str());
  }
}

void restartMDNSFixed() {
  MDNS.end();
  if (MDNS.begin("metarmap")) {
    MDNS.addService("http", "tcp", 80);
  }
}

// ------------------ Config load/save (ArduinoJson v6-safe) ------------------
static bool loadConfig() {
  if (!LittleFS.begin(true)) return false;
  if (!LittleFS.exists(CONFIG_PATH)) return false;

  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return false;

  DynamicJsonDocument doc(24 * 1024);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  cfg.device_ssid = doc["device_ssid"] | "METARMap";

  // wifi
  cfg.wifi_ssid = doc["wifi"]["ssid"] | "";
  cfg.wifi_pass = doc["wifi"]["pass"] | "";

  // map list
  cfg.map_list = doc["map_list"] | "VFR,MVFR,IFR,LIFR,SKIP";

  // brightness (support both root + led.brightness)
  cfg.brightness = (int)(doc["brightness"] | 120);
  int lb = (int)(doc["led"]["brightness"] | 0);
  if (lb > 0) cfg.brightness = lb;
  cfg.brightness = clampInt(cfg.brightness, 1, 255);

  // led settings
  cfg.led_pin = (int)(doc["led"]["pin"] | 5);
  cfg.led_order = doc["led"]["order"] | "GRB";
  cfg.led_order = toUpperTrim(cfg.led_order);

  // provision stamp (Factory writes device.provisioned + device.app)
  cfg.provisioned = (bool)(doc["device"]["provisioned"] | false);
  cfg.app_role    = String((const char*)(doc["device"]["app"] | ""));

  // OTA prefs
  JsonObject ota = doc["ota"].as<JsonObject>();
  if (!ota.isNull()) {
    cfg.otaCheckOnBoot  = (bool)(ota["check_on_boot"] | true);
    cfg.otaAutoUpdate   = (bool)(ota["auto_update"] | false);
    cfg.otaIntervalDays = (int) (ota["interval_days"] | 7);
  }
  cfg.otaIntervalDays = clampInt(cfg.otaIntervalDays, 1, 60);

  return true;
}

bool saveConfig() {
  if (!LittleFS.begin(true)) return false;

  DynamicJsonDocument doc(24 * 1024);

  if (LittleFS.exists(CONFIG_PATH)) {
    File in = LittleFS.open(CONFIG_PATH, "r");
    if (in) {
      DeserializationError err = deserializeJson(doc, in);
      in.close();
      if (err) doc.clear();
    }
  }

  // Keep existing keys; only set/update what Map needs
  doc["device_ssid"] = cfg.device_ssid;

  doc["wifi"]["ssid"] = cfg.wifi_ssid;
  doc["wifi"]["pass"] = cfg.wifi_pass;

  doc["map_list"] = cfg.map_list;

  doc["brightness"] = cfg.brightness;            // keep root compatible
  doc["led"]["pin"] = cfg.led_pin;
  doc["led"]["order"] = cfg.led_order;
  doc["led"]["brightness"] = cfg.brightness;

  // Leave provision stamp as-is (Factory owns it)
  // doc["device"]["provisioned"] / ["app"] untouched

  JsonObject ota = doc["ota"].to<JsonObject>();
  ota["check_on_boot"] = cfg.otaCheckOnBoot;
  ota["auto_update"]   = cfg.otaAutoUpdate;
  ota["interval_days"] = cfg.otaIntervalDays;

  File out = LittleFS.open(CONFIG_PATH, "w");
  if (!out) return false;
  serializeJson(doc, out);
  out.close();
  return true;
}

// ------------------ Token parsing ------------------
static TokenType classifyToken(const String& t) {
  if (t=="SKIP") return TOK_SKIP;
  if (t=="VFR" || t=="MVFR" || t=="IFR" || t=="LIFR") return TOK_LEGEND;
  if (isValidICAO(t)) return TOK_AIRPORT;
  return TOK_INVALID;
}

static void parseTokenList(const String& list) {
  tokenCount = 0;

  String w = list;
  w.replace("\n", ",");
  w.replace("\r", ",");
  w.replace(";", ",");

  int start = 0;
  while (start < (int)w.length() && tokenCount < MAX_TOKENS) {
    int comma = w.indexOf(',', start);
    if (comma < 0) comma = w.length();

    String t = toUpperTrim(w.substring(start, comma));
    if (t.length()) {
      Token tok;
      tok.raw = t;
      tok.type = classifyToken(t);
      if (tok.type == TOK_AIRPORT) tok.icao = t;
      tokens[tokenCount++] = tok;
    }
    start = comma + 1;
  }

  cfg.led_count = clampInt(tokenCount, 1, MAX_TOKENS);
}

// ------------------ HTTPS GET ------------------
static bool httpsGET(const String& url, String& outBody, int& outCode) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("METARLightworks-Map/1.0 ESP32");

  if (!http.begin(client, url)) { outCode = -1; outBody = ""; return false; }
  outCode = http.GET();
  outBody = (outCode > 0) ? http.getString() : "";
  http.end();
  return (outCode == 200);
}

// ------------------ Station Geo (batch) ------------------
static bool buildNextIdsChunk(int& cursor, String& outIdsCsv) {
  outIdsCsv = "";

  while (cursor < tokenCount && tokens[cursor].type != TOK_AIRPORT) cursor++;
  if (cursor >= tokenCount) return false;

  String base = String(AWC_STATION_ENDPOINT) + "?format=json&ids=";
  String ids = "";

  for (int i = cursor; i < tokenCount; i++) {
    if (tokens[i].type != TOK_AIRPORT) continue;
    if (tokens[i].hasGeo) continue;

    String next = tokens[i].icao;
    String cand = ids;
    if (cand.length()) cand += ",";
    cand += next;

    if ((base + cand).length() >= MAX_URL_LEN) break;
    ids = cand;
    cursor = i + 1;
  }

  outIdsCsv = ids;
  return ids.length() > 0;
}

static void applyStationInfoGeo(const String& json) {
  DynamicJsonDocument doc(64 * 1024);
  if (deserializeJson(doc, json)) return;
  if (!doc.is<JsonArray>()) return;

  for (JsonVariant v : doc.as<JsonArray>()) {
    if (!v.is<JsonObject>()) continue;
    JsonObject o = v.as<JsonObject>();

    String id = "";
    if (o.containsKey("icaoId")) id = (const char*)o["icaoId"];
    else if (o.containsKey("station")) id = (const char*)o["station"];
    id = toUpperTrim(id);
    if (!isValidICAO(id)) continue;

    bool ok=false;
    float lat=0, lon=0;
    if (o.containsKey("latitude") && o.containsKey("longitude")) {
      lat = o["latitude"].as<float>();
      lon = o["longitude"].as<float>();
      ok=true;
    } else if (o.containsKey("lat") && o.containsKey("lon")) {
      lat = o["lat"].as<float>();
      lon = o["lon"].as<float>();
      ok=true;
    }
    if (!ok) continue;

    for (int i=0;i<tokenCount;i++) {
      if (tokens[i].type==TOK_AIRPORT && tokens[i].icao==id) {
        tokens[i].hasGeo=true; tokens[i].lat=lat; tokens[i].lon=lon;
        break;
      }
    }
  }
}

static void ensureGeoForAirports() {
  // any missing?
  bool missing=false;
  for (int i=0;i<tokenCount;i++){
    if (tokens[i].type==TOK_AIRPORT && !tokens[i].hasGeo) { missing=true; break; }
  }
  if (!missing) return;

  int cursor=0;
  while (cursor < tokenCount) {
    String idsCsv;
    if (!buildNextIdsChunk(cursor, idsCsv)) break;

    String url = String(AWC_STATION_ENDPOINT) + "?format=json&ids=" + idsCsv;
    String body; int code=0;
    if (httpsGET(url, body, code) && body.length()) applyStationInfoGeo(body);

    delay(120);
    yield();
  }
}

// ------------------ METAR fetch (batch) ------------------
static bool buildNextMetarChunk(int& cursor, String& outIdsCsv) {
  outIdsCsv = "";

  while (cursor < tokenCount && tokens[cursor].type != TOK_AIRPORT) cursor++;
  if (cursor >= tokenCount) return false;

  String base = String(AWC_METAR_ENDPOINT) + "?format=json&ids=";
  String ids = "";

  for (int i = cursor; i < tokenCount; i++) {
    if (tokens[i].type != TOK_AIRPORT) continue;

    String next = tokens[i].icao;
    String cand = ids;
    if (cand.length()) cand += ",";
    cand += next;

    if ((base + cand).length() >= MAX_URL_LEN) break;
    ids = cand;
    cursor = i + 1;
  }

  outIdsCsv = ids;
  return ids.length() > 0;
}

static void clearMetarState() {
  for (int i=0;i<tokenCount;i++){
    tokens[i].hasMetar=false;
    tokens[i].fltCat="UNKNOWN";
  }
}

static void applyMetarResults(const String& json) {
  DynamicJsonDocument doc(96 * 1024);
  if (deserializeJson(doc, json)) return;
  if (!doc.is<JsonArray>()) return;

  for (JsonVariant v : doc.as<JsonArray>()) {
    if (!v.is<JsonObject>()) continue;
    JsonObject o = v.as<JsonObject>();

    String id="";
    if (o.containsKey("icaoId")) id=(const char*)o["icaoId"];
    else if (o.containsKey("station")) id=(const char*)o["station"];
    id = toUpperTrim(id);
    if (!isValidICAO(id)) continue;

    String cat="UNKNOWN";
    if (o.containsKey("fltCat")) cat=(const char*)o["fltCat"];
    else if (o.containsKey("flight_category")) cat=(const char*)o["flight_category"];
    cat = toUpperTrim(cat);

    for (int i=0;i<tokenCount;i++){
      if (tokens[i].type==TOK_AIRPORT && tokens[i].icao==id) {
        tokens[i].hasMetar=true;
        tokens[i].fltCat=cat;
        break;
      }
    }
  }
}

// ------------------ Fallback + Render ------------------
static bool hasValidCat(const Token& t) {
  if (!t.hasMetar) return false;
  String c=t.fltCat; c.trim(); c.toUpperCase();
  return (c=="VFR"||c=="MVFR"||c=="IFR"||c=="LIFR");
}

static int findNearestFallbackIndex(int src) {
  const Token& s = tokens[src];
  if (!s.hasGeo) return -1;

  float best=1e9f;
  int bestIdx=-1;

  for (int i=0;i<tokenCount;i++){
    if (i==src) continue;
    const Token& c = tokens[i];
    if (c.type!=TOK_AIRPORT) continue;
    if (!c.hasGeo) continue;
    if (!hasValidCat(c)) continue;

    float d = haversineNm(s.lat,s.lon,c.lat,c.lon);
    if (d<=FALLBACK_RADIUS_NM && d<best) { best=d; bestIdx=i; }
  }
  return bestIdx;
}

static void renderMap() {
  if (!strip) return;
  strip->clear();

  for (int i=0;i<cfg.led_count;i++){
    if (i>=tokenCount) {
      strip->setPixelColor(i, strip->Color(12,12,12));
      continue;
    }

    const Token& t=tokens[i];

    if (t.type==TOK_SKIP) {
      // off
      continue;
    }

    if (t.type==TOK_LEGEND) {
      uint8_t r,g,b; colorForCategory(t.raw,r,g,b);
      strip->setPixelColor(i, strip->Color(r,g,b));
      continue;
    }

    if (t.type==TOK_AIRPORT) {
      if (hasValidCat(t)) {
        uint8_t r,g,b; colorForCategory(t.fltCat,r,g,b);
        strip->setPixelColor(i, strip->Color(r,g,b));
      } else {
        int fb = findNearestFallbackIndex(i);
        if (fb>=0) {
          uint8_t r,g,b; colorForCategory(tokens[fb].fltCat,r,g,b);
          strip->setPixelColor(i, strip->Color(r,g,b));
        } else {
          strip->setPixelColor(i, strip->Color(12,12,12)); // dim white
        }
      }
    }
  }

  strip->setBrightness((uint8_t)clampInt(cfg.brightness,1,255));
  strip->show();
}

// ------------------ Refresh ------------------
void refreshNow() {
  if (!isProvisionedForMap()) return;

  parseTokenList(cfg.map_list);
  rebuildStripFromConfig();
  renderMap(); // legends/skips immediately

  ensureGeoForAirports();

  clearMetarState();

  int cursor=0;
  while (cursor < tokenCount) {
    String idsCsv;
    if (!buildNextMetarChunk(cursor, idsCsv)) break;

    String url = String(AWC_METAR_ENDPOINT) + "?format=json&ids=" + idsCsv;
    String body; int code=0;
    if (httpsGET(url, body, code) && body.length()) applyMetarResults(body);

    delay(120);
    yield();
  }

  renderMap();
}

// ------------------ OTA (copied workflow from Lamp, Map assets) ------------------
static String chipModelStr() { return String(ESP.getChipModel()); }

const char* otaAssetNameForThisChip() {
  String m = chipModelStr();
  m.toUpperCase();
  if (m.indexOf("S3") >= 0) return OTA_ASSET_ESP32S3M;
  if (m.indexOf("C3") >= 0) return OTA_ASSET_ESP32C3;
  return OTA_ASSET_ESP32;
}

static bool otaGetLatest(String &outTag, String &outUrl, int &outSize) {
  outTag=""; outUrl=""; outSize=0;

  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "METARLightworks-Map");
  http.addHeader("Accept", "application/vnd.github+json");

  // NOTE: We DO NOT use /releases/latest because Lamp owns "latest".
  // We list releases and pick newest tag starting with "map-v".
  String api = String("https://api.github.com/repos/") + OTA_OWNER + "/" + OTA_REPO + "/releases?per_page=20";
  if (!http.begin(client, api)) return false;

  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  String body = http.getString();
  http.end();

  DynamicJsonDocument doc(64 * 1024);
  if (deserializeJson(doc, body)) return false;
  if (!doc.is<JsonArray>()) return false;

  const char* desiredAsset = otaAssetNameForThisChip();

  for (JsonVariant rv : doc.as<JsonArray>()) {
    if (!rv.is<JsonObject>()) continue;
    JsonObject rel = rv.as<JsonObject>();

    String tag = String((const char*)(rel["tag_name"] | ""));
    if (!tag.startsWith(OTA_TAG_PREFIX)) continue;

    JsonArray assets = rel["assets"].as<JsonArray>();
    if (assets.isNull()) continue;

    for (JsonObject a : assets) {
      String name = String((const char*)(a["name"] | ""));
      if (name == String(desiredAsset)) {
        outTag  = tag;
        outUrl  = String((const char*)(a["browser_download_url"] | ""));
        outSize = (int)(a["size"] | 0);
        return (outUrl.length() > 0 && outSize > 0);
      }
    }
    // If tag matched but asset missing, keep searching older map releases
  }

  return false;
}

bool otaCheckNow() {
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

  otaLatestTag  = tag;     // expected like "map-v0.1.0"
  otaLatestUrl  = url;
  otaLatestSize = size;

  // Current version string must match the tag format
  String cur = String(OTA_TAG_PREFIX) + String(FW_VERSION);  // "map-v" + "0.1.0" => "map-v0.1.0"

  if (otaLatestTag.length() && otaLatestTag != cur) {
    otaUpdateAvailable = true;
    otaStatusLine = "Update available: " + otaLatestTag + " (current " + cur + ")";
  } else {
    otaUpdateAvailable = false;
    otaStatusLine = "Up to date (" + cur + ")";
  }

  otaLastCheckMs = millis();
  return true;
}

void otaInstallNow() {
  if (!otaUpdateAvailable || otaLatestUrl.length()==0) { otaStatusLine="No update available"; return; }
  if (WiFi.status()!=WL_CONNECTED) { otaStatusLine="No Wi-Fi"; return; }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  otaStatusLine="Downloading...";
  if (!http.begin(client, otaLatestUrl)) { otaStatusLine="HTTP begin failed"; return; }

  int code=http.GET();
  if (code!=200) { otaStatusLine="BIN HTTP " + String(code); http.end(); return; }

  int len=http.getSize();
  if (len<=0) len=otaLatestSize;

  WiFiClient* stream=http.getStreamPtr();

  if (!Update.begin((len>0)?(size_t)len:UPDATE_SIZE_UNKNOWN)) { otaStatusLine="Update.begin failed"; http.end(); return; }

  size_t written=Update.writeStream(*stream);
  if (written==0) { otaStatusLine="Write failed"; Update.abort(); http.end(); return; }

  if (!Update.end()) { otaStatusLine="Update.end failed"; http.end(); return; }
  if (!Update.isFinished()) { otaStatusLine="Update not finished"; http.end(); return; }

  http.end();
  otaStatusLine="Update success, rebooting...";
  delay(400);
  ESP.restart();
}

void otaMaybeAutoCheck() {
  if (!cfg.otaAutoUpdate) return;
  if (WiFi.status()!=WL_CONNECTED) return;

  unsigned long interval = (unsigned long)cfg.otaIntervalDays * 24UL * 60UL * 60UL * 1000UL;
  if (interval < 12UL * 60UL * 60UL * 1000UL) interval = 12UL * 60UL * 60UL * 1000UL; // guard

  if (otaLastCheckMs == 0 || (millis() - otaLastCheckMs) > interval) {
    bool ok = otaCheckNow();
    if (ok && otaUpdateAvailable) otaInstallNow();
  }
}

// ------------------ Web server ------------------
static void setupWebServer() {
  registerRoutes();  // from AdminUI.h
  server.begin();
}

// ------------------ setup/loop ------------------
void setup() {
  Serial.begin(115200);
  delay(250);

  // load config
  if (!loadConfig()) {
    // still boot AP+UI
    cfg.device_ssid = "METARMap";
    cfg.map_list = "VFR,MVFR,IFR,LIFR,SKIP";
    cfg.brightness = 120;
    cfg.led_pin = 5;
    cfg.led_order = "GRB";
    cfg.provisioned = false;
    cfg.app_role = "";
  }

  // parse tokens now so LED count is correct even before metar
  parseTokenList(cfg.map_list);

  setupWiFi();
  restartMDNSFixed();
  setupWebServer();

  rebuildStripFromConfig();
  renderMap(); // show legends/skips immediately

  connected = (WiFi.status() == WL_CONNECTED);

  // Boot refresh if provisioned
  if (isProvisionedForMap()) {
    refreshNow();
    lastMetarFetch = millis();
  } else {
    lastMetarFetch = millis();
  }

  // OTA check on boot (same workflow)
  if (cfg.otaCheckOnBoot && (WiFi.status() == WL_CONNECTED)) {
    otaCheckNow();
  }
}

void loop() {
  server.handleClient();

  connected = (WiFi.status() == WL_CONNECTED);

  if (isProvisionedForMap()) {
    // periodic metar refresh
    if (millis() - lastMetarFetch > METAR_INTERVAL_MS) {
      lastMetarFetch = millis();
      refreshNow();
    }
  }

  otaMaybeAutoCheck();
  delay(2);
}