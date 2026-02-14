#pragma once
#include <Arduino.h>

struct AppConfig {
  // provisioned by Factory
  String device_ssid = "METARMapworks";
  String wifi_ssid   = "";
  String wifi_pass   = "";

  // Map provisioning stamp (Map app requires this)
  bool   provisioned = false;
  String app_role    = "";  // should be "map"

  // Map settings
  String map_list    = "VFR,MVFR,IFR,LIFR,SKIP";
  int    brightness  = 120; // 1..255

  // LED (admin)
  int    led_pin     = 5;
  int    led_count   = 1;        // derived from token count, saved for visibility
  String led_order   = "GRB";    // RGB/GRB/...

  // OTA prefs (same workflow as Lamp)
  bool otaCheckOnBoot  = true;
  bool otaAutoUpdate   = false;
  int  otaIntervalDays = 7;      // 1..60
};