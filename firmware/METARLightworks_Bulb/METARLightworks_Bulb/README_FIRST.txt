METAR Lightworks Bulb Production v0.2.3

Minimum-change continuation of v0.2.2 Safe Architecture.

Default hardware profile: E27
E27:  R6 G7 B5 CW3 WW4
GU10: R3 G4 B5 CW6 WW7

NVS namespace: ktixbasic
NVS key: bulb_model
If absent, Production defaults to E27.

Admin now includes Bulb Type E27/GU10 and diagnostics report model/PWM map.

Final partitions:
prod 0x010000 0x1C0000 ota_0
safe 0x1D0000 0x1C0000 factory
nvs 0x390000 0x070000

Production direct OTA writing remains removed. Update Firmware requests Safe Mode.
Seven-power-cycle recovery and ~30s healthy clear remain in place.

Compile with ESP32C3 Dev Module, Arduino-ESP32 3.3.11, USB CDC Disabled, 160 MHz CPU, 80 MHz flash, DIO, 4 MB, no PSRAM.

RELEASE
-------
1. In Arduino IDE, compile/export the application binary with the ESP32-C3 settings above (including DIO).
2. From the repository root, run:

   ./firmware/METARLightworks_Bulb/release-bulb.sh

The command reads `version.h`, uses only `METARLightworks_Bulb.ino.bin`, and creates or updates the matching GitHub release asset. To validate a local export without publishing, add `--check`.


v0.2.4 CUSTOMER UI BRANDING
---------------------------
Customer-facing local web UI was restyled to echo metarlightworks.com:
- dark aviation-themed presentation
- METAR LightWorks wordmark treatment
- VFR / MVFR / IFR / LIFR color chips
- cleaner cards, inputs and buttons
- customer-friendly Wi-Fi setup heading
- no external fonts, images, scripts or web dependencies

This is visual-only. Firmware behavior and Safe Mode architecture are unchanged.


v0.2.4a COMPILE FIX
-------------------
No functional change.

Added explicit prototypes for renderChannels() and setBaseOutput() after the
BulbChannels struct declaration. This prevents the Arduino .ino preprocessor
from generating prototypes before the custom type is known.
