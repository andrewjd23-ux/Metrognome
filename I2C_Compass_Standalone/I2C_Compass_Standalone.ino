// I2C_Compass_Standalone.ino
// Standalone serial diagnostic for ESP32-C3 + QMC5883/HMC5883-style compass.
// No Metrognome display code. No WiFi. No secrets.h.
//
// Wiring expected:
//   VCC -> 3V3
//   GND -> GND
//   SDA -> GPIO5
//   SCL -> GPIO6
//   DRDY -> not connected
//
// Serial Monitor: 115200 baud.

#include <Arduino.h>
#include <Wire.h>

constexpr uint8_t SDA_PIN = 5;
constexpr uint8_t SCL_PIN = 6;

constexpr uint8_t QMC_ADDR = 0x0D;
constexpr uint8_t HMC_ADDR = 0x1E;

uint8_t compassAddr = 0;

void scanI2C() {
  Serial.println();
  Serial.println("=== I2C SCAN START ===");

  int found = 0;
  compassAddr = 0;

  for (uint8_t address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    uint8_t error = Wire.endTransmission();

    if (error == 0) {
      Serial.print("Found device at 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
      found++;

      if (address == QMC_ADDR || address == HMC_ADDR) {
        compassAddr = address;
      }
    }
  }

  if (found == 0) Serial.println("No I2C devices found");
  if (compassAddr == QMC_ADDR) Serial.println("Compass candidate: QMC5883L at 0x0D");
  else if (compassAddr == HMC_ADDR) Serial.println("Compass candidate: HMC5883L at 0x1E");
  else Serial.println("No compass candidate found at 0x0D or 0x1E");

  Serial.println("=== I2C SCAN END ===");
  Serial.println();
}

void writeReg(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

uint8_t readReg(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  Wire.requestFrom(addr, (uint8_t)1);
  if (Wire.available()) return Wire.read();
  return 0xFF;
}

bool readBytes(uint8_t addr, uint8_t reg, uint8_t* buffer, uint8_t length) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t got = Wire.requestFrom(addr, length);
  if (got != length) return false;
  for (uint8_t i = 0; i < length; i++) buffer[i] = Wire.read();
  return true;
}

void initQMC() {
  // QMC5883L common init:
  // 0x0B set/reset period, 0x09 continuous mode, 200Hz, 8G, 512 OSR-ish.
  writeReg(QMC_ADDR, 0x0B, 0x01);
  writeReg(QMC_ADDR, 0x09, 0b00011101);
  delay(20);
}

void initHMC() {
  // HMC5883L common init:
  // CRA 8-average 15Hz, CRB gain, mode continuous.
  writeReg(HMC_ADDR, 0x00, 0x70);
  writeReg(HMC_ADDR, 0x01, 0x20);
  writeReg(HMC_ADDR, 0x02, 0x00);
  delay(20);
}

bool readQMC(int16_t &x, int16_t &y, int16_t &z) {
  uint8_t b[6];
  if (!readBytes(QMC_ADDR, 0x00, b, 6)) return false;
  x = (int16_t)(b[0] | (b[1] << 8));
  y = (int16_t)(b[2] | (b[3] << 8));
  z = (int16_t)(b[4] | (b[5] << 8));
  return true;
}

bool readHMC(int16_t &x, int16_t &y, int16_t &z) {
  uint8_t b[6];
  if (!readBytes(HMC_ADDR, 0x03, b, 6)) return false;
  x = (int16_t)((b[0] << 8) | b[1]);
  z = (int16_t)((b[2] << 8) | b[3]);
  y = (int16_t)((b[4] << 8) | b[5]);
  return true;
}

float normalize360(float angle) {
  while (angle < 0.0) angle += 360.0;
  while (angle >= 360.0) angle -= 360.0;
  return angle;
}

const char* cardinal(float heading) {
  const char* dirs[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  int index = (int)((normalize360(heading) + 22.5) / 45.0) % 8;
  return dirs[index];
}

void printCompassRead() {
  if (compassAddr == 0) {
    Serial.println("No compass address selected. Rescanning...");
    scanI2C();
    delay(1000);
    return;
  }

  int16_t x = 0, y = 0, z = 0;
  bool ok = false;

  if (compassAddr == QMC_ADDR) ok = readQMC(x, y, z);
  else if (compassAddr == HMC_ADDR) ok = readHMC(x, y, z);

  if (!ok) {
    Serial.println("Compass read failed");
    return;
  }

  float heading = atan2((float)y, (float)x) * 180.0 / PI;
  heading = normalize360(heading);

  Serial.print("ADDR 0x");
  if (compassAddr < 16) Serial.print("0");
  Serial.print(compassAddr, HEX);
  Serial.print(" | X ");
  Serial.print(x);
  Serial.print(" | Y ");
  Serial.print(y);
  Serial.print(" | Z ");
  Serial.print(z);
  Serial.print(" | Heading ");
  Serial.print(heading, 1);
  Serial.print(" deg ");
  Serial.println(cardinal(heading));
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("I2C Compass Standalone Diagnostic");
  Serial.println("SDA GPIO5, SCL GPIO6, baud 115200");

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);

  scanI2C();

  if (compassAddr == QMC_ADDR) initQMC();
  else if (compassAddr == HMC_ADDR) initHMC();
}

void loop() {
  printCompassRead();
  delay(500);
}
