// I2C_Compass_Standalone.ino
// Focused live reader for the mystery device seen at I2C address 0x2C.
// Use Serial Monitor at 115200 baud.
// Wiring for this test: SDA -> GPIO6, SCL -> GPIO5, VCC -> 3V3, GND -> GND.

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

constexpr uint8_t SDA_PIN = 6;
constexpr uint8_t SCL_PIN = 5;
constexpr uint8_t ADDR = 0x2C;

const uint8_t windows[] = {0x00, 0x03, 0x06, 0x09, 0x10, 0x20, 0x30, 0x34};
const size_t windowCount = sizeof(windows) / sizeof(windows[0]);
uint8_t lastData[8][6];
bool haveLast[8];

void h2(uint8_t v) {
  if (v < 16) Serial.print('0');
  Serial.print(v, HEX);
}

bool ack(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

void scanBus() {
  Serial.println("\n=== SCAN GPIO6/GPIO5 ===");
  for (uint8_t a = 1; a < 127; a++) {
    if (ack(a)) {
      Serial.print("ACK 0x"); h2(a);
      if (a == 0x2C) Serial.print(" mystery");
      if (a == 0x3C) Serial.print(" OLED");
      if (a == 0x1E) Serial.print(" HMC/HP");
      if (a == 0x0D) Serial.print(" QMC");
      Serial.println();
    }
  }
  Serial.println("=== SCAN END ===");
}

bool readBytes(uint8_t reg, uint8_t *buf, uint8_t len) {
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

void writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ADDR);
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

void printVec(const char *label, int16_t x, int16_t y, int16_t z) {
  float hdg = norm(atan2((float)y, (float)x) * 180.0 / PI);
  Serial.print(label);
  Serial.print(" X="); Serial.print(x);
  Serial.print(" Y="); Serial.print(y);
  Serial.print(" Z="); Serial.print(z);
  Serial.print(" HDG="); Serial.print(hdg, 1);
}

bool changed(size_t idx, uint8_t *b) {
  if (!haveLast[idx]) return false;
  for (uint8_t i = 0; i < 6; i++) if (lastData[idx][i] != b[i]) return true;
  return false;
}

void remember(size_t idx, uint8_t *b) {
  for (uint8_t i = 0; i < 6; i++) lastData[idx][i] = b[i];
  haveLast[idx] = true;
}

void dump64() {
  Serial.println("\n=== REGS 00-3F ===");
  for (uint8_t row = 0; row < 4; row++) {
    uint8_t start = row * 16;
    Serial.print("0x"); h2(start); Serial.print(": ");
    for (uint8_t i = 0; i < 16; i++) {
      uint8_t b;
      if (readBytes(start + i, &b, 1)) { Serial.print("0x"); h2(b); }
      else Serial.print("----");
      Serial.print(' ');
    }
    Serial.println();
  }
}

void readWindows() {
  Serial.println("\n=== LIVE WINDOWS: rotate board and watch CHANGED ===");
  for (size_t idx = 0; idx < windowCount; idx++) {
    uint8_t reg = windows[idx];
    uint8_t b[6];
    Serial.print("WIN 0x"); h2(reg); Serial.print(": ");
    if (!readBytes(reg, b, 6)) { Serial.println("read failed"); continue; }

    bool ch = changed(idx, b);
    for (uint8_t i = 0; i < 6; i++) { Serial.print("0x"); h2(b[i]); Serial.print(' '); }
    Serial.print(ch ? " CHANGED | " : " stable  | ");

    printVec("LE", le(b[0],b[1]), le(b[2],b[3]), le(b[4],b[5]));
    Serial.print(" | ");
    printVec("BE", be(b[0],b[1]), be(b[2],b[3]), be(b[4],b[5]));
    Serial.println();
    remember(idx, b);
  }
}

void tryInit() {
  Serial.println("\nTrying HMC-like init");
  writeReg(0x00, 0x70); writeReg(0x01, 0x20); writeReg(0x02, 0x00); delay(100);
  dump64(); readWindows();

  Serial.println("\nTrying QMC-like init");
  writeReg(0x0B, 0x01); writeReg(0x09, 0x1D); delay(100);
  dump64(); readWindows();
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("Mystery 0x2C live reader");
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(50000);
  scanBus();
  if (ack(ADDR)) { dump64(); tryInit(); }
  else Serial.println("0x2C did not ACK");
}

void loop() {
  if (ack(ADDR)) readWindows();
  else Serial.println("0x2C not ACKing now");
  delay(1200);
}
