// QMC_0x2C_Test.ino
// Minimal QMC-style test against the compass-like device at I2C address 0x2C.
// Serial Monitor: 115200 baud. CDC on boot must be enabled.
// Wiring: SDA -> GPIO5, SCL -> GPIO6, VCC -> 3V3, GND -> GND, optional DRDY -> GPIO20/RX.

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

constexpr uint8_t SDA_PIN = 5;
constexpr uint8_t SCL_PIN = 6;
constexpr uint8_t DRDY_PIN = 20;
constexpr uint8_t ADDR = 0x2C;

void h2(uint8_t v) {
  if (v < 16) Serial.print('0');
  Serial.print(v, HEX);
}

bool ack(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readReg(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t got = Wire.requestFrom(ADDR, (uint8_t)1);
  if (got != 1 || !Wire.available()) return false;
  value = Wire.read();
  return true;
}

bool readBlock(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t got = Wire.requestFrom(ADDR, len);
  if (got != len) return false;
  for (uint8_t i = 0; i < len; i++) {
    if (!Wire.available()) return false;
    buf[i] = Wire.read();
  }
  return true;
}

int16_t le16(uint8_t lo, uint8_t hi) {
  return (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
}

float norm360(float x) {
  while (x < 0) x += 360;
  while (x >= 360) x -= 360;
  return x;
}

void scanBus() {
  Serial.println("\n=== BUS SCAN ===");
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    if (ack(a)) {
      Serial.print("ACK 0x"); h2(a);
      if (a == ADDR) Serial.print(" target");
      if (a == 0x3C) Serial.print(" OLED");
      if (a == 0x1E) Serial.print(" HMC address");
      if (a == 0x0D) Serial.print(" QMC address");
      Serial.println();
      found++;
    }
  }
  if (!found) Serial.println("No I2C devices found");
  Serial.println("=== END BUS SCAN ===");
}

void dumpCore() {
  Serial.println("\n=== CORE REGISTER SNAPSHOT ===");
  for (uint8_t r = 0; r <= 12; r++) {
    uint8_t v = 0;
    Serial.print("0x"); h2(r); Serial.print(" = ");
    if (readReg(r, v)) {
      Serial.print("0x"); h2(v);
    } else {
      Serial.print("read failed");
    }
    Serial.println();
  }
}

void writeAndReport(uint8_t reg, uint8_t value, const char *label) {
  Serial.print("Write "); Serial.print(label); Serial.print(" 0x"); h2(reg);
  Serial.print(" <- 0x"); h2(value); Serial.print(" ... ");
  bool ok = writeReg(reg, value);
  delay(30);
  uint8_t rb = 0;
  bool rok = readReg(reg, rb);
  Serial.print(ok ? "writeOK " : "writeFAIL ");
  if (rok) {
    Serial.print("readback 0x"); h2(rb);
  } else {
    Serial.print("readback FAIL");
  }
  Serial.println();
}

void qmcInit() {
  Serial.println("\n=== QMC-STYLE INIT AGAINST 0x2C ===");
  writeAndReport(0x0B, 0x01, "SET/RESET");
  delay(50);
  writeAndReport(0x09, 0x1D, "CTRL1");
  delay(100);
}

void readQmcWindow() {
  uint8_t b[6];
  Serial.println("\n=== QMC WINDOW 0x00..0x05 ===");
  if (!readBlock(0x00, b, 6)) {
    Serial.println("Read failed");
    return;
  }

  Serial.print("Raw: ");
  for (uint8_t i = 0; i < 6; i++) {
    Serial.print("0x"); h2(b[i]); Serial.print(' ');
  }
  Serial.println();

  int16_t x = le16(b[0], b[1]);
  int16_t y = le16(b[2], b[3]);
  int16_t z = le16(b[4], b[5]);
  float heading = norm360(atan2((float)y, (float)x) * 180.0 / PI);

  Serial.print("QMC X="); Serial.print(x);
  Serial.print(" Y="); Serial.print(y);
  Serial.print(" Z="); Serial.print(z);
  Serial.print(" heading="); Serial.println(heading, 1);

  uint8_t status = 0;
  if (readReg(0x06, status)) {
    Serial.print("QMC status 0x06 = 0x"); h2(status);
    Serial.print(" DRDYbit="); Serial.print((status & 0x01) ? 1 : 0);
    Serial.print(" OVFbit="); Serial.print((status & 0x02) ? 1 : 0);
    Serial.print(" DORbit="); Serial.println((status & 0x04) ? 1 : 0);
  }

  Serial.print("DRDY GPIO20 = ");
  Serial.println(digitalRead(DRDY_PIN));
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("QMC_0x2C_Test v1");
  Serial.println("Rotate board and bring magnet near it while output scrolls.");

  pinMode(DRDY_PIN, INPUT);
  delay(1000);
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(300);
  Wire.setClock(10000);

  scanBus();
  if (!ack(ADDR)) {
    Serial.println("Target 0x2C not visible. Check wiring.");
    return;
  }

  dumpCore();
  qmcInit();
  dumpCore();
  readQmcWindow();
}

void loop() {
  if (!ack(ADDR)) {
    Serial.println("0x2C not ACKing now.");
    delay(1500);
    return;
  }
  readQmcWindow();
  delay(1000);
}
