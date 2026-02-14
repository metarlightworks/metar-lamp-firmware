#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <ESP.h>
#include "AppTypes.h"

// These are defined in your .ino (global objects/functions)
extern WebServer server;
extern AppConfig cfg;
extern bool saveConfig();

// These exist in your .ino (your code already has them)
extern void setLEDColor(uint8_t r, uint8_t g, uint8_t b);
extern void clearLED();

// ---------- Basic Auth ----------
static const char* ADMIN_USER = "admin";
static const char* ADMIN_PASS = "north";  // change anytime

static bool adminAuth() {
  if (server.authenticate(ADMIN_USER, ADMIN_PASS)) return true;
  server.requestAuthentication();
  return false;
}

static bool isChipC3() {
  String m = String(ESP.getChipModel());
  m.toUpperCase();
  return (m.indexOf("C3") >= 0);
}

// Conservative pin check by chip
static bool isSafeGpioForNeoPixel(int pin) {
  if (pin < 0) return false;

  if (isChipC3()) {
    // ESP32-C3: conservative 0..21, block common USB/JTAG-ish pins
    if (pin > 21) return false;
    if (pin == 18 || pin == 19) return false;
    return true;
  }

  // OG ESP32: block known-bad pins
  // - GPIO 6..11 = flash pins (no)
  // - GPIO 34..39 = input-only (no)
  // - strapping pins (0,2,12,15) can work but are risky -> block
  // - 1/3 are UART -> block to avoid confusing serial output
  if (pin >= 6 && pin <= 11) return false;
  if (pin >= 34 && pin <= 39) return false;
  if (pin == 0 || pin == 2 || pin == 12 || pin == 15) return false;
  if (pin == 1 || pin == 3) return false;

  return (pin <= 33);
}

// -------------------- Admin Pages --------------------

static void handleAdminHome() {
  if (!adminAuth()) return;

  String html =
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Admin</title>"
    "<style>body{font-family:Arial;background:#f2f2f2;margin:0;padding:16px}"
    ".card{background:#fff;padding:14px;border-radius:10px;box-shadow:0 2px 6px rgba(0,0,0,.12);max-width:720px;margin:auto}"
    "a{display:inline-block;margin:8px 0}</style>"
    "</head><body><div class='card'>"
    "<h2>🔒 Admin</h2>"
    "<a href='/admin/led'>LED Setup</a><br>"
    "<a href='/admin/reboot' onclick=\"return confirm('Reboot now?')\">Reboot Device</a><br>"
    "<hr>"
    "<p><b>Current LED:</b><br>"
    "Pin: " + String(cfg.led_pin) + "<br>" +
    "Count: " + String(cfg.led_count) + "<br>" +
    "Order: " + cfg.led_order + "</p>"
    "<p><a href='/' target='_self'>Back to Main UI</a></p>"
    "</div></body></html>";

  server.send(200, "text/html", html);
}

static void handleAdminLed() {
  if (!adminAuth()) return;

  int maxPin = isChipC3() ? 21 : 33;

  String html =
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>LED Setup</title>"
    "<style>body{font-family:Arial;background:#f2f2f2;margin:0;padding:16px}"
    ".card{background:#fff;padding:14px;border-radius:10px;box-shadow:0 2px 6px rgba(0,0,0,.12);max-width:720px;margin:auto}"
    "input,select,button{width:100%;padding:10px;margin-top:6px;border:1px solid #ccc;border-radius:8px}"
    "label{font-weight:bold;display:block;margin-top:10px}"
    ".small{color:#555;font-size:13px;line-height:1.35}"
    ".row{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}"
    ".row button{flex:1;min-width:120px}</style>"
    "</head><body><div class='card'>"
    "<h2>LED Setup</h2>"
    "<form method='POST' action='/admin/led/save'>"

    "<label>LED Pin (GPIO)</label>"
    "<input name='pin' type='number' min='0' max='" + String(maxPin) + "' value='" + String(cfg.led_pin) + "'>"

    "<label>LED Count</label>"
    "<input name='count' type='number' min='1' max='300' value='" + String(cfg.led_count) + "'>"

    "<label>Color Order</label>"
    "<select name='order'>"
      "<option value='RGB'" + String(cfg.led_order=="RGB"?" selected":"") + ">RGB</option>"
      "<option value='RBG'" + String(cfg.led_order=="RBG"?" selected":"") + ">RBG</option>"
      "<option value='GRB'" + String(cfg.led_order=="GRB"?" selected":"") + ">GRB</option>"
      "<option value='GBR'" + String(cfg.led_order=="GBR"?" selected":"") + ">GBR</option>"
      "<option value='BRG'" + String(cfg.led_order=="BRG"?" selected":"") + ">BRG</option>"
      "<option value='BGR'" + String(cfg.led_order=="BGR"?" selected":"") + ">BGR</option>"
    "</select>"

    "<p class='small'>These changes apply after reboot. If you pick a bad GPIO, the LED may stop responding.</p>"
    "<button type='submit'>Save</button>"
    "</form>"

    "<hr>"
    "<h3>Test LED Colors</h3>"
    "<p class='small'>Use these to verify channel order. Red/Green/Blue should look correct.</p>"
    "<div class='row'>"
      "<button type='button' onclick=\"fetch('/admin/led/test?c=red')\">Red</button>"
      "<button type='button' onclick=\"fetch('/admin/led/test?c=green')\">Green</button>"
      "<button type='button' onclick=\"fetch('/admin/led/test?c=blue')\">Blue</button>"
      "<button type='button' onclick=\"fetch('/admin/led/test?c=off')\">Off</button>"
    "</div>"
    "<div class='row'>"
      "<button type='button' onclick='cycleRGB()'>Cycle RGB</button>"
    "</div>"

    "<script>"
    "function sleep(ms){return new Promise(r=>setTimeout(r,ms));}"
    "async function cycleRGB(){"
      "await fetch('/admin/led/test?c=red'); await sleep(700);"
      "await fetch('/admin/led/test?c=green'); await sleep(700);"
      "await fetch('/admin/led/test?c=blue'); await sleep(700);"
      "await fetch('/admin/led/test?c=off');"
    "}"
    "</script>"

    "<p style='margin-top:12px;'><a href='/admin'>Back</a></p>"
    "</div></body></html>";

  server.send(200, "text/html", html);
}

static void handleAdminLedSave() {
  if (!adminAuth()) return;

  int pin = server.arg("pin").toInt();
  int count = server.arg("count").toInt();
  String order = server.arg("order");
  order.trim(); order.toUpperCase();

  if (!isSafeGpioForNeoPixel(pin)) {
    server.send(400, "text/plain", "Invalid/unsafe GPIO selected.");
    return;
  }
  if (count < 1 || count > 300) {
    server.send(400, "text/plain", "Invalid LED count.");
    return;
  }
  if (!(order == "RGB" || order == "RBG" || order == "GRB" || order == "GBR" || order == "BRG" || order == "BGR")) {
    server.send(400, "text/plain", "Invalid color order.");
    return;
  }

  cfg.led_pin = pin;
  cfg.led_count = count;
  cfg.led_order = order;

  if (!saveConfig()) {
    server.send(500, "text/plain", "Save failed.");
    return;
  }

  server.send(200, "text/plain", "Saved. Reboot required for LED changes.");
}

// NEW: Test endpoint
static void handleAdminLedTest() {
  if (!adminAuth()) return;

  String c = server.arg("c");
  c.trim(); c.toLowerCase();

  if (c == "red")      setLEDColor(255, 0,   0);
  else if (c == "green") setLEDColor(0,   255, 0);
  else if (c == "blue")  setLEDColor(0,   0,   255);
  else if (c == "off")   clearLED();
  else {
    server.send(400, "text/plain", "Bad color. Use c=red|green|blue|off");
    return;
  }

  server.send(200, "text/plain", "OK");
}

static void handleAdminReboot() {
  if (!adminAuth()) return;
  server.send(200, "text/plain", "Rebooting...");
  delay(300);
  ESP.restart();
}

static void registerAdminRoutes() {
  server.on("/admin", HTTP_GET, handleAdminHome);
  server.on("/admin/led", HTTP_GET, handleAdminLed);
  server.on("/admin/led/save", HTTP_POST, handleAdminLedSave);
  server.on("/admin/led/test", HTTP_GET, handleAdminLedTest);   // NEW
  server.on("/admin/reboot", HTTP_GET, handleAdminReboot);
}