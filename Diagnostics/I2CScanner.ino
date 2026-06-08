#include <Wire.h>

void scanI2C() {
  Serial.println();
  Serial.println("=== I2C SCAN START ===");

  int found = 0;

  for (uint8_t address = 1; address < 127; address++) {
    Wire.beginTransmission(address);

    if (Wire.endTransmission() == 0) {
      Serial.print("Found device at 0x");

      if (address < 16) {
        Serial.print("0");
      }

      Serial.println(address, HEX);
      found++;
    }
  }

  if (found == 0) {
    Serial.println("No I2C devices found");
  }

  Serial.println("=== I2C SCAN END ===");
  Serial.println();
}
