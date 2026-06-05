// Metrognome_v7.ino
// Lean Friday-evening compass diagnostic build.
// Purpose: prove the QMC5883 compass and OLED display path before re-merging the full thunder apparatus.

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <QMC5883LCompass.h>

constexpr uint8_t PIN_I2C_SDA = 5;
constexpr uint8_t PIN_I2C_SCL = 6;
constexpr uint8_t PIN_SPI_SCK = 4;
constexpr uint8_t PIN_SPI_MOSI = 7;
constexpr uint8_t PIN_BIG_RST = 8;
constexpr uint8_t PIN_BIG_DC = 9;
constexpr uint8_t PIN_BIG_CS = 10;
constexpr uint8_t PIN_ENC_CLK = 0;
constexpr uint8_t PIN_ENC_DT = 1;
constexpr uint8_t PIN_ENC_SW = 2;

U8G2_SSD1306_72X40_ER_F_HW_I2C smallDisplay(U8G2_R0, U8X8_PIN_NONE);
U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI bigDisplay(U8G2_R0, PIN_BIG_CS, PIN_BIG_DC, PIN_BIG_RST);
QMC5883LCompass compass;

uint32_t frame = 0;
uint32_t lastDraw = 0;
int lastClk = HIGH;

float normalize360(float angle) {
  while (angle < 0.0) angle += 360.0;
  while (angle >= 360.0) angle -= 360.0;
  return angle;
}

String bearingToCardinal(float bearing) {
  const char* dirs[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  int index = (int)((normalize360(bearing) + 22.5) / 45.0) % 8;
  return String(dirs[index]);
}

float readHeading(int &x, int &y, int &z) {
  compass.read();
  x = compass.getX();
  y = compass.getY();
  z = compass.getZ();
  float heading = atan2((float)y, (float)x) * 180.0 / PI;
  return normalize360(heading);
}

void drawSmall(float heading) {
  smallDisplay.clearBuffer();
  smallDisplay.setFont(u8g2_font_4x6_tf);
  smallDisplay.drawStr(0, 6, "METROGNOME V7");
  smallDisplay.setCursor(0, 18);
  smallDisplay.print("COMP ");
  smallDisplay.print((int)heading);
  smallDisplay.print((char)176);
  smallDisplay.setCursor(0, 31);
  smallDisplay.print("PAGE COMPASS");
  if ((frame / 4) % 2 == 0) smallDisplay.drawBox(66, 0, 5, 5);
  else smallDisplay.drawFrame(66, 0, 5, 5);
  smallDisplay.sendBuffer();
}

void drawBig(float heading, int x, int y, int z) {
  String dir = bearingToCardinal(heading);

  bigDisplay.clearBuffer();
  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.drawStr(0, 9, "METROGNOME");
  bigDisplay.setCursor(72, 9);
  bigDisplay.print("COMPASS");
  bigDisplay.drawHLine(0, 12, 128);

  bigDisplay.setFont(u8g2_font_7x14B_tf);
  bigDisplay.setCursor(0, 32);
  bigDisplay.print((int)heading);
  bigDisplay.print((char)176);
  bigDisplay.print(" ");
  bigDisplay.print(dir);

  bigDisplay.setFont(u8g2_font_5x8_tf);
  bigDisplay.setCursor(0, 46);
  bigDisplay.print("X ");
  bigDisplay.print(x);
  bigDisplay.print(" Y ");
  bigDisplay.print(y);
  bigDisplay.setCursor(0, 56);
  bigDisplay.print("Z ");
  bigDisplay.print(z);

  bigDisplay.setCursor(70, 56);
  if (x == 0 && y == 0 && z == 0) bigDisplay.print("QMC ?");
  else bigDisplay.print("QMC OK");

  bigDisplay.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(PIN_ENC_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC_DT, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  lastClk = digitalRead(PIN_ENC_CLK);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);

  smallDisplay.setI2CAddress(0x3C * 2);
  smallDisplay.begin();
  smallDisplay.setContrast(180);

  SPI.begin(PIN_SPI_SCK, -1, PIN_SPI_MOSI, PIN_BIG_CS);
  bigDisplay.begin();
  bigDisplay.setBusClock(1000000);
  bigDisplay.setContrast(200);

  compass.init();
  Serial.println("Metrognome v7 compass diagnostic ready.");
}

void loop() {
  uint32_t now = millis();
  if (now - lastDraw >= 120) {
    lastDraw = now;
    frame++;

    int x = 0, y = 0, z = 0;
    float heading = readHeading(x, y, z);

    Serial.print("Heading: ");
    Serial.print(heading);
    Serial.print(" X: ");
    Serial.print(x);
    Serial.print(" Y: ");
    Serial.print(y);
    Serial.print(" Z: ");
    Serial.println(z);

    drawSmall(heading);
    drawBig(heading, x, y, z);
  }
}
