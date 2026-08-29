/*
  METAR Lightworks - RGBCW Bulb App
  Version: 0.2.2 PRODUCTION / SAFE-MODE ARCH

  PORT BASIS
  ----------
  User-facing behavior is ported from METARLightworks App 1.11.9 onto the
  sealed-bulb-proven v0.1.6 ESP32-C3 foundation.

  Bulb-specific rules:
    - IoTorero / Athom ESP32-C3 12W RGBCW
    - proven physical PWM map:
        RED        GPIO3
        GREEN      GPIO4
        BLUE       GPIO5
        COOL WHITE GPIO6
        WARM WHITE GPIO7
    - direct analogWrite() PWM path retained from the proven v0.1.6 build
    - Preferences/NVS only; NO LittleFS
    - Production never installs firmware directly; updates are handed to Safe Mode
    - NO runway feature
    - Night Light is always available; there is no enable/disable feature flag
    - NO device-specific Flight Pulse privacy/suppression test
    - AVWX is primary when a key is configured
    - AviationWeather.gov is automatic fallback (and works with no AVWX key)

  PRODUCTION / RECOVERY ARCHITECTURE
  ----------------------------------
  Production runs from prod (ota_0) at 0x010000.
  Safe Mode permanently occupies safe (factory) at 0x1D0000.
  Production never writes the safe partition. Firmware updates and recovery
  are requested through NVS, then Production deliberately boots Safe Mode.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <time.h>
#include <math.h>

extern "C" {
  #include "esp_ota_ops.h"
  #include "esp_partition.h"
  #include "esp_system.h"
  #include "esp_err.h"
}

static const char *FW_VERSION = "METAR-BULB-APP-0.2.2-PRODUCTION-SAFE-ARCH";
static const char *PREF_NAMESPACE = "ktixbasic";  // preserves v0.1.6 ssid/pass
static const char *AWC_USER_AGENT = "METARLightworks-Bulb/0.2.4a";

// Permanent production/safe-mode contract.
static const uint32_t PROD_OFFSET = 0x010000UL;
static const uint32_t PROD_SIZE   = 0x1C0000UL;
static const uint32_t SAFE_OFFSET = 0x1D0000UL;
static const uint32_t SAFE_SIZE   = 0x1C0000UL;

static const char *SAFE_REQUEST_KEY = "safe_req";
static const char *BOOT_COUNT_KEY   = "boot_count";
static const char *SAFE_REQ_INSTALL = "install_latest";
static const char *SAFE_REQ_RECOVERY = "recovery";

static const uint8_t RAPID_BOOT_THRESHOLD = 7;
static const unsigned long HEALTHY_BOOT_MS = 30UL * 1000UL;

static const char *PRODUCTION_REPO =
  "metarlightworks/metar-bulb-firmware";
static const char *PRODUCTION_ASSET =
  "METARLightworks_Bulb_ESP32C3.bin";

static const unsigned long METAR_INTERVAL_MS = 20UL * 60UL * 1000UL;
static const unsigned long WIFI_RETRY_MS     = 30UL * 1000UL;
static const unsigned long FP_CHECK_MS       = 45UL * 1000UL;
static const unsigned long STATE_CHECK_MS    = 1000UL;

static const int BRIGHT_MIN = 5;
static const int BRIGHT_MAX = 100;

// Bulb hardware profiles.
// Production factory default is E27. If NVS key "bulb_model" is absent,
// Production MUST behave as E27.
//
// E27 12W: R6 G7 B5 CW3 WW4
// GU10 5W: R3 G4 B5 CW6 WW7
enum BulbModel {
  BULB_E27 = 0,
  BULB_GU10 = 1
};

static BulbModel bulbModel = BULB_E27;
static uint8_t PIN_R  = 6;
static uint8_t PIN_G  = 7;
static uint8_t PIN_B  = 5;
static uint8_t PIN_CW = 3;
static uint8_t PIN_WW = 4;

static String bulbModelName() {
  return bulbModel == BULB_GU10 ? "GU10" : "E27";
}

static String bulbPwmMapText() {
  if (bulbModel == BULB_GU10) return "R3 G4 B5 CW6 WW7";
  return "R6 G7 B5 CW3 WW4";
}

static void applyBulbModelPins() {
  if (bulbModel == BULB_GU10) {
    PIN_R = 3; PIN_G = 4; PIN_B = 5; PIN_CW = 6; PIN_WW = 7;
  } else {
    bulbModel = BULB_E27;
    PIN_R = 6; PIN_G = 7; PIN_B = 5; PIN_CW = 3; PIN_WW = 4;
  }
}

// Admin credentials preserved from App 1.11.9.
// These are intentionally simple for the current development build.
static const char *ADMIN_USER = "north";
static const char *ADMIN_PASS = "metar";

WebServer server(80);
DNSServer dnsServer;
Preferences prefs;
static const byte DNS_PORT = 53;

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

struct AppConfig {
  String deviceSsid;
  String avwxToken;
  String airport = "KTIX";

  int brightness = 30;

  bool scheduleEnabled = false;
  int startHour = 0;
  int startMinute = 0;
  int endHour = 23;
  int endMinute = 59;
  String timezonePref = "UTC0";

  int displayMode = 0;  // 0 auto, 1 VFR, 2 MVFR, 3 IFR, 4 LIFR, 5 cycle

  bool fpEnabled = false;
  String fpIcao;
  String fpTail;

  // Night Light is always a product feature.
  bool nightManualOn = false;
  bool nightSchedEnabled = false;
  int nightStartHour = 20;
  int nightStartMinute = 0;
  int nightEndHour = 6;
  int nightEndMinute = 0;
  int nightBrightness = 15;
  int nightWarmth = 60;  // 0 = cool, 100 = warm
  bool nightPulseWithFlight = false;
};

AppConfig cfg;

// Factory/S3/Safe Mode NVS contract:
//   Preferences namespace: "ktixbasic"
//   provisioned customer values:
//     ssid
//     pass
//     avwx
//   recovery coordination:
//     safe_req   = "install_latest" | "recovery" | ""
//     boot_count = uint8_t rapid/failed-production-boot counter
//
// Production consumes ssid/pass/avwx directly from this same namespace and
// never erases customer configuration when entering Safe Mode.

// -----------------------------------------------------------------------------
// Runtime state
// -----------------------------------------------------------------------------

enum DisplayMode {
  MODE_AUTO = 0,
  MODE_VFR,
  MODE_MVFR,
  MODE_IFR,
  MODE_LIFR,
  MODE_CYCLE
};

DisplayMode displayMode = MODE_AUTO;

static bool setupApRunning = false;
static bool wifiApplyPending = false;
static unsigned long wifiApplyAtMs = 0;
static unsigned long lastWifiAttemptMs = 0;
static bool mdnsStarted = false;
static String mdnsHost = "ktix";

static bool initialFetchPending = true;
static unsigned long lastMetarFetchAttemptMs = 0;
static unsigned long lastMetarFetchSuccessMs = 0;
static uint32_t metarFetchOk = 0;
static uint32_t metarFetchFail = 0;
static int lastHttpCode = 0;
static int lastAvwxHttpCode = 0;
static int lastAwcHttpCode = 0;
static String metarSource = "None";
static String metarError = "Not fetched yet";

static String flightCategory = "UNKNOWN";
static String rawMetar;
static String metarStation;
static String metarTime;
static String metarWind;
static String metarGust;
static String metarVisibility;
static String metarTemp;
static String metarDewpoint;
static String metarPressure;
static long metarObsEpoch = 0;

static bool currentInSchedule = true;
static bool currentNightActive = false;
static unsigned long lastStateCheckMs = 0;
static unsigned long lastCycleSwitchMs = 0;
static int cycleIndex = 0;

// Flight Pulse
static bool fpIsFlying = false;
static int fpFlyingStreak = 0;
static unsigned long fpLastCheckMs = 0;
static int fpLastHttpCode = 0;
static bool fpPulseActive = false;
static unsigned long fpPulseStartMs = 0;
static const float FP_PERIOD_MS = 3500.0f;
static const float FP_MIN_FRACTION = 0.15f;

// Production / Safe Mode recovery state
static unsigned long rebootAtMs = 0;
static bool prefsReady = false;
static bool bootHealthMarked = false;
static uint8_t bootCountThisBoot = 0;
static String lastSafeAction = "None";

// Bulb output
struct BulbChannels {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t cw = 0;
  uint8_t ww = 0;
};

// Explicit prototypes are intentional. Arduino's .ino preprocessor can
// otherwise auto-generate these before BulbChannels is declared.
static void renderChannels(const BulbChannels &ch, int brightnessPct);
static void setBaseOutput(const BulbChannels &ch, int brightnessPct,
                          const String &label);

static bool bulbPwmReady = false;
static BulbChannels baseChannels;
static int baseBrightnessPct = 0;
static int renderedBrightnessPct = 0;
static String outputMode = "BOOT";
static bool adminLedTestActive = false;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static String htmlEscape(const String &s) {
  String o;
  o.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&': o += F("&amp;"); break;
      case '<': o += F("&lt;"); break;
      case '>': o += F("&gt;"); break;
      case '"': o += F("&quot;"); break;
      case '\'': o += F("&#39;"); break;
      default: o += c; break;
    }
  }
  return o;
}

static String jsonEscape(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\b': o += "\\b"; break;
      case '\f': o += "\\f"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[7];
          snprintf(b, sizeof(b), "\\u%04X", c);
          o += b;
        } else {
          o += (char)c;
        }
        break;
    }
  }
  return o;
}

static String uptimeText() {
  uint64_t sec = millis() / 1000ULL;
  uint32_t days = sec / 86400ULL;
  sec %= 86400ULL;
  uint32_t hrs = sec / 3600ULL;
  sec %= 3600ULL;
  uint32_t mins = sec / 60ULL;
  uint32_t secs = sec % 60ULL;

  char b[48];
  if (days) snprintf(b, sizeof(b), "%ud %02u:%02u:%02u", days, hrs, mins, secs);
  else snprintf(b, sizeof(b), "%02u:%02u:%02u", hrs, mins, secs);
  return String(b);
}

static String ageText(unsigned long thenMs) {
  if (!thenMs) return "never";
  unsigned long sec = (millis() - thenMs) / 1000UL;
  if (sec < 60) return String(sec) + " sec ago";
  unsigned long min = sec / 60UL;
  if (min < 60) return String(min) + " min ago";
  return String(min / 60UL) + " hr " + String(min % 60UL) + " min ago";
}

static String resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

static String partLabel(const esp_partition_t *p) {
  if (!p) return "none";
  if (p->label[0]) return String(p->label);
  char b[20];
  snprintf(b, sizeof(b), "0x%06lX", (unsigned long)p->address);
  return String(b);
}

static const esp_partition_t *findProdPartition() {
  const esp_partition_t *p =
    esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                             ESP_PARTITION_SUBTYPE_APP_OTA_0,
                             "prod");
  if (!p) return nullptr;
  if (p->address != PROD_OFFSET || p->size != PROD_SIZE) return nullptr;
  return p;
}

static const esp_partition_t *findSafePartition() {
  const esp_partition_t *p =
    esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                             ESP_PARTITION_SUBTYPE_APP_FACTORY,
                             "safe");
  if (!p) return nullptr;
  if (p->address != SAFE_OFFSET || p->size != SAFE_SIZE) return nullptr;
  return p;
}

static bool partitionArchitectureValid() {
  return findProdPartition() != nullptr && findSafePartition() != nullptr;
}

static bool writeSafeRequest(const char *request) {
  Preferences recoveryPrefs;
  if (!recoveryPrefs.begin(PREF_NAMESPACE, false)) {
    Serial.println("[SAFE] NVS open failed");
    return false;
  }

  size_t wrote = recoveryPrefs.putString(SAFE_REQUEST_KEY, request);
  recoveryPrefs.end();

  if (!wrote) {
    Serial.println("[SAFE] NVS request write failed");
    return false;
  }
  return true;
}

static void resetBootCounterBestEffort() {
  Preferences recoveryPrefs;
  if (!recoveryPrefs.begin(PREF_NAMESPACE, false)) return;
  recoveryPrefs.putUChar(BOOT_COUNT_KEY, 0);
  recoveryPrefs.end();
  bootCountThisBoot = 0;
}

static void clearSafeRequestBestEffort() {
  Preferences recoveryPrefs;
  if (!recoveryPrefs.begin(PREF_NAMESPACE, false)) return;
  recoveryPrefs.remove(SAFE_REQUEST_KEY);
  recoveryPrefs.end();
}

static bool selectSafeModeBoot() {
  const esp_partition_t *safe = findSafePartition();
  if (!safe) {
    Serial.println("[SAFE] safe/factory partition missing or layout mismatch");
    return false;
  }

  esp_err_t err = esp_ota_set_boot_partition(safe);
  if (err != ESP_OK) {
    Serial.printf("[SAFE] esp_ota_set_boot_partition(safe) failed: %s\n",
                  esp_err_to_name(err));
    return false;
  }

  Serial.printf("[SAFE] next boot -> %s @ 0x%06lX\n",
                partLabel(safe).c_str(),
                (unsigned long)safe->address);
  return true;
}

static bool mlwRequestSafeMode(const char *request, const char *reason) {
  Serial.printf("[SAFE] request=%s reason=%s\n", request, reason);

  // Store the command before changing boot selection so Safe Mode can read it.
  if (!writeSafeRequest(request)) {
    lastSafeAction = "NVS request failed";
    return false;
  }

  if (!selectSafeModeBoot()) {
    // Avoid leaving a stale install/recovery request that might execute during
    // some unrelated future Safe Mode entry. Keep the boot counter intact so
    // repeated failed/crashing Production boots can try recovery again.
    clearSafeRequestBestEffort();
    lastSafeAction = "Safe partition selection failed";
    return false;
  }

  // Only clear the rapid-boot counter after Safe Mode has been successfully
  // selected as the next boot target.
  resetBootCounterBestEffort();

  lastSafeAction = String(request) + " requested";
  return true;
}

static bool mlwRequestSafeInstallLatest() {
  return mlwRequestSafeMode(SAFE_REQ_INSTALL, "admin firmware update");
}

static bool mlwRequestSafeRecovery() {
  return mlwRequestSafeMode(SAFE_REQ_RECOVERY, "admin recovery");
}

static void mlwEarlyRecoveryBootCheck() {
  // Run this at the very beginning of setup(), before bulb/network/METAR/UI
  // startup. Preferences/NVS survives real power removal.
  Preferences recoveryPrefs;
  if (!recoveryPrefs.begin(PREF_NAMESPACE, false)) {
    Serial.println("[RECOVERY] early NVS open failed; boot counter unavailable");
    return;
  }

  uint8_t count = recoveryPrefs.getUChar(BOOT_COUNT_KEY, 0);
  if (count < 255) count++;

  recoveryPrefs.putUChar(BOOT_COUNT_KEY, count);
  bootCountThisBoot = count;
  recoveryPrefs.end();

  Serial.printf("[RECOVERY] production boot_count=%u/%u\n",
                (unsigned)count,
                (unsigned)RAPID_BOOT_THRESHOLD);

  if (count < RAPID_BOOT_THRESHOLD) return;

  Serial.println("[RECOVERY] threshold reached -> Safe Mode");

  // If Safe Mode selection succeeds, clear the counter before reboot so a
  // later return to Production starts a fresh recovery window.
  if (mlwRequestSafeMode(SAFE_REQ_RECOVERY, "seven rapid/failed production boots")) {
    delay(60);
    ESP.restart();
    while (true) delay(1000);
  }

  Serial.println("[RECOVERY] Safe Mode entry failed; continuing Production");
}

static void serviceHealthyBootMarker() {
  if (bootHealthMarked || !prefsReady) return;
  if (millis() < HEALTHY_BOOT_MS) return;

  prefs.putUChar(BOOT_COUNT_KEY, 0);
  bootCountThisBoot = 0;
  bootHealthMarked = true;
  Serial.println("[RECOVERY] healthy 30s mark -> boot_count=0");
}

static String sanitizeAirport(String code) {
  code.trim();
  code.toUpperCase();
  String out;
  for (size_t i = 0; i < code.length(); i++) {
    char c = code[i];
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out += c;
  }
  if (out.length() < 3 || out.length() > 4) return "";
  return out;
}

static String hostForAirport(String code) {
  code = sanitizeAirport(code);
  code.toLowerCase();
  if (!code.length()) code = "metarlightworks";
  return code;
}

static String defaultDeviceSsid() {
  uint64_t mac = ESP.getEfuseMac();
  uint32_t suffix = (uint32_t)(mac & 0xFFFFFFULL);
  char b[40];
  snprintf(b, sizeof(b), "METARLightworks-%06lX", (unsigned long)suffix);
  return String(b);
}

static String timeString(int h, int m) {
  char b[6];
  snprintf(b, sizeof(b), "%02d:%02d", h, m);
  return String(b);
}

static bool parseTimeArg(const String &s, int &h, int &m) {
  if (s.length() != 5 || s[2] != ':') return false;
  int hh = s.substring(0, 2).toInt();
  int mm = s.substring(3, 5).toInt();
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
  h = hh;
  m = mm;
  return true;
}

static bool adminAuth() {
  if (server.authenticate(ADMIN_USER, ADMIN_PASS)) return true;
  server.requestAuthentication();
  return false;
}

// -----------------------------------------------------------------------------
// NVS configuration
// -----------------------------------------------------------------------------

static void sanitizeConfig() {
  cfg.airport = sanitizeAirport(cfg.airport);
  if (!cfg.airport.length()) cfg.airport = "KTIX";

  cfg.brightness = clampInt(cfg.brightness, BRIGHT_MIN, BRIGHT_MAX);
  cfg.displayMode = clampInt(cfg.displayMode, 0, 5);

  cfg.fpIcao.trim();
  cfg.fpIcao.toUpperCase();
  cfg.fpTail.trim();
  cfg.fpTail.toUpperCase();

  cfg.nightBrightness = clampInt(cfg.nightBrightness, BRIGHT_MIN, BRIGHT_MAX);
  cfg.nightWarmth = clampInt(cfg.nightWarmth, 0, 100);

  if (!cfg.deviceSsid.length()) cfg.deviceSsid = defaultDeviceSsid();
}

static void loadConfig() {
  String model = prefs.getString("bulb_model", "E27");
  model.trim();
  model.toUpperCase();
  bulbModel = (model == "GU10") ? BULB_GU10 : BULB_E27;
  applyBulbModelPins();

  cfg.deviceSsid = prefs.getString("apname", "");
  cfg.avwxToken = prefs.getString("avwx", "");
  cfg.airport = prefs.getString("airport", "KTIX");
  cfg.brightness = prefs.getInt("bright", 30);

  cfg.scheduleEnabled = prefs.getBool("schedEn", false);
  cfg.startHour = prefs.getInt("sh", 0);
  cfg.startMinute = prefs.getInt("sm", 0);
  cfg.endHour = prefs.getInt("eh", 23);
  cfg.endMinute = prefs.getInt("em", 59);
  cfg.timezonePref = prefs.getString("tz", "UTC0");

  cfg.displayMode = prefs.getInt("mode", 0);

  cfg.fpEnabled = prefs.getBool("fpEn", false);
  cfg.fpIcao = prefs.getString("fpIcao", "");
  cfg.fpTail = prefs.getString("fpTail", "");

  cfg.nightManualOn = prefs.getBool("nMan", false);
  cfg.nightSchedEnabled = prefs.getBool("nSched", false);
  cfg.nightStartHour = prefs.getInt("nsh", 20);
  cfg.nightStartMinute = prefs.getInt("nsm", 0);
  cfg.nightEndHour = prefs.getInt("neh", 6);
  cfg.nightEndMinute = prefs.getInt("nem", 0);
  cfg.nightBrightness = prefs.getInt("nBright", 15);
  cfg.nightWarmth = prefs.getInt("nWarm", 60);
  cfg.nightPulseWithFlight = prefs.getBool("nPulse", false);

  sanitizeConfig();

  // Persist generated AP name once so it stays stable.
  if (!prefs.isKey("apname")) prefs.putString("apname", cfg.deviceSsid);
}

static void saveConfig() {
  sanitizeConfig();

  prefs.putString("bulb_model", bulbModelName());
  prefs.putString("apname", cfg.deviceSsid);
  prefs.putString("avwx", cfg.avwxToken);
  prefs.putString("airport", cfg.airport);
  prefs.putInt("bright", cfg.brightness);

  prefs.putBool("schedEn", cfg.scheduleEnabled);
  prefs.putInt("sh", cfg.startHour);
  prefs.putInt("sm", cfg.startMinute);
  prefs.putInt("eh", cfg.endHour);
  prefs.putInt("em", cfg.endMinute);
  prefs.putString("tz", cfg.timezonePref);

  prefs.putInt("mode", cfg.displayMode);

  prefs.putBool("fpEn", cfg.fpEnabled);
  prefs.putString("fpIcao", cfg.fpIcao);
  prefs.putString("fpTail", cfg.fpTail);

  prefs.putBool("nMan", cfg.nightManualOn);
  prefs.putBool("nSched", cfg.nightSchedEnabled);
  prefs.putInt("nsh", cfg.nightStartHour);
  prefs.putInt("nsm", cfg.nightStartMinute);
  prefs.putInt("neh", cfg.nightEndHour);
  prefs.putInt("nem", cfg.nightEndMinute);
  prefs.putInt("nBright", cfg.nightBrightness);
  prefs.putInt("nWarm", cfg.nightWarmth);
  prefs.putBool("nPulse", cfg.nightPulseWithFlight);
}

// -----------------------------------------------------------------------------
// Bulb PWM
// -----------------------------------------------------------------------------

static uint16_t scaleChannel(uint8_t value, int brightnessPct) {
  brightnessPct = clampInt(brightnessPct, 0, 100);
  return (uint16_t)(((uint32_t)value * (uint32_t)brightnessPct) / 100UL);
}

static void renderChannels(const BulbChannels &ch, int brightnessPct) {
  if (!bulbPwmReady) return;

  renderedBrightnessPct = clampInt(brightnessPct, 0, 100);

  analogWrite(PIN_R,  scaleChannel(ch.r,  renderedBrightnessPct));
  analogWrite(PIN_G,  scaleChannel(ch.g,  renderedBrightnessPct));
  analogWrite(PIN_B,  scaleChannel(ch.b,  renderedBrightnessPct));
  analogWrite(PIN_CW, scaleChannel(ch.cw, renderedBrightnessPct));
  analogWrite(PIN_WW, scaleChannel(ch.ww, renderedBrightnessPct));
}

static void setBaseOutput(const BulbChannels &ch, int brightnessPct,
                          const String &label) {
  baseChannels = ch;
  baseBrightnessPct = clampInt(brightnessPct, 0, 100);
  outputMode = label;

  if (!fpPulseActive) renderChannels(baseChannels, baseBrightnessPct);
}

static void setBaseRGB(uint8_t r, uint8_t g, uint8_t b, int brightnessPct,
                       const String &label) {
  BulbChannels ch;
  ch.r = r;
  ch.g = g;
  ch.b = b;
  setBaseOutput(ch, brightnessPct, label);
}

static void setBaseWhiteBlend(int warmth, int brightnessPct,
                              const String &label) {
  warmth = clampInt(warmth, 0, 100);

  // Dedicated RGBCW hardware: use only the two white channels for Night Light.
  // This avoids driving RGB + white at high output simultaneously.
  BulbChannels ch;
  ch.cw = (uint8_t)roundf(255.0f * ((100 - warmth) / 100.0f));
  ch.ww = (uint8_t)roundf(255.0f * (warmth / 100.0f));
  setBaseOutput(ch, brightnessPct, label);
}

static void clearBulb(const String &label = "OFF") {
  BulbChannels ch;
  setBaseOutput(ch, 0, label);
}

static void initBulb() {
  applyBulbModelPins();

  pinMode(PIN_R, OUTPUT);
  pinMode(PIN_G, OUTPUT);
  pinMode(PIN_B, OUTPUT);
  pinMode(PIN_CW, OUTPUT);
  pinMode(PIN_WW, OUTPUT);

  analogWrite(PIN_R, 0);
  analogWrite(PIN_G, 0);
  analogWrite(PIN_B, 0);
  analogWrite(PIN_CW, 0);
  analogWrite(PIN_WW, 0);

  bulbPwmReady = true;

  // Familiar dim white during startup, using both dedicated whites gently.
  BulbChannels ch;
  ch.cw = 100;
  ch.ww = 100;
  setBaseOutput(ch, 15, "BOOT / WAITING");
}

// -----------------------------------------------------------------------------
// Time / schedule / display
// -----------------------------------------------------------------------------

static void applyTimezone() {
  configTzTime(cfg.timezonePref.c_str(), "pool.ntp.org", "time.nist.gov");
}

static bool scheduleWindow(bool enabled, int sh, int sm, int eh, int em) {
  if (!enabled) return true;

  struct tm tmnow;
  if (!getLocalTime(&tmnow, 10)) {
    // If time is not available yet, do not unexpectedly turn the lamp off.
    return true;
  }

  int cur = tmnow.tm_hour * 60 + tmnow.tm_min;
  int start = sh * 60 + sm;
  int end = eh * 60 + em;

  return (start <= end) ? (cur >= start && cur < end)
                        : (cur >= start || cur < end);
}

static bool computeNightActive() {
  if (cfg.nightManualOn) return true;

  if (!cfg.nightSchedEnabled) return false;

  struct tm tmnow;
  if (!getLocalTime(&tmnow, 10)) return false;

  int cur = tmnow.tm_hour * 60 + tmnow.tm_min;
  int start = cfg.nightStartHour * 60 + cfg.nightStartMinute;
  int end = cfg.nightEndHour * 60 + cfg.nightEndMinute;

  return (start <= end) ? (cur >= start && cur < end)
                        : (cur >= start || cur < end);
}

static String modeToString() {
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

static void applyPrimaryColor() {
  if (adminLedTestActive) return;

  if (currentNightActive) {
    setBaseWhiteBlend(cfg.nightWarmth, cfg.nightBrightness, "NIGHT LIGHT");
    return;
  }

  if (!currentInSchedule) {
    clearBulb("SCHEDULE OFF");
    return;
  }

  if (displayMode == MODE_VFR) {
    setBaseRGB(0, 255, 0, cfg.brightness, "MANUAL VFR");
    return;
  }
  if (displayMode == MODE_MVFR) {
    setBaseRGB(0, 0, 255, cfg.brightness, "MANUAL MVFR");
    return;
  }
  if (displayMode == MODE_IFR) {
    setBaseRGB(255, 0, 0, cfg.brightness, "MANUAL IFR");
    return;
  }
  if (displayMode == MODE_LIFR) {
    setBaseRGB(255, 0, 255, cfg.brightness, "MANUAL LIFR");
    return;
  }

  if (displayMode == MODE_CYCLE) {
    switch (cycleIndex) {
      case 0: setBaseRGB(0, 255, 0, cfg.brightness, "CYCLE VFR"); break;
      case 1: setBaseRGB(0, 0, 255, cfg.brightness, "CYCLE MVFR"); break;
      case 2: setBaseRGB(255, 0, 0, cfg.brightness, "CYCLE IFR"); break;
      default:setBaseRGB(255, 0, 255, cfg.brightness, "CYCLE LIFR"); break;
    }
    return;
  }

  if (flightCategory == "VFR") {
    setBaseRGB(0, 255, 0, cfg.brightness, "METAR VFR");
  } else if (flightCategory == "MVFR") {
    setBaseRGB(0, 0, 255, cfg.brightness, "METAR MVFR");
  } else if (flightCategory == "IFR") {
    setBaseRGB(255, 0, 0, cfg.brightness, "METAR IFR");
  } else if (flightCategory == "LIFR") {
    setBaseRGB(255, 0, 255, cfg.brightness, "METAR LIFR");
  } else {
    BulbChannels ch;
    ch.cw = 80;
    ch.ww = 80;
    setBaseOutput(ch, 15, "NO VALID METAR");
  }
}

static void refreshDisplayState(bool force = false) {
  bool inSched = scheduleWindow(cfg.scheduleEnabled,
                                cfg.startHour, cfg.startMinute,
                                cfg.endHour, cfg.endMinute);
  bool night = computeNightActive();

  if (force || inSched != currentInSchedule || night != currentNightActive) {
    currentInSchedule = inSched;
    currentNightActive = night;
    applyPrimaryColor();
  }
}

// -----------------------------------------------------------------------------
// Wi-Fi / AP / mDNS
// -----------------------------------------------------------------------------

static void stopMdns() {
  if (!mdnsStarted) return;
  MDNS.end();
  mdnsStarted = false;
}

static void startMdns() {
  if (WiFi.status() != WL_CONNECTED) return;

  stopMdns();

  mdnsHost = hostForAirport(cfg.airport);
  if (MDNS.begin(mdnsHost.c_str())) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
    Serial.printf("[MDNS] http://%s.local/\n", mdnsHost.c_str());
  }
}

static void startSetupAp() {
  if (setupApRunning) return;

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);

  if (!WiFi.softAP(cfg.deviceSsid.c_str())) {
    Serial.println("[WIFI] setup AP start failed");
    return;
  }

  setupApRunning = true;
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  Serial.printf("[WIFI] setup AP '%s' ip=%s\n",
                cfg.deviceSsid.c_str(),
                WiFi.softAPIP().toString().c_str());
}

static void stopSetupAp() {
  if (!setupApRunning) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  setupApRunning = false;
  Serial.println("[WIFI] setup AP stopped");
}

static void beginSavedWiFi() {
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");

  WiFi.mode(setupApRunning ? WIFI_AP_STA : WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  lastWifiAttemptMs = millis();

  if (ssid.length()) {
    Serial.printf("[WIFI] connect saved '%s'\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    // Preserve the v0.1.6 / recovery behavior: try credentials already stored
    // by the ESP32 Wi-Fi stack before opening setup AP.
    Serial.println("[WIFI] trying existing ESP32 Wi-Fi credentials");
    WiFi.begin();
  }
}

static void serviceWiFi() {
  static wl_status_t previous = WiFi.status();
  wl_status_t now = WiFi.status();

  if (now == WL_CONNECTED && previous != WL_CONNECTED) {
    Serial.printf("[WIFI] CONNECTED ssid='%s' ip=%s RSSI=%d\n",
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());

    stopSetupAp();
    startMdns();
    applyTimezone();
    initialFetchPending = true;
  }

  if (now != WL_CONNECTED && previous == WL_CONNECTED) {
    Serial.println("[WIFI] DISCONNECTED");
    stopMdns();
  }

  previous = now;

  if (wifiApplyPending && millis() >= wifiApplyAtMs) {
    wifiApplyPending = false;
    WiFi.disconnect();
    delay(50);
    beginSavedWiFi();
  }

  if (now != WL_CONNECTED) {
    if (!setupApRunning && millis() > 15000UL) startSetupAp();

    if (!wifiApplyPending &&
        millis() - lastWifiAttemptMs >= WIFI_RETRY_MS) {
      beginSavedWiFi();
    }
  }
}

// -----------------------------------------------------------------------------
// METAR parsing / AVWX primary / AviationWeather.gov fallback
// -----------------------------------------------------------------------------

static void clearMetarDetailFields() {
  metarStation = cfg.airport;
  metarTime = "N/A";
  metarWind = "N/A";
  metarGust = "N/A";
  metarVisibility = "N/A";
  metarTemp = "N/A";
  metarDewpoint = "N/A";
  metarPressure = "N/A";
}

static bool parseAvwx(const String &json) {
  DynamicJsonDocument doc(6144);
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    metarError = String("AVWX JSON: ") + err.c_str();
    return false;
  }

  String station = doc["station"] | "";
  String rules = doc["flight_rules"] | "";
  String raw = doc["raw"] | "";

  station.trim();
  station.toUpperCase();
  rules.trim();
  rules.toUpperCase();
  raw.trim();

  if (station.length() && station != cfg.airport) {
    metarError = "AVWX station mismatch: " + station;
    return false;
  }

  if (!(rules == "VFR" || rules == "MVFR" ||
        rules == "IFR" || rules == "LIFR")) {
    metarError = "AVWX missing flight_rules";
    return false;
  }

  flightCategory = rules;
  rawMetar = raw;
  metarStation = station.length() ? station : cfg.airport;
  metarTime = String((const char *)(doc["time"]["dt"] | ""));

  if (doc["wind_direction"]["value"].isNull()) {
    metarWind = "N/A";
  } else {
    int wd = doc["wind_direction"]["value"].as<int>();
    int ws = doc["wind_speed"]["value"] | 0;
    metarWind = String(wd) + "° at " + String(ws) + " kt / " +
                String(ws * 1.15078f, 1) + " mph";
  }

  if (doc["wind_gust"]["value"].isNull()) {
    metarGust = "None";
  } else {
    int gust = doc["wind_gust"]["value"].as<int>();
    metarGust = String(gust) + " kt / " +
                String(gust * 1.15078f, 1) + " mph";
  }

  if (doc["visibility"]["value"].isNull()) {
    metarVisibility = "N/A";
  } else {
    metarVisibility = String(doc["visibility"]["value"].as<float>(), 1) + " sm";
  }

  if (doc["temperature"]["value"].isNull()) {
    metarTemp = "N/A";
  } else {
    float c = doc["temperature"]["value"].as<float>();
    metarTemp = String(c, 1) + " °C / " + String(c * 9.0f / 5.0f + 32.0f, 1) + " °F";
  }

  if (doc["dewpoint"]["value"].isNull()) {
    metarDewpoint = "N/A";
  } else {
    float c = doc["dewpoint"]["value"].as<float>();
    metarDewpoint = String(c, 1) + " °C / " + String(c * 9.0f / 5.0f + 32.0f, 1) + " °F";
  }

  if (doc["altimeter"]["value"].isNull()) {
    metarPressure = "N/A";
  } else {
    metarPressure = String(doc["altimeter"]["value"].as<float>(), 2) + " inHg";
  }

  metarObsEpoch = 0;
  return true;
}

static String awcStringOrNumber(JsonVariantConst v, int decimals = 0) {
  if (v.isNull()) return "";
  if (v.is<const char*>()) return String(v.as<const char*>());
  if (decimals > 0) return String(v.as<float>(), decimals);
  return String(v.as<int>());
}

static bool parseAwc(const String &json) {
  DynamicJsonDocument doc(6144);
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    metarError = String("AWC JSON: ") + err.c_str();
    return false;
  }

  if (!doc.is<JsonArray>() || doc.size() < 1) {
    metarError = "AWC returned no METAR row";
    return false;
  }

  JsonObject m = doc[0].as<JsonObject>();

  String station = m["icaoId"] | "";
  String rules = m["fltCat"] | "";
  String raw = m["rawOb"] | "";

  station.trim();
  station.toUpperCase();
  rules.trim();
  rules.toUpperCase();
  raw.trim();

  if (station.length() && station != cfg.airport) {
    metarError = "AWC station mismatch: " + station;
    return false;
  }

  if (!(rules == "VFR" || rules == "MVFR" ||
        rules == "IFR" || rules == "LIFR")) {
    metarError = "AWC missing fltCat";
    return false;
  }

  flightCategory = rules;
  rawMetar = raw;
  metarStation = station.length() ? station : cfg.airport;
  metarObsEpoch = m["obsTime"] | 0;

  if (metarObsEpoch > 0) {
    time_t t = (time_t)metarObsEpoch;
    struct tm tmUtc;
    gmtime_r(&t, &tmUtc);
    char tb[24];
    strftime(tb, sizeof(tb), "%Y-%m-%d %H:%MZ", &tmUtc);
    metarTime = String(tb);
  } else {
    metarTime = "N/A";
  }

  String wd = awcStringOrNumber(m["wdir"]);
  String ws = awcStringOrNumber(m["wspd"]);
  if (!wd.length() && !ws.length()) {
    metarWind = "N/A";
  } else {
    metarWind = (wd.length() ? wd : "?") + String("° at ") +
                (ws.length() ? ws : "?") + " kt";
    if (ws.length()) {
      float k = ws.toFloat();
      metarWind += " / " + String(k * 1.15078f, 1) + " mph";
    }
  }

  String wg = awcStringOrNumber(m["wgst"]);
  if (wg.length() && wg.toFloat() > 0) {
    metarGust = wg + " kt / " + String(wg.toFloat() * 1.15078f, 1) + " mph";
  } else {
    metarGust = "None";
  }

  String vis = awcStringOrNumber(m["visib"], 1);
  metarVisibility = vis.length() ? vis + " sm" : "N/A";

  if (!m["temp"].isNull()) {
    float c = m["temp"].as<float>();
    metarTemp = String(c, 1) + " °C / " + String(c * 9.0f / 5.0f + 32.0f, 1) + " °F";
  } else {
    metarTemp = "N/A";
  }

  if (!m["dewp"].isNull()) {
    float c = m["dewp"].as<float>();
    metarDewpoint = String(c, 1) + " °C / " + String(c * 9.0f / 5.0f + 32.0f, 1) + " °F";
  } else {
    metarDewpoint = "N/A";
  }

  if (!m["altim"].isNull()) {
    float a = m["altim"].as<float>();
    // AWC decoded altimeter commonly arrives in hPa. Accept either form.
    float inHg = (a > 100.0f) ? a * 0.0295299831f : a;
    metarPressure = String(inHg, 2) + " inHg";
  } else {
    metarPressure = "N/A";
  }

  return true;
}

static bool fetchAvwx() {
  lastAvwxHttpCode = 0;
  if (!cfg.avwxToken.length()) {
    metarError = "AVWX key not configured";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(7000);
  http.setTimeout(10000);
  http.setReuse(false);

  String url = "https://avwx.rest/api/metar/" + cfg.airport +
               "?format=json&onfail=cache";

  if (!http.begin(client, url)) {
    metarError = "AVWX http.begin failed";
    lastAvwxHttpCode = -1;
    return false;
  }

  http.addHeader("Authorization", "Bearer " + cfg.avwxToken);
  int code = http.GET();
  lastAvwxHttpCode = code;
  lastHttpCode = code;

  if (code != HTTP_CODE_OK) {
    metarError = "AVWX HTTP " + String(code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  if (!parseAvwx(body)) return false;

  metarSource = "AVWX";
  metarError = "";
  return true;
}

static bool fetchAwc() {
  lastAwcHttpCode = 0;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(12000);
  http.setReuse(false);
  http.setUserAgent(AWC_USER_AGENT);

  String url = "https://aviationweather.gov/api/data/metar?ids=" +
               cfg.airport + "&format=json";

  if (!http.begin(client, url)) {
    metarError = "AWC http.begin failed";
    lastAwcHttpCode = -1;
    return false;
  }

  int code = http.GET();
  lastAwcHttpCode = code;
  lastHttpCode = code;

  if (code != HTTP_CODE_OK) {
    metarError = "AWC HTTP " + String(code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  if (!parseAwc(body)) return false;

  metarSource = "AviationWeather.gov";
  metarError = "";
  return true;
}

static bool fetchMetar() {
  if (WiFi.status() != WL_CONNECTED) {
    metarError = "Wi-Fi not connected";
    return false;
  }

  lastMetarFetchAttemptMs = millis();
  clearMetarDetailFields();

  String avwxFailure;

  bool ok = false;
  if (cfg.avwxToken.length()) {
    ok = fetchAvwx();
    if (!ok) avwxFailure = metarError;
  }

  if (!ok) {
    ok = fetchAwc();
    if (!ok && avwxFailure.length()) {
      metarError = avwxFailure + " | fallback: " + metarError;
    }
  }

  if (!ok) {
    metarFetchFail++;
    Serial.printf("[METAR] FAIL %s\n", metarError.c_str());
    return false;
  }

  metarFetchOk++;
  lastMetarFetchSuccessMs = millis();

  Serial.printf("[METAR] OK source=%s airport=%s category=%s\n",
                metarSource.c_str(),
                cfg.airport.c_str(),
                flightCategory.c_str());
  Serial.printf("[METAR] %s\n", rawMetar.c_str());

  if (!adminLedTestActive) applyPrimaryColor();
  return true;
}

// -----------------------------------------------------------------------------
// Tail/hex utilities copied from App 1.11.9 (privacy exception removed)
// -----------------------------------------------------------------------------

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
  const String base34 = "ABCDEFGHJKLMNPQRSTUVWXYZ0123456789";

  const int icaooffset = 0xA00001;
  const int b1 = 101711;
  const int b2 = 10111;

  if (tail.length() < 2 || tail[0] != 'N') return -1;

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
    return icao + enc;
  }

  icao += d2 * b2 + 601;
  if (tail.length() == 3) return icao;

  int d3 = base10.indexOf(tail[3]);
  if (d3 > -1) {
    icao += d3 * 951 + 601;
    if (tail.length() == 4) return icao;

    int d4 = base10.indexOf(tail[4]);
    if (d4 > -1) {
      icao += d4 * 35 + 601;
      if (tail.length() == 5) return icao;

      String suf = tail.substring(5);
      if (suf.length() > 1) suf = suf.substring(0, 1);
      int enc = enc_suffix(suf);
      if (enc < 0) return -1;
      icao += enc;
      return icao;
    }

    String suf = tail.substring(4);
    if (suf.length() > 2) suf = suf.substring(0, 2);
    int enc = enc_suffix(suf);
    if (enc < 0) return -1;
    icao += enc;
    return icao;
  }

  String suf = tail.substring(3);
  if (suf.length() > 2) suf = suf.substring(0, 2);
  int enc = enc_suffix(suf);
  if (enc < 0) return -1;
  icao += enc;
  return icao;
}

static String usN_to_hex6(String tail) {
  int icao = usN_to_icao_int(tail);
  if (icao < 0) return "";
  char out[7];
  snprintf(out, sizeof(out), "%06X", icao);
  return String(out);
}

static String resolveTailOrHex(String tailIn, String hexIn) {
  tailIn.trim();
  tailIn.toUpperCase();
  hexIn.trim();
  hexIn.toUpperCase();

  if (tailIn.length()) {
    String h = usN_to_hex6(tailIn);
    if (h.length() == 6) return h;
  }

  if (hexIn.length() && isHex6(hexIn)) return hexIn;
  return "";
}

// -----------------------------------------------------------------------------
// Flight Pulse
// -----------------------------------------------------------------------------

static bool fpFetchIsFlying(const String &icaoHex6) {
  if (WiFi.status() != WL_CONNECTED || !isHex6(icaoHex6)) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(7000);
  http.setTimeout(10000);
  http.setReuse(false);

  String icao = icaoHex6;
  icao.toLowerCase();

  if (!http.begin(client, "https://api.adsb.lol/v2/icao/" + icao)) {
    fpLastHttpCode = -1;
    return false;
  }

  int code = http.GET();
  fpLastHttpCode = code;

  if (code != HTTP_CODE_OK) {
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

  float gs = a["gs"] | 0.0f;
  float altBaro = 0.0f;
  float altGeom = 0.0f;

  if (!a["alt_baro"].is<const char*>()) altBaro = a["alt_baro"] | 0.0f;
  if (!a["alt_geom"].is<const char*>()) altGeom = a["alt_geom"] | 0.0f;

  int gnd = a["gnd"] | 0;
  float alt = (altBaro > 0.0f) ? altBaro : altGeom;

  if (gnd == 1) return false;
  return alt > 500.0f || gs > 35.0f;
}

static void fpStartPulse() {
  fpPulseActive = true;
  fpPulseStartMs = millis();
}

static void fpStopPulseRestore() {
  if (!fpPulseActive) return;
  fpPulseActive = false;
  renderChannels(baseChannels, baseBrightnessPct);
}

static void fpUpdatePulseOverlay() {
  if (!fpPulseActive || adminLedTestActive) return;

  unsigned long now = millis();
  float t = fmodf((float)(now - fpPulseStartMs), FP_PERIOD_MS) / FP_PERIOD_MS;
  float wave = 0.5f - 0.5f * cosf(2.0f * PI * t);

  int minB = (int)roundf(baseBrightnessPct * FP_MIN_FRACTION);
  if (baseBrightnessPct > 0 && minB < 1) minB = 1;

  int b = (int)roundf(minB + (baseBrightnessPct - minB) * wave);
  renderChannels(baseChannels, b);
}

static bool fpAllowedNow() {
  if (!cfg.fpEnabled || WiFi.status() != WL_CONNECTED ||
      cfg.fpIcao.length() != 6) return false;

  if (adminLedTestActive) return false;

  // Night Light is its own explicit Flight Pulse gate.
  if (currentNightActive) return cfg.nightPulseWithFlight;

  return currentInSchedule;
}

static void serviceFlightPulse() {
  if (!fpAllowedNow()) {
    fpIsFlying = false;
    fpFlyingStreak = 0;
    fpStopPulseRestore();
    return;
  }

  unsigned long now = millis();

  if (now - fpLastCheckMs >= FP_CHECK_MS) {
    fpLastCheckMs = now;

    bool flyingNow = fpFetchIsFlying(cfg.fpIcao);

    if (flyingNow) fpFlyingStreak = min(fpFlyingStreak + 1, 3);
    else fpFlyingStreak = max(fpFlyingStreak - 1, -3);

    bool debounced = fpFlyingStreak >= 2;

    if (debounced != fpIsFlying) {
      fpIsFlying = debounced;
      if (fpIsFlying) fpStartPulse();
      else fpStopPulseRestore();
    }
  }

  fpUpdatePulseOverlay();
}

// -----------------------------------------------------------------------------
// Production firmware update handoff
// -----------------------------------------------------------------------------
// Production intentionally contains no firmware downloader/writer. Safe Mode
// owns GitHub download, Production partition writing, validation, and boot-back.

// -----------------------------------------------------------------------------
// User handlers
// -----------------------------------------------------------------------------

static void handleBrightness() {
  if (!server.hasArg("value")) {
    server.send(400, "text/plain", "Missing value");
    return;
  }

  cfg.brightness = clampInt(server.arg("value").toInt(),
                            BRIGHT_MIN, BRIGHT_MAX);

  refreshDisplayState(true);

  bool preview = server.hasArg("preview");
  if (!preview) saveConfig();

  server.send(200, "text/plain", "OK");
}

static void handleAirport() {
  String code = sanitizeAirport(server.arg("code"));
  if (!code.length()) {
    server.send(400, "text/plain", "Use a 3-4 character airport/station code.");
    return;
  }

  cfg.airport = code;
  saveConfig();

  rawMetar = "";
  flightCategory = "UNKNOWN";
  metarSource = "None";
  metarError = "Station changed; waiting for METAR";
  clearMetarDetailFields();

  if (WiFi.status() == WL_CONNECTED) {
    startMdns();
    fetchMetar();
  } else {
    applyPrimaryColor();
  }

  server.send(200, "text/plain", "OK");
}

static void handleSchedule() {
  if (!server.hasArg("enabled") || !server.hasArg("start") ||
      !server.hasArg("end") || !server.hasArg("tz")) {
    server.send(400, "text/plain", "Bad request");
    return;
  }

  int sh, sm, eh, em;
  if (!parseTimeArg(server.arg("start"), sh, sm) ||
      !parseTimeArg(server.arg("end"), eh, em)) {
    server.send(400, "text/plain", "Bad time");
    return;
  }

  cfg.scheduleEnabled = server.arg("enabled") == "on";
  cfg.startHour = sh;
  cfg.startMinute = sm;
  cfg.endHour = eh;
  cfg.endMinute = em;
  cfg.timezonePref = server.arg("tz");

  saveConfig();
  applyTimezone();
  refreshDisplayState(true);

  server.send(200, "text/plain", "OK");
}

static void handleMode() {
  String m = server.arg("value");
  m.toLowerCase();

  if (m == "auto") displayMode = MODE_AUTO;
  else if (m == "vfr") displayMode = MODE_VFR;
  else if (m == "mvfr") displayMode = MODE_MVFR;
  else if (m == "ifr") displayMode = MODE_IFR;
  else if (m == "lifr") displayMode = MODE_LIFR;
  else if (m == "cycle") displayMode = MODE_CYCLE;
  else {
    server.send(400, "text/plain", "Bad mode");
    return;
  }

  cfg.displayMode = (int)displayMode;
  saveConfig();

  cycleIndex = 0;
  lastCycleSwitchMs = millis();
  applyPrimaryColor();

  server.send(200, "text/plain", "OK");
}

static void handleFlightPulse() {
  bool enabled = server.arg("enabled") == "on";
  String tail = server.arg("tail");
  String hex = server.arg("hex");

  tail.trim();
  tail.toUpperCase();
  hex.trim();
  hex.toUpperCase();

  String resolved;
  if (enabled) {
    resolved = resolveTailOrHex(tail, hex);
    if (resolved.length() != 6) {
      server.send(400, "text/plain",
                  "Enter a valid US N-number or 6-character ICAO hex.");
      return;
    }
  }

  cfg.fpEnabled = enabled;
  cfg.fpTail = tail;
  cfg.fpIcao = resolved;

  fpIsFlying = false;
  fpFlyingStreak = 0;
  fpLastCheckMs = 0;
  fpStopPulseRestore();

  saveConfig();
  server.send(200, "text/plain", "OK");
}

static void handleNightManual() {
  cfg.nightManualOn = server.arg("on") == "1";
  saveConfig();
  refreshDisplayState(true);
  server.send(200, "text/plain", "OK");
}

static void handleNightSchedule() {
  int sh, sm, eh, em;
  if (!parseTimeArg(server.arg("start"), sh, sm) ||
      !parseTimeArg(server.arg("end"), eh, em)) {
    server.send(400, "text/plain", "Bad time");
    return;
  }

  cfg.nightSchedEnabled = server.arg("enabled") == "on";
  cfg.nightStartHour = sh;
  cfg.nightStartMinute = sm;
  cfg.nightEndHour = eh;
  cfg.nightEndMinute = em;

  saveConfig();
  refreshDisplayState(true);
  server.send(200, "text/plain", "OK");
}

static void handleNightWarmth() {
  cfg.nightWarmth = clampInt(server.arg("value").toInt(), 0, 100);
  saveConfig();
  refreshDisplayState(true);
  server.send(200, "text/plain", "OK");
}

static void handleNightBrightness() {
  cfg.nightBrightness = clampInt(server.arg("value").toInt(),
                                 BRIGHT_MIN, BRIGHT_MAX);
  saveConfig();
  refreshDisplayState(true);
  server.send(200, "text/plain", "OK");
}

static void handleNightPulse() {
  cfg.nightPulseWithFlight = server.arg("on") == "1";
  saveConfig();
  if (!cfg.nightPulseWithFlight && currentNightActive) fpStopPulseRestore();
  server.send(200, "text/plain", "OK");
}

static void handleRefreshMetar() {
  bool ok = fetchMetar();
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", ok ? "OK" : "FAIL");
}

static void handleSaveWiFi() {
  // The old page gave the manual SSID field priority. Browser autofill/stale
  // text could therefore override the network the user actually selected.
  // v0.2.1 carries an explicit source selector from the page.
  String source = server.arg("wifi_source");
  String selected = server.arg("ssid");
  String manual = server.arg("ssid_manual");
  String pass = server.arg("password");

  selected.trim();
  manual.trim();
  source.trim();

  String ssid;
  if (source == "manual") {
    ssid = manual;
  } else {
    ssid = selected;
  }

  // Graceful fallback for browsers with JavaScript disabled.
  if (!ssid.length()) {
    ssid = manual.length() ? manual : selected;
  }

  if (!ssid.length()) {
    server.send(400, "text/plain", "SSID required");
    return;
  }

  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);

  Serial.printf("[WIFI] user selected SSID '%s' via %s entry\n",
                ssid.c_str(),
                source == "manual" ? "manual" : "scan");

  server.send(200, "text/html",
              "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<body style='font-family:Arial,sans-serif;background:#0f1114;color:#f5f7f8;padding:24px;max-width:620px;margin:auto'>"
              "<div style='font-size:11px;letter-spacing:.18em;text-transform:uppercase;color:#32c96b;font-weight:800'>METAR LightWorks</div>"
              "<h2>Wi-Fi saved</h2><p>Saved network: <b>" + htmlEscape(ssid) + "</b></p>"
              "<p>The bulb is connecting now.</p>"
              "<p style='color:#aeb8c2'>If it cannot connect, its setup access point will return automatically.</p></body>");

  wifiApplyPending = true;
  wifiApplyAtMs = millis() + 500UL;
}

static void handleWifiScanStart() {
  // Async scan keeps the web server responsive. Delete only a completed old
  // scan; never cancel a scan already in progress.
  int state = WiFi.scanComplete();

  if (state == -1) {  // WIFI_SCAN_RUNNING
    server.send(200, "application/json", "{\"status\":\"scanning\"}");
    return;
  }

  if (state >= 0) WiFi.scanDelete();

  int rc = WiFi.scanNetworks(true, true);  // async, include hidden
  if (rc == -2) {  // WIFI_SCAN_FAILED / could not start
    server.send(500, "application/json", "{\"status\":\"error\"}");
    return;
  }

  server.send(200, "application/json", "{\"status\":\"scanning\"}");
}

static void handleWifiScanResults() {
  int n = WiFi.scanComplete();

  if (n == -1) {  // WIFI_SCAN_RUNNING
    server.send(200, "application/json", "{\"status\":\"scanning\"}");
    return;
  }

  if (n < 0) {
    server.send(500, "application/json", "{\"status\":\"error\"}");
    return;
  }

  // De-duplicate mesh/extender results by SSID and retain the strongest RSSI.
  // This keeps the list stable and prevents several identical SSID rows.
  static const int MAX_NETS = 32;
  String ssids[MAX_NETS];
  int rssis[MAX_NETS];
  bool opens[MAX_NETS];
  int count = 0;

  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;

    int found = -1;
    for (int j = 0; j < count; j++) {
      if (ssids[j] == s) {
        found = j;
        break;
      }
    }

    int rssi = WiFi.RSSI(i);
    bool isOpen = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;

    if (found >= 0) {
      if (rssi > rssis[found]) {
        rssis[found] = rssi;
        opens[found] = isOpen;
      }
      continue;
    }

    if (count < MAX_NETS) {
      ssids[count] = s;
      rssis[count] = rssi;
      opens[count] = isOpen;
      count++;
    }
  }

  String out = "{\"status\":\"done\",\"networks\":[";
  for (int i = 0; i < count; i++) {
    if (i) out += ",";
    out += "{\"ssid\":\"" + jsonEscape(ssids[i]) + "\",";
    out += "\"rssi\":" + String(rssis[i]) + ",";
    out += "\"open\":" + String(opens[i] ? "true" : "false") + "}";
  }
  out += "]}";

  WiFi.scanDelete();
  server.send(200, "application/json", out);
}

// -----------------------------------------------------------------------------
// Admin handlers
// -----------------------------------------------------------------------------

static String diagnosticsText() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *boot = esp_ota_get_boot_partition();
  const esp_partition_t *prod = findProdPartition();
  const esp_partition_t *safe = findSafePartition();

  String d;
  d.reserve(2600);

  d += "METAR Lightworks Bulb\n";
  d += "Firmware: " + String(FW_VERSION) + "\n";
  d += "Bulb model: " + bulbModelName() + "\n";
  d += "PWM map: " + bulbPwmMapText() + "\n";
  d += "Airport: " + cfg.airport + "\n";
  d += "Category: " + flightCategory + "\n";
  d += "METAR source: " + metarSource + "\n";
  d += "Raw METAR: " + rawMetar + "\n";
  d += "METAR error: " + metarError + "\n";
  d += "Last HTTP: " + String(lastHttpCode) + "\n";
  d += "AVWX HTTP: " + String(lastAvwxHttpCode) + "\n";
  d += "AWC HTTP: " + String(lastAwcHttpCode) + "\n";
  d += "Fetch OK/FAIL: " + String(metarFetchOk) + "/" + String(metarFetchFail) + "\n";
  d += "Last fetch attempt: " + ageText(lastMetarFetchAttemptMs) + "\n";
  d += "Last good METAR: " + ageText(lastMetarFetchSuccessMs) + "\n";

  d += "Display mode: " + modeToString() + "\n";
  d += "Output mode: " + outputMode + "\n";
  d += "Base R/G/B/CW/WW: " +
       String(baseChannels.r) + "/" + String(baseChannels.g) + "/" +
       String(baseChannels.b) + "/" + String(baseChannels.cw) + "/" +
       String(baseChannels.ww) + "\n";
  d += "Base brightness: " + String(baseBrightnessPct) + "%\n";
  d += "Rendered brightness: " + String(renderedBrightnessPct) + "%\n";
  d += "Night active: " + String(currentNightActive ? "YES" : "NO") + "\n";
  d += "Schedule on: " + String(currentInSchedule ? "YES" : "NO") + "\n";

  d += "Flight Pulse enabled: " + String(cfg.fpEnabled ? "YES" : "NO") + "\n";
  d += "Flight Pulse ICAO: " + cfg.fpIcao + "\n";
  d += "Flight Pulse flying: " + String(fpIsFlying ? "YES" : "NO") + "\n";
  d += "Flight Pulse pulsing: " + String(fpPulseActive ? "YES" : "NO") + "\n";
  d += "Flight Pulse HTTP: " + String(fpLastHttpCode) + "\n";

  d += "Uptime: " + uptimeText() + "\n";
  d += "Heap: " + String(ESP.getFreeHeap()) + "\n";
  d += "Min heap: " + String(ESP.getMinFreeHeap()) + "\n";
  d += "Reset: " + resetReasonText() + "\n";
  d += "Running: " + partLabel(running) + "\n";
  d += "Configured boot: " + partLabel(boot) + "\n";
  d += "Production: " + partLabel(prod) + " @ 0x010000 / 0x1C0000\n";
  d += "Safe Mode: " + partLabel(safe) + " @ 0x1D0000 / 0x1C0000\n";
  d += "Partition architecture: " +
       String(partitionArchitectureValid() ? "OK" : "MISMATCH") + "\n";
  d += "Recovery boot count: " + String(bootCountThisBoot) + "\n";
  d += "Healthy 30s mark: " + String(bootHealthMarked ? "YES" : "NO") + "\n";
  d += "Safe request: " + prefs.getString(SAFE_REQUEST_KEY, "") + "\n";
  d += "Last safe action: " + lastSafeAction + "\n";

  d += "WiFi: " + String(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DOWN") + "\n";
  if (WiFi.status() == WL_CONNECTED) {
    d += "SSID: " + WiFi.SSID() + "\n";
    d += "IP: " + WiFi.localIP().toString() + "\n";
    d += "RSSI: " + String(WiFi.RSSI()) + " dBm\n";
    d += "mDNS: http://" + mdnsHost + ".local/\n";
  }
  if (setupApRunning) {
    d += "Setup AP: " + cfg.deviceSsid + " @ " + WiFi.softAPIP().toString() + "\n";
  }

  d += "AVWX key: " + String(cfg.avwxToken.length() ? "CONFIGURED" : "NOT SET") + "\n";
  d += "Firmware repo: " + String(PRODUCTION_REPO) + "\n";
  d += "Production asset: " + String(PRODUCTION_ASSET) + "\n";
  d += "Production direct OTA: DISABLED - Safe Mode owns firmware install\n";

  return d;
}

static void handleAdminDiag() {
  if (!adminAuth()) return;
  server.send(200, "text/plain; charset=utf-8", diagnosticsText());
}

static void handleAdminBulbModelSave() {
  if (!adminAuth()) return;

  String model = server.arg("bulb_model");
  model.trim();
  model.toUpperCase();

  bulbModel = (model == "GU10") ? BULB_GU10 : BULB_E27;
  applyBulbModelPins();
  prefs.putString("bulb_model", bulbModelName());

  initBulb();
  adminLedTestActive = false;
  refreshDisplayState(true);

  server.sendHeader("Location", "/admin");
  server.send(303, "text/plain", "Saved");
}

static void handleAdminAvwxSave() {
  if (!adminAuth()) return;

  bool clear = server.hasArg("clear");
  String key = server.arg("avwx");
  key.trim();

  if (clear) cfg.avwxToken = "";
  else if (key.length()) cfg.avwxToken = key;

  saveConfig();

  // Test the new source selection immediately if online.
  if (WiFi.status() == WL_CONNECTED) fetchMetar();

  server.sendHeader("Location", "/admin");
  server.send(303, "text/plain", "Saved");
}

static void handleAdminFirmwareUpdate() {
  if (!adminAuth()) return;

  if (!mlwRequestSafeInstallLatest()) {
    server.send(500, "text/plain",
                "Could not request Safe Mode firmware install. Check diagnostics.");
    return;
  }

  server.send(200, "text/html",
              "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<h2>Firmware update requested</h2>"
              "<p>The bulb will restart into Safe Mode.</p>"
              "<p>Safe Mode will install the latest production firmware and return to Production.</p>");
  rebootAtMs = millis() + 1200UL;
}

static void handleAdminSafeRecovery() {
  if (!adminAuth()) return;

  if (!mlwRequestSafeRecovery()) {
    server.send(500, "text/plain",
                "Could not select Safe Mode. Check diagnostics.");
    return;
  }

  server.send(200, "text/html",
              "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<h2>Restarting into Safe Mode</h2>"
              "<p>No Production update was requested.</p>");
  rebootAtMs = millis() + 1200UL;
}

static void handleAdminLedTest() {
  if (!adminAuth()) return;

  String c = server.arg("c");
  c.toLowerCase();

  fpStopPulseRestore();

  if (c == "metar") {
    adminLedTestActive = false;
    refreshDisplayState(true);
  } else {
    adminLedTestActive = true;

    if (c == "red") setBaseRGB(255, 0, 0, 30, "ADMIN TEST RED");
    else if (c == "green") setBaseRGB(0, 255, 0, 30, "ADMIN TEST GREEN");
    else if (c == "blue") setBaseRGB(0, 0, 255, 30, "ADMIN TEST BLUE");
    else if (c == "cw") {
      BulbChannels ch; ch.cw = 255;
      setBaseOutput(ch, 30, "ADMIN TEST COOL WHITE");
    } else if (c == "ww") {
      BulbChannels ch; ch.ww = 255;
      setBaseOutput(ch, 30, "ADMIN TEST WARM WHITE");
    } else if (c == "off") {
      clearBulb("ADMIN TEST OFF");
    } else {
      server.send(400, "text/plain", "Bad LED test");
      return;
    }

    renderChannels(baseChannels, baseBrightnessPct);
  }

  server.send(200, "text/plain", "OK");
}

static void handleAdminWifiClear() {
  if (!adminAuth()) return;

  prefs.remove("ssid");
  prefs.remove("pass");

  WiFi.disconnect(true, true);
  delay(100);
  startSetupAp();

  server.send(200, "text/html",
              "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<h2>Saved Wi-Fi cleared</h2>"
              "<p>The setup access point is being enabled.</p>");
}

static void handleAdminReboot() {
  if (!adminAuth()) return;

  server.send(200, "text/html",
              "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<h2>Rebooting...</h2>");
  rebootAtMs = millis() + 700UL;
}

// -----------------------------------------------------------------------------
// User page
// -----------------------------------------------------------------------------

static String userPage() {
  struct tm tmnow;
  char clockBuf[9];
  if (getLocalTime(&tmnow, 10)) {
    snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d:%02d",
             tmnow.tm_hour, tmnow.tm_min, tmnow.tm_sec);
  } else {
    strcpy(clockBuf, "--:--:--");
  }

  String fpStatus = "Disabled";
  if (cfg.fpEnabled) {
    if (!cfg.fpIcao.length()) fpStatus = "Enabled, no aircraft set";
    else fpStatus = fpIsFlying ? "Flying: YES" : "Flying: NO";
  }

  String page;
  page.reserve(19000);

  page += R"rawliteral(<!doctype html><html><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>METAR LightWorks Bulb</title><style>
:root{--bg:#0f1114;--panel:#191d22;--panel2:#20262c;--line:#303840;--text:#f5f7f8;--muted:#aeb8c2;--vfr:#32c96b;--mvfr:#3f83f8;--ifr:#e5484d;--lifr:#b45cff}
*{box-sizing:border-box}
body{font-family:Arial,sans-serif;background:var(--bg);color:var(--text);margin:0}
header{background:#0a0c0f;color:#fff;padding:22px 16px 18px;text-align:left;position:relative;border-bottom:1px solid var(--line)}
.brand-kicker{font-size:11px;letter-spacing:.18em;text-transform:uppercase;color:var(--vfr);font-weight:800}
.brand-title{font-size:31px;line-height:.95;font-weight:800;letter-spacing:-.035em;margin:5px 0 8px}
.brand-sub{font-size:13px;color:var(--muted);margin:0}
.wx-row{display:flex;gap:6px;margin-top:13px;flex-wrap:wrap}
.wx{font-size:10px;font-weight:800;letter-spacing:.05em;padding:4px 8px;border-radius:999px;color:#fff}
.vfr{background:var(--vfr)}.mvfr{background:var(--mvfr)}.ifr{background:var(--ifr)}.lifr{background:var(--lifr)}
.lock{position:absolute;right:14px;top:16px;color:#dce2e8;text-decoration:none;font-size:12px;border:1px solid var(--line);padding:7px 10px;border-radius:8px}
.container{padding:14px;max-width:760px;margin:auto}
.card{background:var(--panel);padding:14px;margin-bottom:12px;border-radius:12px;border:1px solid var(--line);box-shadow:0 8px 24px #0004}
.card h3{margin-top:2px;color:#fff}
label{font-weight:bold;display:block;margin-top:9px;color:#eef2f4}
input,select,button{box-sizing:border-box;width:100%;padding:10px;margin-top:6px;border:1px solid var(--line);border-radius:8px}
input,select{background:#0d1013;color:#fff}
button{cursor:pointer;background:#e9edf0;color:#111;font-weight:700}
button:hover{background:#fff}
.sw{position:relative;display:inline-block;width:50px;height:28px}
.sw input{opacity:0;width:0;height:0}.sl{position:absolute;cursor:pointer;inset:0;background:#59616a;transition:.2s;border-radius:28px}
.sl:before{position:absolute;content:"";height:22px;width:22px;left:3px;bottom:3px;background:#fff;transition:.2s;border-radius:50%}
input:checked+.sl{background:var(--vfr)}input:checked+.sl:before{transform:translateX(22px)}
td{padding:4px 6px;vertical-align:top}.grid{display:grid;grid-template-columns:1fr 1fr;gap:7px;margin-top:8px}
.row{display:flex;gap:8px;align-items:center}.row input[type=range]{flex:1}.row input[type=number]{width:90px}.row button{width:100px}
.badge{display:inline-block;padding:4px 9px;border-radius:999px;background:var(--panel2);border:1px solid var(--line);font-size:12px;color:#fff}
.small{font-size:13px;color:var(--muted);line-height:1.35}.raw{word-break:break-word;background:#0d1013;padding:9px;border-radius:8px;border:1px solid var(--line);color:#e8ecef}
@media(max-width:520px){.brand-title{font-size:28px}.row{flex-wrap:wrap}.row button{width:100%}}
</style></head><body><header>
<div class="brand-kicker">Aviation Weather</div>
<div class="brand-title">METAR<br>LightWorks</div>
<p class="brand-sub">Live flight conditions, always visible.</p>
<div class="wx-row"><span class="wx vfr">VFR</span><span class="wx mvfr">MVFR</span><span class="wx ifr">IFR</span><span class="wx lifr">LIFR</span></div>
<a class="lock" href="/admin">Admin</a></header><div class="container">)rawliteral";

  page += "<div class='card'><h3>Current METAR & Time</h3><table>";
  page += "<tr><td>Time</td><td>" + String(clockBuf) + "</td></tr>";
  page += "<tr><td>Station</td><td>" + htmlEscape(metarStation.length() ? metarStation : cfg.airport) + "</td></tr>";
  page += "<tr><td>METAR Time</td><td>" + htmlEscape(metarTime) + "</td></tr>";
  page += "<tr><td>Category</td><td><b>" + htmlEscape(flightCategory) + "</b></td></tr>";
  page += "<tr><td>Source</td><td>" + htmlEscape(metarSource) + "</td></tr>";
  page += "<tr><td>Wind</td><td>" + htmlEscape(metarWind) + "</td></tr>";
  page += "<tr><td>Gusts</td><td>" + htmlEscape(metarGust) + "</td></tr>";
  page += "<tr><td>Visibility</td><td>" + htmlEscape(metarVisibility) + "</td></tr>";
  page += "<tr><td>Temp</td><td>" + htmlEscape(metarTemp) + "</td></tr>";
  page += "<tr><td>Dewpoint</td><td>" + htmlEscape(metarDewpoint) + "</td></tr>";
  page += "<tr><td>Pressure</td><td>" + htmlEscape(metarPressure) + "</td></tr>";
  page += "</table>";

  if (rawMetar.length()) {
    page += "<div class='raw'>" + htmlEscape(rawMetar) + "</div>";
  }
  if (metarError.length()) {
    page += "<p class='small'><b>Last fetch note:</b> " + htmlEscape(metarError) + "</p>";
  }

  page += "<form method='POST' action='/refresh'><button>Refresh METAR</button></form></div>";

  page += R"rawliteral(<div class="card"><h3>Station & Brightness</h3>
<label>Airport Code</label><input id="airportInput" value=")rawliteral";
  page += htmlEscape(cfg.airport);
  page += R"rawliteral("><button id="airportBtn" type="button">Update Station</button>
<label>Bulb Brightness</label><div class="row"><input type="range" id="brightnessSlider" min="5" max="100" value=")rawliteral";
  page += String(cfg.brightness);
  page += R"rawliteral("><input type="number" id="brightnessNum" min="5" max="100" value=")rawliteral";
  page += String(cfg.brightness);
  page += R"rawliteral("><button id="brightnessSave" type="button">Save</button></div>
<div class="small">Drag/type previews. Save stores it.</div></div>)rawliteral";

  page += "<div class='card'><h3>Flight Pulse</h3><p><span class='badge'>" + fpStatus + "</span></p>";
  page += R"rawliteral(<label>Enable</label><label class="sw"><input type="checkbox" id="fpToggle" )rawliteral";
  page += cfg.fpEnabled ? "checked" : "";
  page += R"rawliteral(><span class="sl"></span></label>
<label>Tail Number</label><input id="fpTail" placeholder="N247AP" value=")rawliteral";
  page += htmlEscape(cfg.fpTail);
  page += R"rawliteral("><label>or ICAO Hex</label><input id="fpHex" maxlength="6" placeholder="A24A0E" value=")rawliteral";
  page += htmlEscape(cfg.fpIcao);
  page += R"rawliteral("><div class="small">Tail converts to hex. Hex is used if Tail is blank.</div>
<button id="fpSaveBtn" type="button">Save Flight Pulse</button></div>)rawliteral";

  page += R"rawliteral(<div class="card"><h3>METAR Schedule</h3>
<label>Enable</label><label class="sw"><input type="checkbox" id="schedToggle" )rawliteral";
  page += cfg.scheduleEnabled ? "checked" : "";
  page += R"rawliteral(><span class="sl"></span></label>
<label>Time Zone</label><select id="tzSelect">
<option value="UTC0">UTC</option>
<option value="EST5EDT">America/New_York</option>
<option value="CST6CDT">America/Chicago</option>
<option value="MST7MDT">America/Denver</option>
<option value="PST8PDT">America/Los_Angeles</option>
</select><label>Start</label><input type="time" id="startTime" value=")rawliteral";
  page += timeString(cfg.startHour, cfg.startMinute);
  page += R"rawliteral("><label>End</label><input type="time" id="endTime" value=")rawliteral";
  page += timeString(cfg.endHour, cfg.endMinute);
  page += R"rawliteral("><button id="scheduleBtn" type="button">Save Schedule</button></div>)rawliteral";

  // Night Light is always present.
  page += R"rawliteral(<div class="card"><h3>Night Light</h3>
<label>Manual</label><label class="sw"><input type="checkbox" id="nightManual" )rawliteral";
  page += cfg.nightManualOn ? "checked" : "";
  page += R"rawliteral(><span class="sl"></span></label>
<label>Schedule</label><label class="sw"><input type="checkbox" id="nightSched" )rawliteral";
  page += cfg.nightSchedEnabled ? "checked" : "";
  page += R"rawliteral(><span class="sl"></span></label>
<label>Start</label><input type="time" id="nightStart" value=")rawliteral";
  page += timeString(cfg.nightStartHour, cfg.nightStartMinute);
  page += R"rawliteral("><label>End</label><input type="time" id="nightEnd" value=")rawliteral";
  page += timeString(cfg.nightEndHour, cfg.nightEndMinute);
  page += R"rawliteral("><label>Warmth (Cool → Warm)</label>
<div class="row"><input type="range" id="nightWarmth" min="0" max="100" value=")rawliteral";
  page += String(cfg.nightWarmth);
  page += R"rawliteral("><input type="number" id="nightWarmthNum" min="0" max="100" value=")rawliteral";
  page += String(cfg.nightWarmth);
  page += R"rawliteral("></div><label>Brightness</label>
<div class="row"><input type="range" id="nightBright" min="5" max="100" value=")rawliteral";
  page += String(cfg.nightBrightness);
  page += R"rawliteral("><input type="number" id="nightBrightNum" min="5" max="100" value=")rawliteral";
  page += String(cfg.nightBrightness);
  page += R"rawliteral("></div><label>Pulse with Flight Pulse</label>
<label class="sw"><input type="checkbox" id="nightPulse" )rawliteral";
  page += cfg.nightPulseWithFlight ? "checked" : "";
  page += R"rawliteral(><span class="sl"></span></label>
<button id="nightSave" type="button">Save Night Light</button></div>)rawliteral";

  page += "<div class='card'><h3>Display Mode</h3><p><b>Current:</b> <span class='badge'>" +
          modeToString() + "</span></p>";
  page += R"rawliteral(<div class="grid">
<button class="modeBtn" type="button" data-mode="auto">Auto</button>
<button class="modeBtn" type="button" data-mode="vfr">VFR</button>
<button class="modeBtn" type="button" data-mode="mvfr">MVFR</button>
<button class="modeBtn" type="button" data-mode="ifr">IFR</button>
<button class="modeBtn" type="button" data-mode="lifr">LIFR</button>
<button class="modeBtn" type="button" data-mode="cycle">Cycle</button>
</div></div>)rawliteral";

  page += R"rawliteral(<div class="card"><h3>Connect Your Bulb</h3><div class="small">Choose your Wi-Fi network below. You can also enter the network name manually.</div>
<form id="wifiForm" action="/save" method="POST" autocomplete="off">
<input type="hidden" id="wifiSource" name="wifi_source" value="scan">
<label>Select Wi-Fi Network</label><select id="ssidSelect" name="ssid"><option value="">Tap Refresh Networks</option></select>
<button id="refreshWifiBtn" type="button">Refresh Networks</button>
<div id="wifiScanStatus" class="small" style="margin-top:8px">Network list loads only when requested.</div>
<label>Or enter manually</label><input id="ssidManual" name="ssid_manual" autocomplete="off" placeholder="SSID">
<label>Password</label><input type="password" name="password" autocomplete="new-password" placeholder="Password">
<button type="submit">Save Wi-Fi</button></form>)rawliteral";

  if (WiFi.status() == WL_CONNECTED) {
    page += "<div class='small'>Connected to " + htmlEscape(WiFi.SSID()) +
            " at " + WiFi.localIP().toString() + ". mDNS: http://" +
            mdnsHost + ".local/</div>";
  } else if (setupApRunning) {
    page += "<div class='small'>Setup AP: " + htmlEscape(cfg.deviceSsid) +
            " at 192.168.4.1</div>";
  }
  page += "</div>";

  page += R"rawliteral(</div><script>
(function(){
function q(id){return document.getElementById(id)}
function clamp(v,a,b){v=parseInt(v,10);if(isNaN(v))v=b;if(v<a)v=a;if(v>b)v=b;return v}
function get(u,cb){fetch(u).then(function(r){return r.text().then(function(t){if(cb)cb(r,t)})})}

var sl=q('brightnessSlider'),nu=q('brightnessNum'),tm;
if(sl&&nu){
  function pv(v){clearTimeout(tm);tm=setTimeout(function(){fetch('/brightness?value='+encodeURIComponent(v)+'&preview=1')},150)}
  sl.oninput=function(){var v=clamp(this.value,5,100);nu.value=v;pv(v)}
  nu.oninput=function(){var v=clamp(this.value,5,100);sl.value=v;pv(v)}
  q('brightnessSave').onclick=function(){var v=clamp(nu.value,5,100);sl.value=nu.value=v;get('/brightness?value='+encodeURIComponent(v),function(){alert('Saved')})}
}

q('airportBtn').onclick=function(){
  var c=q('airportInput').value.trim().toUpperCase();
  if(c)get('/airport?code='+encodeURIComponent(c),function(r,t){if(!r.ok)alert(t);else location.reload()})
};

q('tzSelect').value=')rawliteral";
  page += cfg.timezonePref;
  page += R"rawliteral(';

q('scheduleBtn').onclick=function(){
  var e=q('schedToggle').checked?'on':'off';
  var u='/schedule?enabled='+e+'&tz='+encodeURIComponent(q('tzSelect').value)+'&start='+encodeURIComponent(q('startTime').value)+'&end='+encodeURIComponent(q('endTime').value);
  get(u,function(r,t){if(!r.ok)alert(t);else alert('Schedule saved')})
};

document.querySelectorAll('.modeBtn').forEach(function(b){
  b.onclick=function(){get('/mode?value='+encodeURIComponent(this.dataset.mode),function(){location.reload()})}
});

var ft=q('fpTail'),fh=q('fpHex');
ft.oninput=function(){if(this.value.trim())fh.value=''};
fh.oninput=function(){if(this.value.trim())ft.value=''};
q('fpSaveBtn').onclick=function(){
  var e=q('fpToggle').checked?'on':'off';
  var u='/flightpulse?enabled='+e+'&tail='+encodeURIComponent(ft.value.trim().toUpperCase())+'&hex='+encodeURIComponent(fh.value.trim().toUpperCase());
  get(u,function(r,t){if(!r.ok)alert(t);else{alert('Flight Pulse saved');location.reload()}})
};

function sync(a,b){
  q(a).oninput=function(){q(b).value=this.value};
  q(b).oninput=function(){q(a).value=this.value}
}
sync('nightWarmth','nightWarmthNum');
sync('nightBright','nightBrightNum');

q('nightSave').onclick=function(){
  Promise.all([
    fetch('/night/manual?on='+(q('nightManual').checked?'1':'0')),
    fetch('/night/schedule?enabled='+(q('nightSched').checked?'on':'off')+'&start='+encodeURIComponent(q('nightStart').value)+'&end='+encodeURIComponent(q('nightEnd').value)),
    fetch('/night/warmth?value='+encodeURIComponent(q('nightWarmth').value)),
    fetch('/night/pulse?on='+(q('nightPulse').checked?'1':'0')),
    fetch('/night/brightness?value='+encodeURIComponent(q('nightBright').value))
  ]).then(function(){alert('Night Light saved')})
};

var wifiScanTimer=null;
function finishWifiScan(d){
  var s=q('ssidSelect'),n=d.networks||[],old=s.value;
  s.innerHTML='';
  if(!n.length){
    s.innerHTML='<option value="">No networks found</option>';
    q('wifiScanStatus').textContent='No networks found. Type SSID manually.';
    return
  }
  n.sort(function(a,b){return(b.rssi||-999)-(a.rssi||-999)});
  n.forEach(function(x){
    var o=document.createElement('option');
    o.value=x.ssid;
    o.textContent=x.ssid+' ('+(x.rssi||0)+' dBm, '+(x.open?'Open':'Secured')+')';
    s.appendChild(o)
  });
  if(old){
    for(var i=0;i<s.options.length;i++){
      if(s.options[i].value===old){s.selectedIndex=i;break}
    }
  }
  q('wifiScanStatus').textContent='Found '+n.length+' network'+(n.length==1?'':'s')+'. Select the one you want.';
}
function pollWifiScan(){
  fetch('/wifi/scan/results',{cache:'no-store'}).then(function(r){return r.json()}).then(function(d){
    if(d.status==='scanning'){
      wifiScanTimer=setTimeout(pollWifiScan,700);
      return
    }
    q('refreshWifiBtn').disabled=false;
    if(d.status==='done')finishWifiScan(d);
    else q('wifiScanStatus').textContent='Scan failed. Type SSID manually.';
  }).catch(function(){
    q('refreshWifiBtn').disabled=false;
    q('wifiScanStatus').textContent='Scan failed. Type SSID manually.'
  })
}
q('refreshWifiBtn').onclick=function(){
  if(wifiScanTimer)clearTimeout(wifiScanTimer);
  this.disabled=true;
  q('wifiScanStatus').textContent='Scanning...';
  fetch('/wifi/scan/start',{cache:'no-store'}).then(function(r){return r.json()}).then(function(){
    pollWifiScan()
  }).catch(function(){
    q('refreshWifiBtn').disabled=false;
    q('wifiScanStatus').textContent='Could not start scan. Type SSID manually.'
  })
};

q('ssidSelect').onchange=function(){
  q('wifiSource').value='scan';
  q('ssidManual').value='';
};
q('ssidManual').oninput=function(){
  if(this.value.trim()){
    q('wifiSource').value='manual';
    q('ssidSelect').selectedIndex=-1;
  }else{
    q('wifiSource').value='scan';
  }
};
q('wifiForm').onsubmit=function(){
  var selected=q('ssidSelect').value.trim();
  var manual=q('ssidManual').value.trim();
  var source=q('wifiSource').value;
  if(source==='manual' && !manual){
    alert('Enter the manual SSID.');
    return false;
  }
  if(source!=='manual' && !selected){
    if(manual){
      q('wifiSource').value='manual';
    }else{
      alert('Select a network or enter an SSID manually.');
      return false;
    }
  }
  return true;
};
})();
</script></body></html>)rawliteral";

  return page;
}

// -----------------------------------------------------------------------------
// Admin page
// -----------------------------------------------------------------------------

static String adminPage() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *boot = esp_ota_get_boot_partition();
  const esp_partition_t *safe = findSafePartition();

  String h;
  h.reserve(12000);

  h += R"rawliteral(<!doctype html><html><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bulb Admin</title><style>
body{font-family:Arial;background:#f2f2f2;margin:0;padding:16px;color:#333}
.card{background:#fff;padding:14px;border-radius:10px;box-shadow:0 2px 6px #0002;max-width:760px;margin:0 auto 12px}
input,button{box-sizing:border-box;width:100%;padding:10px;margin-top:6px;border:1px solid #ccc;border-radius:8px}
button{cursor:pointer}.row{display:flex;gap:8px;flex-wrap:wrap}.row button{flex:1;min-width:120px}
.small{color:#555;font-size:13px;line-height:1.35}code{word-break:break-word}
</style></head><body>)rawliteral";

  h += "<div class='card'><h2>Bulb Admin</h2>";
  h += "<p><b>Firmware:</b> <code>" + String(FW_VERSION) + "</code></p>";
  h += "<p><b>Running:</b> <code>" + partLabel(running) + "</code> &nbsp; "
       "<b>Boot:</b> <code>" + partLabel(boot) + "</code> &nbsp; "
       "<b>Safe Mode:</b> <code>" + partLabel(safe) + "</code></p>";
  h += "<p><b>METAR:</b> " + htmlEscape(cfg.airport) + " / " +
       htmlEscape(flightCategory) + " / " + htmlEscape(metarSource) + "</p>";
  h += "<p><a href='/'>Back to Main UI</a> &nbsp; <a href='/admin/diag'>Plain Diagnostics</a></p></div>";

  h += "<div class='card'><h3>Bulb Hardware</h3>";
  h += "<p><b>Model:</b> " + bulbModelName() + "</p>";
  h += "<p><b>PWM map:</b> <code>" + bulbPwmMapText() + "</code></p>";
  h += "<p class='small'>Factory default is E27. Change this only when intentionally building a GU10 bulb.</p>";
  h += R"rawliteral(<form method="POST" action="/admin/bulbmodel">
<label>Bulb Type</label>
<select name="bulb_model">
<option value="E27")rawliteral";
  h += bulbModel == BULB_E27 ? " selected" : "";
  h += R"rawliteral(>E27</option>
<option value="GU10")rawliteral";
  h += bulbModel == BULB_GU10 ? " selected" : "";
  h += R"rawliteral(>GU10</option>
</select><button>Save Bulb Type</button></form></div>)rawliteral";

  h += "<div class='card'><h3>AVWX</h3>";
  h += "<p class='small'>AVWX is primary when a key is configured. AviationWeather.gov automatically takes over if the key is missing or AVWX fails.</p>";
  h += "<p><b>Status:</b> " + String(cfg.avwxToken.length() ? "Key configured" : "No key - using .gov fallback") + "</p>";
  h += R"rawliteral(<form method="POST" action="/admin/avwx">
<label>New AVWX Key</label><input name="avwx" type="password" placeholder="Leave blank to keep current key">
<label><input style="width:auto" type="checkbox" name="clear"> Clear stored AVWX key</label>
<button>Save AVWX Setting</button></form></div>)rawliteral";

  h += R"rawliteral(<div class="card"><h3>LED / Channel Test</h3>
<p class="small">Uses the sealed-bulb-proven physical mapping. Test mode stays active until Return to Lamp.</p>
<div class="row">
<button onclick="fetch('/admin/led?c=red')">Red</button>
<button onclick="fetch('/admin/led?c=green')">Green</button>
<button onclick="fetch('/admin/led?c=blue')">Blue</button>
<button onclick="fetch('/admin/led?c=cw')">Cool White</button>
<button onclick="fetch('/admin/led?c=ww')">Warm White</button>
<button onclick="fetch('/admin/led?c=off')">Off</button>
<button onclick="fetch('/admin/led?c=metar')">Return to Lamp</button>
</div></div>)rawliteral";

  h += "<div class='card'><h3>Firmware / Safe Mode</h3>";
  h += "<p class='small'>Production never downloads or writes firmware. Update Firmware saves an install-latest request in NVS, selects the permanent Safe Mode factory partition, and reboots. Safe Mode owns the GitHub install and writes only Production.</p>";
  h += "<p class='small'><b>Repository:</b> " + String(PRODUCTION_REPO) +
       "<br><b>Asset:</b> " + String(PRODUCTION_ASSET) + "</p>";
  h += R"rawliteral(<form method="POST" action="/admin/firmware/update" onsubmit="return confirm('Restart into Safe Mode and install the latest production firmware?')">
<button>UPDATE FIRMWARE</button></form>
<form method="POST" action="/admin/safe" onsubmit="return confirm('Restart into Safe Mode for recovery/maintenance?')">
<button>RESTART INTO SAFE MODE</button></form></div>)rawliteral";

  h += R"rawliteral(<div class="card"><h3>Maintenance</h3>
<form method="POST" action="/admin/wifi/clear" onsubmit="return confirm('Clear saved Wi-Fi and start setup AP?')">
<button>Clear Saved Wi-Fi / Start Setup AP</button></form>
<form method="POST" action="/admin/reboot" onsubmit="return confirm('Reboot bulb now?')">
<button>Reboot Bulb</button></form>
</div>)rawliteral";

  h += "</body></html>";
  return h;
}

// -----------------------------------------------------------------------------
// Web route setup
// -----------------------------------------------------------------------------

static void handleRoot() {
  server.send(200, "text/html; charset=utf-8", userPage());
}

static void handleAdminHome() {
  if (!adminAuth()) return;
  server.send(200, "text/html; charset=utf-8", adminPage());
}

static void handleNotFound() {
  if (setupApRunning) {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "Redirecting...");
    return;
  }

  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

static void setupWeb() {
  Serial.println("[WEB] register routes");

  // User
  server.on("/", HTTP_GET, handleRoot);
  server.on("/refresh", HTTP_POST, handleRefreshMetar);
  server.on("/brightness", HTTP_GET, handleBrightness);
  server.on("/airport", HTTP_GET, handleAirport);
  server.on("/schedule", HTTP_GET, handleSchedule);
  server.on("/mode", HTTP_GET, handleMode);
  server.on("/flightpulse", HTTP_GET, handleFlightPulse);

  server.on("/night/manual", HTTP_GET, handleNightManual);
  server.on("/night/schedule", HTTP_GET, handleNightSchedule);
  server.on("/night/warmth", HTTP_GET, handleNightWarmth);
  server.on("/night/brightness", HTTP_GET, handleNightBrightness);
  server.on("/night/pulse", HTTP_GET, handleNightPulse);

  server.on("/save", HTTP_POST, handleSaveWiFi);
  server.on("/wifi/scan/start", HTTP_GET, handleWifiScanStart);
  server.on("/wifi/scan/results", HTTP_GET, handleWifiScanResults);

  // Admin
  server.on("/admin", HTTP_GET, handleAdminHome);
  server.on("/admin/diag", HTTP_GET, handleAdminDiag);
  server.on("/admin/bulbmodel", HTTP_POST, handleAdminBulbModelSave);
  server.on("/admin/avwx", HTTP_POST, handleAdminAvwxSave);
  server.on("/admin/led", HTTP_GET, handleAdminLedTest);
  server.on("/admin/wifi/clear", HTTP_POST, handleAdminWifiClear);
  server.on("/admin/reboot", HTTP_POST, handleAdminReboot);
  server.on("/admin/firmware/update", HTTP_POST, handleAdminFirmwareUpdate);
  server.on("/admin/safe", HTTP_POST, handleAdminSafeRecovery);

  server.onNotFound(handleNotFound);

  Serial.println("[WEB] server.begin()");
  server.begin();
  Serial.println("[WEB] READY");
}

// -----------------------------------------------------------------------------
// setup / loop
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  // Physical sealed-bulb recovery must run before bulb/network/METAR/UI setup.
  mlwEarlyRecoveryBootCheck();

  delay(150);
  Serial.println();
  Serial.println("============================================");
  Serial.printf(" %s\n", FW_VERSION);
  Serial.println("============================================");

  Serial.printf("[STATE] running=%s boot=%s safe=%s arch=%s\n",
                partLabel(esp_ota_get_running_partition()).c_str(),
                partLabel(esp_ota_get_boot_partition()).c_str(),
                partLabel(findSafePartition()).c_str(),
                partitionArchitectureValid() ? "OK" : "MISMATCH");

  initBulb();

  prefsReady = prefs.begin(PREF_NAMESPACE, false);
  Serial.printf("[NVS] begin '%s' -> %s\n",
                PREF_NAMESPACE,
                prefsReady ? "OK" : "FAIL");

  loadConfig();

  displayMode = (DisplayMode)cfg.displayMode;
  mdnsHost = hostForAirport(cfg.airport);

  // IMPORTANT: v0.1.2 proved the C3 must have a network interface before
  // WebServer::begin(). Keep this ordering frozen.
  WiFi.persistent(true);
  beginSavedWiFi();

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 12000UL) {
    delay(20);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] CONNECTED ip=%s RSSI=%d\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
  } else {
    Serial.println("[WIFI] home not connected; starting setup AP");
    startSetupAp();
  }

  setupWeb();

  if (WiFi.status() == WL_CONNECTED) {
    startMdns();
    applyTimezone();
    fetchMetar();
    initialFetchPending = false;
  }

  currentInSchedule = scheduleWindow(cfg.scheduleEnabled,
                                     cfg.startHour, cfg.startMinute,
                                     cfg.endHour, cfg.endMinute);
  currentNightActive = computeNightActive();
  applyPrimaryColor();

  lastWifiAttemptMs = millis();
  lastStateCheckMs = millis();
  fpLastCheckMs = millis() - FP_CHECK_MS;  // allow first check promptly
}

void loop() {
  serviceHealthyBootMarker();

  if (setupApRunning) dnsServer.processNextRequest();

  server.handleClient();
  serviceWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    if (initialFetchPending) {
      initialFetchPending = false;
      fetchMetar();
    } else if (!lastMetarFetchAttemptMs ||
               millis() - lastMetarFetchAttemptMs >= METAR_INTERVAL_MS) {
      fetchMetar();
    }
  }

  unsigned long now = millis();

  if (now - lastStateCheckMs >= STATE_CHECK_MS) {
    lastStateCheckMs = now;
    refreshDisplayState(false);
  }

  if (!currentNightActive && currentInSchedule &&
      displayMode == MODE_CYCLE &&
      now - lastCycleSwitchMs >= 3000UL) {
    lastCycleSwitchMs = now;
    cycleIndex = (cycleIndex + 1) % 4;
    applyPrimaryColor();
  }

  serviceFlightPulse();

  if (rebootAtMs && millis() >= rebootAtMs) {
    delay(50);
    ESP.restart();
  }

  delay(2);
}
