#include "version.h"

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.print("METARLightworks FW: ");
  Serial.println(FW_VERSION);
}

void loop() {
  delay(1000);
}
