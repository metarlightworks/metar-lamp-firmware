#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <ESP.h>
#include "AppTypes.h"

// defined in .ino
extern WebServer server;
extern AppConfig cfg;

extern bool saveConfig();
extern void restartMDNSFixed();
extern void rebuildStripFromConfig();
extern void refreshNow();
extern void clearLED();
extern void setLEDColor(uint8_t r, uint8_t g, uint8_t b);

// OTA status from .ino
extern String otaStatusLine;
extern bool   otaUpdateAvailable;
extern unsigned long otaLastCheckMs;
extern const char* otaAssetNameForThisChip();

// OTA actions in .ino
extern bool otaCheckNow();
extern void otaInstallNow();
extern void otaMaybeAutoCheck();

// ---------- Basic Auth ----------
static const char* ADMIN_USER = "admin";
static const char* ADMIN_PASS = "north";  // keep same as your Lamp, or change

static bool adminAuth() {
  if (server.authenticate(ADMIN_USER, ADMIN_PASS)) return true;
  server.requestAuthentication();
  return false;
}

// chip helpers (same conservative logic as Lamp)
static bool isChipC3() {
  String m = String(ESP.getChipModel());
  m.toUpperCase();
  return (m.indexOf("C3") >= 0);
}

static bool isSafeGpioForNeoPixel(int pin) {
  if (pin < 0) return false;

  if (isChipC3()) {
    if (pin > 21) return false;
    if (pin == 18 || pin == 19) return false;
    return true;
  }

  if (pin >= 6 && pin <= 11) return false;
  if (pin >= 34 && pin <= 39) return false;
  if (pin == 0 || pin == 2 || pin == 12 || pin == 15) return false;
  if (pin == 1 || pin == 3) return false;

  return (pin <= 33);
}

// ---------- Pages ----------
static String pageStyle() {
  return
    "<style>"
    "body{font-family:Arial;background:#f2f2f2;margin:0;padding:16px}"
    ".card{background:#fff;padding:14px;border-radius:10px;box-shadow:0 2px 6px rgba(0,0,0,.12);max-width:860px;margin:auto}"
    "input,select,textarea,button{width:100%;padding:10px;margin-top:6px;border:1px solid #ccc;border-radius:8px;box-sizing:border-box}"
    "textarea{min-height:120px;font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace}"
    "label{font-weight:bold;display:block;margin-top:10px}"
    ".small{color:#555;font-size:13px;line-height:1.35}"
    ".row{display:flex;gap:8px;flex-wrap:wrap}"
    ".row>*{flex:1;min-width:220px}"
    ".badge{display:inline-block;padding:6px 10px;border-radius:999px;background:#111;color:#fff;font-size:12px}"
    ".btnrow{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}"
    ".btnrow button{flex:1;min-width:160px}"
    "a{color:#1a73e8;text-decoration:none}"
    "</style>";
}

static void handleRoot() {
  // Map provisioning gate
  bool ok = (cfg.provisioned && cfg.app_role.length() && cfg.app_role.equalsIgnoreCase("map"));

  String html =
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>METAR Map</title>" + pageStyle() +
    "</head><body><div class='card'>"
    "<h2>🗺️ METAR Map</h2>"
    "<p class='small'>mDNS: <b>metarmap.local</b> &nbsp; | &nbsp; LEDs: <b>" + String(cfg.led_count) + "</b> / 250</p>";

  if (!ok) {
    html +=
      "<p><span class='badge'>NOT PROVISIONED</span></p>"
      "<p class='small'>This firmware requires Factory provisioning.</p>"
      "<p class='small'>Expected in <code>/config.json</code>:</p>"
      "<pre style='background:#f7f7f7;padding:10px;border-radius:8px;overflow:auto'>"
      "\"device\": { \"provisioned\": true, \"app\": \"map\" }"
      "</pre>"
      "<p class='small'>SoftAP is running so you can reach this page.</p>"
      "</div></body></html>";
    server.send(200, "text/html", html);
    return;
  }

  html +=
    "<form method='POST' action='/save'>"
    "<label>Airport / Legend List (comma-separated)</label>"
    "<textarea name='map_list'>" + cfg.map_list + "</textarea>"
    "<p class='small'>Tokens supported: <b>ICAO</b> (KJFK), <b>SKIP</b>, and legend tokens <b>VFR</b>, <b>MVFR</b>, <b>IFR</b>, <b>LIFR</b>.</p>"
    "<p class='small'>Refresh is every <b>20 minutes</b>. Fallback radius <b>75nm</b>. If no data + no fallback: dim white.</p>"

    "<div class='row'>"
      "<div><label>Brightness (1-255)</label><input name='brightness' type='number' min='1' max='255' value='" + String(cfg.brightness) + "'></div>"
      "<div><label>Derived LED Count</label><input value='" + String(cfg.led_count) + "' disabled></div>"
    "</div>"

    "<div class='btnrow'>"
      "<button type='submit'>💾 Save</button>"
      "<button type='button' onclick=\"fetch('/refresh').then(()=>location.reload())\">🔄 Refresh Now</button>"
      "<button type='button' onclick=\"fetch('/reboot').then(()=>alert('Rebooting...'))\">♻️ Reboot</button>"
    "</div>"
    "</form>"

    "<hr>"
    "<h3>OTA Updates</h3>"
    "<p><span class='badge'>" + otaStatusLine + "</span></p>"
    "<p class='small'>Asset: " + String(otaAssetNameForThisChip()) + "</p>"
    "<p class='small'>Auto-update: <b>" + String(cfg.otaAutoUpdate ? "ON" : "OFF") + "</b> &nbsp; | &nbsp; Interval: <b>" + String(cfg.otaIntervalDays) + " days</b></p>"

    "<div class='row'>"
      "<div>"
        "<label>Auto-update</label>"
        "<select id='otaAuto'>"
          "<option value='off'" + String(cfg.otaAutoUpdate ? "" : " selected") + ">OFF</option>"
          "<option value='on'" + String(cfg.otaAutoUpdate ? " selected" : "") + ">ON</option>"
        "</select>"
      "</div>"
      "<div>"
        "<label>Interval (days)</label>"
        "<input id='otaDays' type='number' min='1' max='60' value='" + String(cfg.otaIntervalDays) + "'>"
      "</div>"
    "</div>"

    "<div class='btnrow'>"
      "<button type='button' id='otaSaveBtn'>💾 Save OTA Settings</button>"
      "<button type='button' id='otaCheckBtn'>🔍 Check Now</button>"
      "<button type='button' id='otaInstallBtn'>⬇️ Install Update</button>"
    "</div>"

    "<script>"
    "document.getElementById('otaCheckBtn').onclick=function(){fetch('/ota/check').then(()=>location.reload());};"
    "document.getElementById('otaInstallBtn').onclick=function(){"
      "fetch('/ota/install').then(r=>r.text()).then(t=>{alert(t); location.reload();});"
    "};"
    "document.getElementById('otaSaveBtn').onclick=function(){"
      "var auto=document.getElementById('otaAuto').value;"
      "var days=document.getElementById('otaDays').value;"
      "fetch('/ota/settings?auto='+encodeURIComponent(auto)+'&days='+encodeURIComponent(days))"
      ".then(()=>location.reload());"
    "};"
    "</script>"

    "<hr>"
    "<p><a href='/admin'>🔒 Admin</a></p>"
    "</div></body></html>";

  server.send(200, "text/html", html);
}

static void handleSave() {
  bool ok = (cfg.provisioned && cfg.app_role.length() && cfg.app_role.equalsIgnoreCase("map"));
  if (!ok) { server.send(403, "text/plain", "Not provisioned"); return; }

  cfg.map_list = server.arg("map_list");
  cfg.brightness = server.arg("brightness").toInt();
  if (cfg.brightness < 1) cfg.brightness = 1;
  if (cfg.brightness > 255) cfg.brightness = 255;

  if (!saveConfig()) { server.send(500, "text/plain", "Save failed"); return; }

  refreshNow();   // refreshNow() already parses token list + rebuilds strip + fetches
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "Saved");
}

static void handleRefresh() {
  bool ok = (cfg.provisioned && cfg.app_role.length() && cfg.app_role.equalsIgnoreCase("map"));
  if (!ok) { server.send(403, "text/plain", "Not provisioned"); return; }
  refreshNow();
  server.send(200, "text/plain", "OK");
}

static void handleReboot() {
  server.send(200, "text/plain", "Rebooting...");
  delay(300);
  ESP.restart();
}

// ---------- OTA endpoints (same pattern as Lamp) ----------
static void handleOtaCheck() {
  bool ok = otaCheckNow();
  server.send(ok ? 200 : 500, "text/plain", otaStatusLine);
}

static void handleOtaInstall() {
  bool ok = otaCheckNow();
  if (!ok) { server.send(500, "text/plain", otaStatusLine); return; }
  if (!otaUpdateAvailable) { server.send(200, "text/plain", "No update available"); return; }
  server.send(200, "text/plain", "Installing update... device will reboot.");
  delay(100);
  otaInstallNow(); // will reboot on success
}

static void handleOtaSettings() {
  String a = server.arg("auto"); a.trim(); a.toLowerCase();
  int days = server.arg("days").toInt();
  if (days < 1) days = 1;
  if (days > 60) days = 60;

  cfg.otaAutoUpdate = (a == "on");
  cfg.otaIntervalDays = days;

  if (!saveConfig()) { server.send(500, "text/plain", "Save failed"); return; }
  server.send(200, "text/plain", "Saved");
}

// ---------- Admin pages (LED setup + reboot + LED test) ----------
static void handleAdminHome() {
  if (!adminAuth()) return;

  String html =
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Admin</title>" + pageStyle() +
    "</head><body><div class='card'>"
    "<h2>🔒 Admin</h2>"
    "<a href='/admin/led'>LED Setup</a><br>"
    "<a href='/admin/reboot' onclick=\"return confirm('Reboot now?')\">Reboot Device</a><br>"
    "<hr>"
    "<p class='small'><b>LED:</b> Pin " + String(cfg.led_pin) + " | Count " + String(cfg.led_count) + " | Order " + cfg.led_order + "</p>"
    "<p><a href='/'>Back</a></p>"
    "</div></body></html>";

  server.send(200, "text/html", html);
}

static void handleAdminLed() {
  if (!adminAuth()) return;
  int maxPin = isChipC3() ? 21 : 33;

  String html =
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>LED Setup</title>" + pageStyle() +
    "</head><body><div class='card'>"
    "<h2>LED Setup</h2>"
    "<form method='POST' action='/admin/led/save'>"
    "<label>LED Pin (GPIO)</label>"
    "<input name='pin' type='number' min='0' max='" + String(maxPin) + "' value='" + String(cfg.led_pin) + "'>"

    "<label>Color Order</label>"
    "<select name='order'>"
      "<option value='RGB'" + String(cfg.led_order=="RGB"?" selected":"") + ">RGB</option>"
      "<option value='RBG'" + String(cfg.led_order=="RBG"?" selected":"") + ">RBG</option>"
      "<option value='GRB'" + String(cfg.led_order=="GRB"?" selected":"") + ">GRB</option>"
      "<option value='GBR'" + String(cfg.led_order=="GBR"?" selected":"") + ">GBR</option>"
      "<option value='BRG'" + String(cfg.led_order=="BRG"?" selected":"") + ">BRG</option>"
      "<option value='BGR'" + String(cfg.led_order=="BGR"?" selected":"") + ">BGR</option>"
    "</select>"

    "<p class='small'>LED count is derived from the token list (max 250).</p>"
    "<button type='submit'>Save</button>"
    "</form>"

    "<hr><h3>Test Colors</h3>"
    "<div class='btnrow'>"
      "<button type='button' onclick=\"fetch('/admin/led/test?c=red')\">Red</button>"
      "<button type='button' onclick=\"fetch('/admin/led/test?c=green')\">Green</button>"
      "<button type='button' onclick=\"fetch('/admin/led/test?c=blue')\">Blue</button>"
      "<button type='button' onclick=\"fetch('/admin/led/test?c=off')\">Off</button>"
    "</div>"
    "<p><a href='/admin'>Back</a></p>"
    "</div></body></html>";

  server.send(200, "text/html", html);
}

static void handleAdminLedSave() {
  if (!adminAuth()) return;

  int pin = server.arg("pin").toInt();
  String order = server.arg("order"); order.trim(); order.toUpperCase();

  if (!isSafeGpioForNeoPixel(pin)) { server.send(400, "text/plain", "Invalid/unsafe GPIO selected."); return; }
  if (!(order == "RGB" || order == "RBG" || order == "GRB" || order == "GBR" || order == "BRG" || order == "BGR")) {
    server.send(400, "text/plain", "Invalid color order.");
    return;
  }

  cfg.led_pin = pin;
  cfg.led_order = order;

  if (!saveConfig()) { server.send(500, "text/plain", "Save failed."); return; }

  rebuildStripFromConfig();
  server.send(200, "text/plain", "Saved.");
}

static void handleAdminLedTest() {
  if (!adminAuth()) return;

  String c = server.arg("c"); c.trim(); c.toLowerCase();
  if (c == "red") setLEDColor(255,0,0);
  else if (c == "green") setLEDColor(0,255,0);
  else if (c == "blue") setLEDColor(0,0,255);
  else if (c == "off") clearLED();
  else { server.send(400, "text/plain", "Bad color. Use c=red|green|blue|off"); return; }

  server.send(200, "text/plain", "OK");
}

static void handleAdminReboot() {
  if (!adminAuth()) return;
  server.send(200, "text/plain", "Rebooting...");
  delay(300);
  ESP.restart();
}

static void registerRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/refresh", HTTP_GET, handleRefresh);
  server.on("/reboot", HTTP_GET, handleReboot);

  server.on("/ota/check", HTTP_GET, handleOtaCheck);
  server.on("/ota/install", HTTP_GET, handleOtaInstall);
  server.on("/ota/settings", HTTP_GET, handleOtaSettings);

  server.on("/admin", HTTP_GET, handleAdminHome);
  server.on("/admin/led", HTTP_GET, handleAdminLed);
  server.on("/admin/led/save", HTTP_POST, handleAdminLedSave);
  server.on("/admin/led/test", HTTP_GET, handleAdminLedTest);
  server.on("/admin/reboot", HTTP_GET, handleAdminReboot);

  server.onNotFound([]() {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirect");
  });
}