#pragma once
#include <Arduino.h>

struct AppConfig {
  // provisioned by Factory
  String device_ssid = "METARLightworks";
  String avwx_token  = "";
  String wifi_ssid   = "";
  String wifi_pass   = "";

  // settings
  String airport_code = "KTIX";
  int brightness = 100; // 3..100

  // schedule
  bool scheduleEnabled = false;
  int startHour = 0, startMinute = 0;
  int endHour   = 23, endMinute  = 59;
  String timezonePref = "UTC0";

  // mode
  int displayMode = 0; // 0..5

  // flight pulse
  bool fpEnabled = false;
  String fpIcao = ""; // 6 hex
  String fpTail = ""; // N-number

  // OTA prefs
  bool otaCheckOnBoot  = true;
  bool otaAutoUpdate   = false;
  int  otaIntervalDays = 7;

  // advanced LED (admin)
  int led_pin = 5;
  int led_count = 1;
  String led_order = "RGB"; // RGB/GRB/BRG
};
