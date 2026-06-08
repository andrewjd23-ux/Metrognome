// I2C_Compass_Standalone.ino
// Standalone serial diagnostic for ESP32-C3 + mystery HP5883/HMC5883 module.
// Focuses on the odd device seen at 0x2C when using SDA GPIO6 / SCL GPIO5.
//
// No Metrognome display code. No WiFi. No secrets.h.
// Serial Monitor: 115200 baud.

#include <Arduino.h>
#include <Wire.h>

constexpr uint8_t SDA_PIN = 6;
constexpr uint8_t SCL_PIN = 5;
constexpr uint8_t MYSTERY_ADDR = 0x2C;

uint8_t lastDump[64];
bool haveLastDump = false;

void printHex2(uint8_t value) {
  if (value < 16) Serial.print("0");
  Serial.print(value, HEX);
}

bool scanAddress(uint8_t address) {
  Wire.beginTransmission(address);
  uint8_t error = Wire.endTransmission();
  return error == 0;
}

void scanBus() {
  Serial.println();
  Serial.print("=== I2C SCAN SDA GPIO");
  Serial.print(SDA_PIN);
  Serial.print(" / SCL GPIO");
  Serial.print(SCL_PIN);
  Serial.println(" ===");

  int found = 0;
  for (uint8_t address = 1; address < 127; address++) {
    if (scanAddress(address)) {
      Serial.print("ACK at 0x");
      printHex2(address);
      if (address == 0x2C) Serial.print("  MYSTERY DEVICE");
      if (address == 0x3C) Serial.print("  OLED");
      if (address == 0x1E) Serial.print("  HMC/HP candidate");
      if (address == 0x0D) Serial.print("  QMC candidate");
      Serial.println();
      found++;
    }
  }

  if (found == 0) Serial.println("No I2C devices found");
  Serial.println("=== SCAN END ===");
}

bool readRegByte(uint8_t addr, uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  uint8_t err = Wire.endTransmission(false);
  if (err != 0) return false;

  uint8_t got = Wire.requestFrom(addr, (uint8_t)1);
  if (got != 1 || !Wire.available()) return false;

  value = Wire.read();
  return true;
}

void writeRegByte(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

void dumpRegisters(uint8_t addr) {
  Serial.println();
  Serial.print("=== REGISTER DUMP 0x");
  printHex2(addr);
  Serial.println(" regs 0x00-0x3F ===");

  uint8_t currentDump[64];
  bool readOk[64];

  for (uint8_t reg = 0; reg < 64; reg++) {
    uint8_t value = 0x00;
    bool ok = readRegByte(addr, reg, value);
    currentDump[reg] = value;
    readOk[reg] = ok;
  }

  for (uint8_t row = 0; row < 4; row++) {
    uint8_t start = row * 16;
    Serial.print("0x");
    printHex2(start);
    Serial.print(": ");

    for (uint8_t i = 0; i < 16; i++) {
      uint8_t reg = start + i;
      if (readOk[reg]) {
        Serial.print("0x");
        printHex2(currentDump[reg]);
      } else {
        Serial.print("----");
      }

      if (haveLastDump && readOk[reg] && currentDump[reg] != lastDump[reg]) {
        Serial.print("*");
      } else {
        Serial.print(" ");
      }
    }
    Serial.println();
  }

  memcpy(lastDump, currentDump, 64);
  haveLastDump = true;
  Serial.println("* means changed since last dump");
  Serial.println("=== DUMP END ===");
}

void tryKnownInitSequences(uint8_t addr) {
  Serial.println();
  Serial.println("Trying known compass init sequences against 0x2C...");

  Serial.println("Trying HMC-style init registers 0x00/0x01/0x02");
  writeRegByte(addr, 0x00, 0x70);
  writeRegByte(addr, 0x01, 0x20);
  writeRegByte(addr, 0x02, 0x00);
  delay(100);
  dumpRegisters(addr);

  Serial.println("Trying QMC-style init registers 0x0B/0x09");
  writeRegByte(addr, 0x0B, 0x01);
  writeRegByte(addr, 0x09, 0b00011101);
  delay(100);
  dumpRegisters(addr);
}

void tryRawBurstRead(uint8_t addr) {
  Serial.println();
  Serial.print("Raw requestFrom 0x");
  printHex2(addr);
  Serial.println(" without register pointer:");

  uint8_t got = Wire.requestFrom(addr, (uint8_t)16);
  Serial.print("got ");
  Serial.print(got);
  Serial.print(" bytes: ");

  while (Wire.available()) {
    uint8_t b = Wire.read();
    Serial.print("0x");
    printHex2(b);
    Serial.print(" ");
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("Mystery 0x2C I2C Interrogator");
  Serial.println("Expected wiring for this test:");
  Serial.println("  Compass SDA -> GPIO6");
  Serial.println("  Compass SCL -> GPIO5");
  Serial.println("  Compass VCC -> 3V3");
  Serial.println("  Compass GND -> GND");
  Serial.println("Serial Monitor: 115200 baud");

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(50000);

  scanBus();

  if (scanAddress(MYSTERY_ADDR)) {
    Serial.println("0x2C ACKed. Interrogating it now.");
    dumpRegisters(MYSTERY_ADDR);
    tryRawBurstRead(MYSTERY_ADDR);
    tryKnownInitSequences(MYSTERY_ADDR);
  } else {
    Serial.println("0x2C did not ACK on this boot.");
  }
}

void loop() {
  if (scanAddress(MYSTERY_ADDR)) {
    dumpRegisters(MYSTERY_ADDR);
    tryRawBurstRead(MYSTERY_ADDR);
  } else {
    Serial.println("0x2C not ACKing now.");
  }

  delay(1500);
}
