// Compass_TruthSerum.ino
// Fresh ESP32-C3 compass diagnostic now that CDC/Serial is known-good.
// Serial Monitor: 115200 baud.
// Try with compass on SDA GPIO5 / SCL GPIO6 first.
// If needed, swap to SDA GPIO6 / SCL GPIO5 by changing the two constants below.

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

constexpr uint8_t SDA_PIN = 5;
constexpr uint8_t SCL_PIN = 6;

const uint8_t addrs[] = {0x0D, 0x1E, 0x2C, 0x3C};
const uint8_t windows[] = {0x00, 0x03, 0x06, 0x09, 0x10, 0x20, 0x30, 0x34};

void h2(uint8_t v) { if (v < 16) Serial.print('0'); Serial.print(v, HEX); }

bool ack(uint8_t a) {
  Wire.beginTransmission(a);
  return Wire.endTransmission() == 0;
}

bool readBytes(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t got = Wire.requestFrom(addr, len);
  if (got != len) return false;
  for (uint8_t i = 0; i < len; i++) {
    if (!Wire.available()) return false;
    buf[i] = Wire.read();
  }
  return true;
}

void writeReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

int16_t le(uint8_t a, uint8_t b) { return (int16_t)((uint16_t)a | ((uint16_t)b << 8)); }
int16_t be(uint8_t a, uint8_t b) { return (int16_t)(((uint16_t)a << 8) | b); }

float norm(float x) {
  while (x < 0) x += 360;
  while (x >= 360) x -= 360;
  return x;
}

void vec(const char *label, int16_t x, int16_t y, int16_t z) {
  float hdg = norm(atan2((float)y, (float)x) * 180.0 / PI);
  Serial.print(label);
  Serial.print(" X="); Serial.print(x);
  Serial.print(" Y="); Serial.print(y);
  Serial.print(" Z="); Serial.print(z);
  Serial.print(" H="); Serial.print(hdg, 1);
}

void scanBus() {
  Serial.println("\n=== BUS SCAN ===");
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    if (ack(a)) {
      Serial.print("ACK 0x"); h2(a);
      if (a == 0x0D) Serial.print(" QMC candidate");
      if (a == 0x1E) Serial.print(" HMC/HP candidate");
      if (a == 0x2C) Serial.print(" mystery 0x2C");
      if (a == 0x3C) Serial.print(" OLED");
      Serial.println();
      found++;
    }
  }
  if (!found) Serial.println("No I2C devices found");
  Serial.println("=== END SCAN ===");
}

void dumpRegs(uint8_t addr) {
  Serial.print("\n--- REG DUMP 0x"); h2(addr); Serial.println(" 00-3F ---");
  for (uint8_t row = 0; row < 4; row++) {
    uint8_t start = row * 16;
    Serial.print("0x"); h2(start); Serial.print(": ");
    for (uint8_t i = 0; i < 16; i++) {
      uint8_t b = 0;
      if (readBytes(addr, start + i, &b, 1)) { Serial.print("0x"); h2(b); }
      else Serial.print("----");
      Serial.print(' ');
    }
    Serial.println();
  }
}

void readWindows(uint8_t addr) {
  Serial.print("\n--- LIVE WINDOWS 0x"); h2(addr); Serial.println(" ---");
  for (uint8_t wi = 0; wi < sizeof(windows); wi++) {
    uint8_t reg = windows[wi];
    uint8_t b[6];
    Serial.print("WIN 0x"); h2(reg); Serial.print(": ");
    if (!readBytes(addr, reg, b, 6)) { Serial.println("read failed"); continue; }
    for (uint8_t i = 0; i < 6; i++) { Serial.print("0x"); h2(b[i]); Serial.print(' '); }
    Serial.print(" | ");
    vec("LE", le(b[0],b[1]), le(b[2],b[3]), le(b[4],b[5]));
    Serial.print(" | ");
    vec("BE", be(b[0],b[1]), be(b[2],b[3]), be(b[4],b[5]));
    Serial.println();
  }
}

void tryInit(uint8_t addr) {
  Serial.print("\nTrying HMC init on 0x"); h2(addr); Serial.println();
  writeReg(addr, 0x00, 0x70); writeReg(addr, 0x01, 0x20); writeReg(addr, 0x02, 0x00); delay(80);
  readWindows(addr);

  Serial.print("\nTrying QMC init on 0x"); h2(addr); Serial.println();
  writeReg(addr, 0x0B, 0x01); writeReg(addr, 0x09, 0x1D); delay(80);
  readWindows(addr);
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("COMPASS TRUTH SERUM v1");
  Serial.print("SDA GPIO"); Serial.print(SDA_PIN);
  Serial.print(" SCL GPIO"); Serial.println(SCL_PIN);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(50000);
  scanBus();
  for (uint8_t i = 0; i < sizeof(addrs); i++) {
    uint8_t a = addrs[i];
    if (ack(a)) { dumpRegs(a); tryInit(a); }
  }
}

void loop() {
  for (uint8_t i = 0; i < sizeof(addrs); i++) {
    uint8_t a = addrs[i];
    if (ack(a) && a != 0x3C) readWindows(a);
  }
  delay(1500);
}
