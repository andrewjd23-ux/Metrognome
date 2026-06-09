// HP5883_RegisterWatch.ino
// Diagnostic for the HP5883-ish compass clone seen at I2C address 0x2C.
// Serial Monitor: 115200 baud.
// Wiring: SDA -> GPIO5, SCL -> GPIO6, VCC -> 3V3, GND -> GND.
//
// This sketch avoids destructive probing. It scans, dumps registers, tries the
// same gentle HMC/QMC-style setup writes we already used, then watches all
// registers 0x00-0x3F for changes while you rotate the board.

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

constexpr uint8_t SDA_PIN = 5;
constexpr uint8_t SCL_PIN = 6;
constexpr uint8_t HP_ADDR = 0x2C;

uint8_t previousRegs[64];
bool havePrevious = false;

void h2(uint8_t v) {
  if (v < 16) Serial.print('0');
  Serial.print(v, HEX);
}

bool ack(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool readReg(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(HP_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t got = Wire.requestFrom(HP_ADDR, (uint8_t)1);
  if (got != 1 || !Wire.available()) return false;
  value = Wire.read();
  return true;
}

bool readBlock(uint8_t startReg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(HP_ADDR);
  Wire.write(startReg);
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t got = Wire.requestFrom(HP_ADDR, len);
  if (got != len) return false;
  for (uint8_t i = 0; i < len; i++) {
    if (!Wire.available()) return false;
    buf[i] = Wire.read();
  }
  return true;
}

void writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(HP_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

int16_t le16(uint8_t a, uint8_t b) { return (int16_t)((uint16_t)a | ((uint16_t)b << 8)); }
int16_t be16(uint8_t a, uint8_t b) { return (int16_t)(((uint16_t)a << 8) | b); }

float norm360(float x) {
  while (x < 0) x += 360;
  while (x >= 360) x -= 360;
  return x;
}

void printVec(const char *label, int16_t x, int16_t y, int16_t z) {
  float h = norm360(atan2((float)y, (float)x) * 180.0 / PI);
  Serial.print(label);
  Serial.print(" X="); Serial.print(x);
  Serial.print(" Y="); Serial.print(y);
  Serial.print(" Z="); Serial.print(z);
  Serial.print(" H="); Serial.print(h, 1);
}

void scanBus() {
  Serial.println("\n=== BUS SCAN ===");
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    if (ack(a)) {
      Serial.print("ACK 0x"); h2(a);
      if (a == HP_ADDR) Serial.print(" HP5883-ish target");
      if (a == 0x3C) Serial.print(" OLED");
      if (a == 0x1E) Serial.print(" HMC candidate");
      if (a == 0x0D) Serial.print(" QMC candidate");
      Serial.println();
      found++;
    }
  }
  if (!found) Serial.println("No I2C devices found");
  Serial.println("=== END BUS SCAN ===");
}

void dumpRegs(const char *label) {
  Serial.println();
  Serial.print("=== "); Serial.print(label); Serial.println(" REG 00-3F ===");
  for (uint8_t row = 0; row < 4; row++) {
    uint8_t start = row * 16;
    Serial.print("0x"); h2(start); Serial.print(": ");
    for (uint8_t i = 0; i < 16; i++) {
      uint8_t reg = start + i;
      uint8_t v = 0;
      if (readReg(reg, v)) { Serial.print("0x"); h2(v); }
      else Serial.print("----");
      Serial.print(' ');
    }
    Serial.println();
  }
}

void gentleInitAttempts() {
  Serial.println("\n=== GENTLE INIT ATTEMPTS ===");

  Serial.println("HMC-style: 00=70, 01=20, 02=00");
  writeReg(0x00, 0x70);
  writeReg(0x01, 0x20);
  writeReg(0x02, 0x00);
  delay(120);
  dumpRegs("AFTER HMC-STYLE");

  Serial.println("QMC-style: 0B=01, 09=1D");
  writeReg(0x0B, 0x01);
  writeReg(0x09, 0x1D);
  delay(120);
  dumpRegs("AFTER QMC-STYLE");

  Serial.println("QMC gentle: 0B=01, 09=0D");
  writeReg(0x0B, 0x01);
  writeReg(0x09, 0x0D);
  delay(120);
  dumpRegs("AFTER QMC-GENTLE");
}

void rememberCurrent() {
  for (uint8_t r = 0; r < 64; r++) {
    uint8_t v = 0;
    if (readReg(r, v)) previousRegs[r] = v;
    else previousRegs[r] = 0;
  }
  havePrevious = true;
}

void liveWatch() {
  uint8_t current[64];
  bool ok[64];
  uint8_t changed = 0;

  for (uint8_t r = 0; r < 64; r++) {
    ok[r] = readReg(r, current[r]);
    if (havePrevious && ok[r] && current[r] != previousRegs[r]) changed++;
  }

  Serial.println("\n=== LIVE REGISTER WATCH ===");
  Serial.print("Changed registers since last read: "); Serial.println(changed);

  if (changed > 0) {
    for (uint8_t r = 0; r < 64; r++) {
      if (havePrevious && ok[r] && current[r] != previousRegs[r]) {
        Serial.print("reg 0x"); h2(r);
        Serial.print(" 0x"); h2(previousRegs[r]);
        Serial.print(" -> 0x"); h2(current[r]);
        Serial.println();
      }
    }
  }

  const uint8_t starts[] = {0x00, 0x03, 0x06, 0x09, 0x10, 0x20, 0x30, 0x34};
  for (uint8_t si = 0; si < sizeof(starts); si++) {
    uint8_t s = starts[si];
    uint8_t b[6];
    Serial.print("WIN 0x"); h2(s); Serial.print(": ");
    if (!readBlock(s, b, 6)) { Serial.println("read failed"); continue; }
    for (uint8_t i = 0; i < 6; i++) { Serial.print("0x"); h2(b[i]); Serial.print(' '); }
    Serial.print(" | ");
    printVec("LE", le16(b[0],b[1]), le16(b[2],b[3]), le16(b[4],b[5]));
    Serial.print(" | ");
    printVec("BE", be16(b[0],b[1]), be16(b[2],b[3]), be16(b[4],b[5]));
    Serial.println();
  }

  for (uint8_t r = 0; r < 64; r++) previousRegs[r] = current[r];
  havePrevious = true;
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("HP5883 REGISTER WATCH v1");
  Serial.print("SDA GPIO"); Serial.print(SDA_PIN);
  Serial.print(" SCL GPIO"); Serial.println(SCL_PIN);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(50000);

  scanBus();
  if (!ack(HP_ADDR)) {
    Serial.println("0x2C not visible. Check wiring.");
    return;
  }

  dumpRegs("BASELINE");
  gentleInitAttempts();
  rememberCurrent();

  Serial.println("\nRotate the board now. Watch for changed registers.");
}

void loop() {
  if (!ack(HP_ADDR)) {
    Serial.println("0x2C not ACKing now.");
    delay(1500);
    return;
  }

  liveWatch();
  delay(1500);
}
