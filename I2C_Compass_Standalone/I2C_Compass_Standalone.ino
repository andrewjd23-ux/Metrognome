// I2C_Compass_Standalone.ino
// Standalone serial diagnostic for ESP32-C3 + HP5883/HMC5883/QMC5883-style compass.
// Now includes a GPIO pin sweep to find devices on unexpected SDA/SCL pairs.
//
// No Metrognome display code. No WiFi. No secrets.h.
// Serial Monitor: 115200 baud.

#include <Arduino.h>
#include <Wire.h>

constexpr uint8_t DEFAULT_SDA_PIN = 5;
constexpr uint8_t DEFAULT_SCL_PIN = 6;

constexpr uint8_t QMC_ADDR = 0x0D;
constexpr uint8_t HMC_ADDR = 0x1E;

const uint8_t candidatePins[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 21};
const size_t candidateCount = sizeof(candidatePins) / sizeof(candidatePins[0]);

uint8_t compassAddr = 0;
uint8_t activeSda = DEFAULT_SDA_PIN;
uint8_t activeScl = DEFAULT_SCL_PIN;

void printHex2(uint8_t value) {
  if (value < 16) Serial.print("0");
  Serial.print(value, HEX);
}

const char* knownDevice(uint8_t address) {
  switch (address) {
    case 0x0D: return "QMC5883L candidate";
    case 0x1E: return "HMC5883L / HP5883 candidate";
    case 0x3C: return "SSD1306 OLED candidate";
    case 0x3D: return "SSD1306 alternate OLED candidate";
    default: return "unknown";
  }
}

bool scanAddress(uint8_t address) {
  Wire.beginTransmission(address);
  uint8_t error = Wire.endTransmission();
  return error == 0;
}

void scanCurrentBus(const char* label) {
  Serial.println();
  Serial.print("=== I2C SCAN START: ");
  Serial.print(label);
  Serial.print(" SDA GPIO");
  Serial.print(activeSda);
  Serial.print(" SCL GPIO");
  Serial.print(activeScl);
  Serial.println(" ===");

  int found = 0;
  compassAddr = 0;

  for (uint8_t address = 1; address < 127; address++) {
    if (scanAddress(address)) {
      Serial.print("Found device at 0x");
      printHex2(address);
      Serial.print("  ");
      Serial.println(knownDevice(address));
      found++;

      if (address == QMC_ADDR || address == HMC_ADDR) compassAddr = address;
    }
  }

  if (found == 0) Serial.println("No I2C devices found");
  if (compassAddr == QMC_ADDR) Serial.println("Compass candidate: QMC5883L at 0x0D");
  else if (compassAddr == HMC_ADDR) Serial.println("Compass candidate: HMC5883L / HP5883 at 0x1E");
  else Serial.println("No compass candidate found at 0x0D or 0x1E");

  Serial.println("=== I2C SCAN END ===");
  Serial.println();
}

void tryRegisterProbe(uint8_t address) {
  Serial.print("    probing registers at 0x");
  printHex2(address);
  Serial.println();

  for (uint8_t reg = 0; reg < 8; reg++) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    uint8_t err = Wire.endTransmission(false);

    Serial.print("    reg 0x");
    printHex2(reg);
    Serial.print(" -> ");

    if (err != 0) {
      Serial.print("pointer write err ");
      Serial.println(err);
      continue;
    }

    uint8_t got = Wire.requestFrom(address, (uint8_t)1);
    if (got == 1 && Wire.available()) {
      Serial.print("0x");
      printHex2(Wire.read());
      Serial.println();
    } else {
      Serial.println("no byte");
    }
  }
}

void scanPair(uint8_t sda, uint8_t scl, uint32_t clockHz) {
  if (sda == scl) return;

  Wire.end();
  delay(10);
  Wire.begin(sda, scl);
  Wire.setClock(clockHz);
  activeSda = sda;
  activeScl = scl;
  delay(25);

  bool pairFound = false;

  for (uint8_t address = 1; address < 127; address++) {
    if (scanAddress(address)) {
      if (!pairFound) {
        Serial.println();
        Serial.print("PAIR SDA GPIO");
        Serial.print(sda);
        Serial.print(" / SCL GPIO");
        Serial.print(scl);
        Serial.print(" @ ");
        Serial.print(clockHz);
        Serial.println(" Hz");
        pairFound = true;
      }

      Serial.print("  ACK at 0x");
      printHex2(address);
      Serial.print("  ");
      Serial.println(knownDevice(address));

      if (address == QMC_ADDR || address == HMC_ADDR) {
        compassAddr = address;
        tryRegisterProbe(address);
      }
    }
  }
}

void runPinSweep() {
  Serial.println();
  Serial.println("==============================");
  Serial.println("ESP32-C3 I2C PIN SWEEP START");
  Serial.println("Trying candidate SDA/SCL GPIO pairs");
  Serial.println("==============================");

  bool foundAny = false;
  const uint32_t speeds[] = {100000, 50000};

  for (uint8_t speedIndex = 0; speedIndex < 2; speedIndex++) {
    uint32_t speed = speeds[speedIndex];
    Serial.println();
    Serial.print("--- SCANNING AT ");
    Serial.print(speed);
    Serial.println(" Hz ---");

    for (size_t i = 0; i < candidateCount; i++) {
      for (size_t j = 0; j < candidateCount; j++) {
        uint8_t sda = candidatePins[i];
        uint8_t scl = candidatePins[j];
        if (sda == scl) continue;

        uint32_t before = millis();
        scanPair(sda, scl, speed);
        if (millis() - before > 0) foundAny = true;
        delay(5);
      }
    }
  }

  Serial.println();
  Serial.println("==============================");
  Serial.println("PIN SWEEP COMPLETE");
  Serial.println("Only lines beginning PAIR found an ACKing device.");
  Serial.println("If you only see the OLED at 0x3C and never 0x1E/0x0D, the compass is not answering.");
  Serial.println("==============================");
  Serial.println();
}

void writeReg(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
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
  writeReg(QMC_ADDR, 0x0B, 0x01);
  writeReg(QMC_ADDR, 0x09, 0b00011101);
  delay(20);
}

void initHMC() {
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
    Serial.println("No compass address selected. Nothing to read.");
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
  printHex2(compassAddr);
  Serial.print(" on SDA GPIO");
  Serial.print(activeSda);
  Serial.print(" SCL GPIO");
  Serial.print(activeScl);
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
  delay(2000);

  Serial.println();
  Serial.println("I2C Compass Standalone Diagnostic with GPIO Pin Sweep");
  Serial.println("Serial Monitor: 115200 baud");

  Wire.begin(DEFAULT_SDA_PIN, DEFAULT_SCL_PIN);
  Wire.setClock(100000);
  activeSda = DEFAULT_SDA_PIN;
  activeScl = DEFAULT_SCL_PIN;

  scanCurrentBus("DEFAULT BUS");
  if (compassAddr == QMC_ADDR) initQMC();
  else if (compassAddr == HMC_ADDR) initHMC();

  runPinSweep();

  // Return to the default bus after the sweep.
  Wire.end();
  delay(10);
  Wire.begin(DEFAULT_SDA_PIN, DEFAULT_SCL_PIN);
  Wire.setClock(100000);
  activeSda = DEFAULT_SDA_PIN;
  activeScl = DEFAULT_SCL_PIN;
  scanCurrentBus("DEFAULT BUS AFTER SWEEP");
}

void loop() {
  printCompassRead();
  delay(1000);
}
