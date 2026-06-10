// HP5883_RegisterWatch.ino
// Tidied diagnostic for the HP5883-ish compass clone seen at I2C address 0x2C.
// Serial Monitor: 115200 baud.
// Wiring: SDA -> GPIO5, SCL -> GPIO6, VCC -> 3V3, GND -> GND, optional DRDY -> GPIO20/RX.
//
// What this checks:
//   1. Full I2C bus scan.
//   2. HMC5883L-compatible register behaviour at 0x2C.
//   3. Whether config/mode writes stick.
//   4. Whether ID registers look like ASCII H43.
//   5. Whether status/data registers change after continuous-mode init.
//   6. Optional DRDY state.

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

constexpr uint8_t SDA_PIN = 5;
constexpr uint8_t SCL_PIN = 6;
constexpr uint8_t DRDY_PIN = 20;   // RX pin, optional. Leave wired only if convenient.
constexpr uint8_t MAG_ADDR = 0x2C;

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
  Wire.beginTransmission(MAG_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;

  uint8_t got = Wire.requestFrom(MAG_ADDR, (uint8_t)1);
  if (got != 1 || !Wire.available()) return false;

  value = Wire.read();
  return true;
}

bool readBlock(uint8_t startReg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(MAG_ADDR);
  Wire.write(startReg);
  if (Wire.endTransmission(false) != 0) return false;

  uint8_t got = Wire.requestFrom(MAG_ADDR, len);
  if (got != len) return false;

  for (uint8_t i = 0; i < len; i++) {
    if (!Wire.available()) return false;
    buf[i] = Wire.read();
  }
  return true;
}

bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MAG_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

int16_t be16(uint8_t msb, uint8_t lsb) {
  return (int16_t)(((uint16_t)msb << 8) | lsb);
}

float norm360(float x) {
  while (x < 0) x += 360;
  while (x >= 360) x -= 360;
  return x;
}

void printHeadingFromXZYCandidate(int16_t x, int16_t z, int16_t y) {
  float h = norm360(atan2((float)y, (float)x) * 180.0 / PI);
  Serial.print("X="); Serial.print(x);
  Serial.print(" Z="); Serial.print(z);
  Serial.print(" Y="); Serial.print(y);
  Serial.print(" heading="); Serial.print(h, 1);
}

void scanBus() {
  Serial.println("\n=== BUS SCAN ===");
  int found = 0;

  for (uint8_t a = 1; a < 127; a++) {
    if (ack(a)) {
      Serial.print("ACK 0x"); h2(a);
      if (a == MAG_ADDR) Serial.print(" target");
      if (a == 0x3C) Serial.print(" OLED or HMC 8-bit WRITE address confusion");
      if (a == 0x1E) Serial.print(" real HMC5883L 7-bit address");
      if (a == 0x0D) Serial.print(" QMC5883L 7-bit address");
      Serial.println();
      found++;
    }
  }

  if (!found) Serial.println("No I2C devices found");
  Serial.println("=== END BUS SCAN ===");
}

void dumpRegs00to3F(const char *label) {
  Serial.println();
  Serial.print("=== "); Serial.print(label); Serial.println(" REG 00-3F ===");

  for (uint8_t row = 0; row < 4; row++) {
    uint8_t start = row * 16;
    Serial.print("0x"); h2(start); Serial.print(": ");

    for (uint8_t i = 0; i < 16; i++) {
      uint8_t reg = start + i;
      uint8_t v = 0;
      if (readReg(reg, v)) {
        Serial.print("0x"); h2(v);
      } else {
        Serial.print("----");
      }
      Serial.print(' ');
    }
    Serial.println();
  }
}

void printCoreRegs(const char *label) {
  Serial.println();
  Serial.print("=== CORE REGS: "); Serial.print(label); Serial.println(" ===");

  for (uint8_t reg = 0; reg <= 12; reg++) {
    uint8_t v = 0;
    Serial.print("Reg 0x"); h2(reg); Serial.print(" = ");
    if (readReg(reg, v)) {
      Serial.print("0x"); h2(v);
      if (reg >= 10 && reg <= 12) {
        Serial.print(" ASCII '");
        char c = (v >= 32 && v <= 126) ? (char)v : '.';
        Serial.print(c);
        Serial.print("'");
      }
    } else {
      Serial.print("read failed");
    }
    Serial.println();
  }

  uint8_t a = 0, b = 0, c = 0;
  if (readReg(10, a) && readReg(11, b) && readReg(12, c)) {
    Serial.print("ID triplet = '");
    Serial.print((char)a); Serial.print((char)b); Serial.print((char)c);
    Serial.println("'  (real HMC5883L should be 'H43')");
  }
}

void writeAndVerify(uint8_t reg, uint8_t value, const char *name) {
  Serial.print("Write "); Serial.print(name); Serial.print(" reg 0x"); h2(reg);
  Serial.print(" <- 0x"); h2(value); Serial.print(" ... ");

  bool writeOk = writeReg(reg, value);
  delay(20);

  uint8_t after = 0;
  bool readOk = readReg(reg, after);

  if (!writeOk) Serial.print("WRITE_FAIL ");
  if (!readOk) Serial.print("READ_FAIL");
  else {
    Serial.print("readback 0x"); h2(after);
    if (after == value) Serial.print(" OK");
    else Serial.print(" DIFFERENT");
  }
  Serial.println();
}

void hmcStyleContinuousInit() {
  Serial.println("\n=== HMC-STYLE CONTINUOUS INIT TEST ===");
  Serial.println("Expected genuine HMC sequence: CRA=0x70, CRB=0xA0, MODE=0x00");

  writeAndVerify(0x00, 0x70, "CRA");
  writeAndVerify(0x01, 0xA0, "CRB");
  writeAndVerify(0x02, 0x00, "MODE continuous");

  Serial.println("Waiting 100ms for first continuous measurement...");
  delay(100);

  printCoreRegs("AFTER HMC-STYLE INIT");
}

void readHmcDataBurst(const char *label) {
  Serial.println();
  Serial.print("=== DATA BURST: "); Serial.print(label); Serial.println(" ===");

  uint8_t b[6];
  if (!readBlock(0x03, b, 6)) {
    Serial.println("Read 0x03..0x08 failed");
    return;
  }

  Serial.print("Raw 0x03..0x08: ");
  for (uint8_t i = 0; i < 6; i++) {
    Serial.print("0x"); h2(b[i]); Serial.print(' ');
  }
  Serial.print(" | ");

  int16_t x = be16(b[0], b[1]);
  int16_t z = be16(b[2], b[3]);
  int16_t y = be16(b[4], b[5]);
  printHeadingFromXZYCandidate(x, z, y);
  Serial.println();

  uint8_t status = 0;
  if (readReg(0x09, status)) {
    Serial.print("Status 0x09 = 0x"); h2(status);
    Serial.print(" LOCK="); Serial.print((status & 0x02) ? 1 : 0);
    Serial.print(" RDY="); Serial.println((status & 0x01) ? 1 : 0);
  }

  Serial.print("DRDY GPIO20 = ");
  Serial.println(digitalRead(DRDY_PIN));
}

void rememberCurrent() {
  for (uint8_t r = 0; r < 64; r++) {
    uint8_t v = 0;
    if (readReg(r, v)) previousRegs[r] = v;
    else previousRegs[r] = 0;
  }
  havePrevious = true;
}

void liveRegisterWatch() {
  uint8_t current[64];
  bool ok[64];
  uint8_t changed = 0;

  for (uint8_t r = 0; r < 64; r++) {
    ok[r] = readReg(r, current[r]);
    if (havePrevious && ok[r] && current[r] != previousRegs[r]) changed++;
  }

  Serial.println("\n=== LIVE WATCH ===");
  Serial.print("DRDY GPIO20 = "); Serial.println(digitalRead(DRDY_PIN));
  Serial.print("Changed registers 0x00-0x3F since last read: "); Serial.println(changed);

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

  readHmcDataBurst("0x03 DATA WINDOW");

  for (uint8_t r = 0; r < 64; r++) previousRegs[r] = current[r];
  havePrevious = true;
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("HP5883/HMC COMPASS DIAGNOSTIC v2");
  Serial.println("CDC must be enabled. Serial Monitor 115200 baud.");
  Serial.print("SDA GPIO"); Serial.print(SDA_PIN);
  Serial.print(" SCL GPIO"); Serial.print(SCL_PIN);
  Serial.print(" target 0x"); h2(MAG_ADDR); Serial.println();

  pinMode(DRDY_PIN, INPUT);

  delay(1000);
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(500);
  Wire.setClock(10000);
  delay(100);

  scanBus();
  if (!ack(MAG_ADDR)) {
    Serial.println("Target 0x2C not visible. Check SDA/SCL/VCC/GND.");
    return;
  }

  dumpRegs00to3F("BASELINE");
  printCoreRegs("BASELINE");
  hmcStyleContinuousInit();

  readHmcDataBurst("FIRST READ AFTER INIT");
  delay(100);
  readHmcDataBurst("SECOND READ AFTER INIT");

  rememberCurrent();
  Serial.println("\nRotate board and/or bring magnet near it. Live watch repeats every 1.5s.");
}

void loop() {
  if (!ack(MAG_ADDR)) {
    Serial.println("Target 0x2C not ACKing now.");
    delay(1500);
    return;
  }

  liveRegisterWatch();
  delay(1500);
}
