// ============================================================
// METARLightworks - FACTORY Firmware (ESP32 + ESP32-C3 + ESP32-S3)
// ============================================================
// Purpose:
// 1) Connect to your bench Wi-Fi (from secrets.h)
// 2) Write /config.json to LittleFS with provisioning stamp + app selection
// 3) Query GitHub Releases "latest" for your repo
// 4) Auto-select correct APP bin by chip model AND selection:
//      - Lamp: METARLightworks_App_*.bin
//      - Map : METARLightworks_Map_*.bin
// 5) Download + OTA flash, then reboot into APP firmware
//
// Tabs needed:
// - METARLightworks_Factory.ino  (this file)
// - secrets.h
// - version.h
// ============================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESP.h>

#include "version.h"
#include "secrets.h"

// ---------------------------
// Constants
// ---------------------------
static const char*   CONFIG_PATH = "/config.json";
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

// ---------------------------
// Forward declarations (Arduino-proof)
// ---------------------------
static void   serialAttachSafe();
static String deviceSSID();
static String macSuffix4();

static bool   saveFactoryConfig();
static bool   connectFactoryWiFi();

static const char* appAssetNameForThisChip();
static const char* appNameString();

static bool   getLatestAsset(const char* desiredName, String &outUrl, int &outSize, String &outTag);
static bool   otaFromUrl(const String &url, int expectedSize);

// ---------------------------
// Serial
// ---------------------------
static void serialAttachSafe() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 5000) {
    delay(10);
  }
  Serial.println();
}

// ---------------------------
// Device SSID + MAC suffix
// ---------------------------
static String deviceSSID() {
  // ORDER_NUMBER MUST be a string in secrets.h, e.g. #define ORDER_NUMBER "040"
  uint64_t mac = ESP.getEfuseMac();
  uint16_t macTail = (uint16_t)(mac & 0xFFFF);

  char buf[40];
  snprintf(buf, sizeof(buf), "METARLightworks%s-%04X", ORDER_NUMBER, macTail);
  return String(buf);
}

static String macSuffix4() {
  uint64_t mac = ESP.getEfuseMac();
  char macSuffix[5];
  snprintf(macSuffix, sizeof(macSuffix), "%04X", (uint16_t)(mac & 0xFFFF));
  return String(macSuffix);
}

// ---------------------------
// App selection helpers
// ---------------------------
static const char* appNameString() {
  // 1 = lamp, 2 = map
  return (FACTORY_APP_SELECTION == 2) ? "map" : "lamp";
}

// ---------------------------
// Choose APP asset by chip + app selection
// ---------------------------
static const char* appAssetNameForThisChip() {
  String m = ESP.getChipModel();
  m.toUpperCase();

  const bool wantMap = (FACTORY_APP_SELECTION == 2);

  if (wantMap) {
    if (m.indexOf("S3") >= 0) return MAP_ASSET_ESP32S3M;
    if (m.indexOf("C3") >= 0) return MAP_ASSET_ESP32C3;
    return MAP_ASSET_ESP32;
  } else {
    if (m.indexOf("S3") >= 0) return APP_ASSET_ESP32S3M;
    if (m.indexOf("C3") >= 0) return APP_ASSET_ESP32C3;
    return APP_ASSET_ESP32;
  }
}

// ---------------------------
// Config write (LittleFS)
// ---------------------------
static bool saveFactoryConfig() {
  // Keep this small & stable
  StaticJsonDocument<1536> doc;

  // Core identity
  String ssid = deviceSSID();
  doc["device_ssid"]  = ssid;                 // METARLightworks<ORDER>-<MAC4>
  doc["order_number"] = String(ORDER_NUMBER); // e.g. "041"
  doc["mac_suffix"]   = macSuffix4();         // e.g. "7A3C"

  // Provisioning + app selection (THIS is the “simple safeguard”)
  JsonObject device = doc.createNestedObject("device");
  device["provisioned"]   = true;
  device["app"]           = appNameString();          // "lamp" or "map"
  device["app_selection"] = (int)FACTORY_APP_SELECTION; // 1 or 2

  // Only write AVWX token for LAMP devices
  if (FACTORY_APP_SELECTION != 2) {
    doc["avwx_token"] = String(AVWX_TOKEN);
    doc["airport"]    = "KTIX";
    doc["brightness"] = 120;
  } else {
    // Map device defaults can be minimal
    doc["brightness"] = 120;
    // Map list can be empty; Map APP can supply defaults if blank
    doc["map_list"]   = "VFR,MVFR,IFR,LIFR,SKIP";
  }

  // Wi-Fi creds (bench/staging) — your App can later let user change these
  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["ssid"] = String(FACTORY_WIFI_SSID);
  wifi["pass"] = String(FACTORY_WIFI_PASSWORD);

  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) return false;

  if (serializeJson(doc, f) == 0) {
    f.close();
    return false;
  }
  f.close();
  return true;
}

// ---------------------------
// Wi-Fi connect
// ---------------------------
static bool connectFactoryWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  Serial.printf("[FACTORY] Connecting Wi-Fi: %s\n", FACTORY_WIFI_SSID);
  WiFi.begin(FACTORY_WIFI_SSID, FACTORY_WIFI_PASSWORD);

  unsigned long start = millis();
  while (millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("[FACTORY] Connected, IP: ");
      Serial.println(WiFi.localIP());
      return true;
    }
    delay(200);
  }

  Serial.println("[FACTORY] Wi-Fi connect timeout");
  return false;
}

// ---------------------------
// GitHub latest release lookup
// ---------------------------
static bool getLatestAsset(const char* desiredName, String &outUrl, int &outSize, String &outTag) {
  WiFiClientSecure client;
  client.setInsecure(); // simplest; OK for now

  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(12000);

  String api = String("https://api.github.com/repos/") + GITHUB_OWNER + "/" + GITHUB_REPO + "/releases/latest";
  Serial.printf("[FACTORY] GitHub latest: %s\n", api.c_str());

  if (!http.begin(client, api)) {
    Serial.println("[FACTORY] HTTP begin failed (latest release)");
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[FACTORY] GitHub latest release HTTP %d\n", code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  StaticJsonDocument<16384> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.print("[FACTORY] JSON parse failed: ");
    Serial.println(err.c_str());
    return false;
  }

  outTag = String(doc["tag_name"] | "");

  JsonArray assets = doc["assets"].as<JsonArray>();
  if (assets.isNull()) {
    Serial.println("[FACTORY] No assets array in release JSON");
    return false;
  }

  for (JsonObject a : assets) {
    String name = String(a["name"] | "");
    if (name == String(desiredName)) {
      outUrl  = String(a["browser_download_url"] | "");
      outSize = (int)(a["size"] | 0);

      Serial.printf("[FACTORY] Found asset: %s\n", name.c_str());
      Serial.printf("[FACTORY] URL: %s\n", outUrl.c_str());
      Serial.printf("[FACTORY] Size: %d\n", outSize);

      return (outUrl.length() > 0 && outSize > 0);
    }
  }

  Serial.printf("[FACTORY] Asset not found in latest release: %s\n", desiredName);
  return false;
}

// ---------------------------
// OTA flash from URL
// ---------------------------
static bool otaFromUrl(const String &url, int expectedSize) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(20000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  Serial.println("[FACTORY] Starting OTA download...");
  if (!http.begin(client, url)) {
    Serial.println("[FACTORY] HTTP begin failed (bin)");
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[FACTORY] BIN HTTP %d\n", code);
    http.end();
    return false;
  }

  int len = http.getSize();
  if (len <= 0) len = expectedSize;

  WiFiClient *stream = http.getStreamPtr();

  if (!Update.begin(len)) {
    Serial.printf("[FACTORY] Update.begin failed, err=%d\n", Update.getError());
    http.end();
    return false;
  }

  size_t written = Update.writeStream(*stream);
  if (written == 0) {
    Serial.printf("[FACTORY] Update.writeStream wrote 0, err=%d\n", Update.getError());
    Update.abort();
    http.end();
    return false;
  }

  if (!Update.end()) {
    Serial.printf("[FACTORY] Update.end failed, err=%d\n", Update.getError());
    http.end();
    return false;
  }

  if (!Update.isFinished()) {
    Serial.println("[FACTORY] Update not finished");
    http.end();
    return false;
  }

  http.end();
  Serial.println("[FACTORY] OTA success!");
  return true;
}

// ---------------------------
// Arduino entry points
// ---------------------------
void setup() {
  serialAttachSafe();
  Serial.print("[FACTORY] FW: ");
  Serial.println(FW_VERSION);

  Serial.print("[FACTORY] Chip: ");
  Serial.println(ESP.getChipModel());

  Serial.print("[FACTORY] Device SSID: ");
  Serial.println(deviceSSID());

  Serial.print("[FACTORY] App selection: ");
  Serial.println(appNameString());

  const char* desiredAsset = appAssetNameForThisChip();
  Serial.print("[FACTORY] Desired app asset: ");
  Serial.println(desiredAsset);

  // FS
  if (!LittleFS.begin(true)) {
    Serial.println("[FACTORY] LittleFS mount FAILED");
  } else {
    Serial.println("[FACTORY] LittleFS mounted");
    if (saveFactoryConfig()) {
      Serial.println("[FACTORY] Wrote /config.json");

      File f = LittleFS.open(CONFIG_PATH, "r");
      if (f) {
        Serial.println("[FACTORY] /config.json contents:");
        while (f.available()) Serial.write(f.read());
        Serial.println();
        f.close();
      }
    } else {
      Serial.println("[FACTORY] Failed to write /config.json");
    }
  }

  // Wi-Fi
  if (!connectFactoryWiFi()) {
    Serial.println("[FACTORY] Cannot proceed without Wi-Fi");
    return;
  }

  // GitHub + OTA
  String url, tag;
  int size = 0;

  if (!getLatestAsset(desiredAsset, url, size, tag)) {
    Serial.println("[FACTORY] Could not locate latest app asset");
    return;
  }

  Serial.printf("[FACTORY] Latest release: %s\n", tag.c_str());

  if (otaFromUrl(url, size)) {
    Serial.println("[FACTORY] Rebooting into APP...");
    delay(800);
    ESP.restart();
  } else {
    Serial.println("[FACTORY] OTA failed; staying in FACTORY firmware");
  }
}

void loop() {
  delay(1000);
}