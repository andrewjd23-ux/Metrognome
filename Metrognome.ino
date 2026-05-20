#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <U8g2lib.h>

// Metrognome dual OLED test
// Board: ESP32-C3 / Ozobot circuit kit style board
//
// Confirmed onboard OLED:
//   72x40 monochrome OLED, I2C address 0x3C
//   SCL -> GPIO5
//   SDA -> GPIO6
//
// Confirmed external OLED4SPI wiring:
//   OLED GND -> ESP32 GND
//   OLED VCC/VOC -> ESP32 3V3
//   OLED SCK -> GPIO4
//   OLED SDA -> GPIO7  // SPI MOSI/data, not I2C SDA
//   OLED RES -> GPIO8
//   OLED DC  -> GPIO9
//   OLED CS  -> GPIO10

constexpr uint8_t PIN_I2C_SCL  = 5;
constexpr uint8_t PIN_I2C_SDA  = 6;

constexpr uint8_t PIN_SPI_SCK  = 4;
constexpr uint8_t PIN_SPI_MOSI = 7;
constexpr uint8_t PIN_BIG_RST  = 8;
constexpr uint8_t PIN_BIG_DC   = 9;
constexpr uint8_t PIN_BIG_CS   = 10;

// Small built-in display. This constructor matches the 72x40 onboard OLED.
U8G2_SSD1306_72X40_ER_F_HW_I2C smallDisplay(
  U8G2_R0,
  U8X8_PIN_NONE
);

// Big external display. Confirmed working as SSD1306 128x64 SPI.
U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI bigDisplay(
  U8G2_R0,
  PIN_BIG_CS,
  PIN_BIG_DC,
  PIN_BIG_RST
);

uint32_t frame = 0;
uint32_t lastDrawMs = 0;

void drawSmallStatus() {
  smallDisplay.clearBuffer();

  smallDisplay.setFont(u8g2_font_4x6_tf);
  smallDisplay.drawStr(0, 6, "METROGNOME");
  smallDisplay.drawStr(0, 15, "WiFi: TEST");
  smallDisplay.drawStr(0, 24, "API : IDLE");
  smallDisplay.drawStr(0, 33, "BIG : OK");

  // Tiny heartbeat pixel so we know it is refreshing.
  if ((frame / 4) % 2 == 0) {
    smallDisplay.drawBox(66, 0, 5, 5);
  } else {
    smallDisplay.drawFrame(66, 0, 5, 5);
  }

  smallDisplay.sendBuffer();
}

void drawBigWeatherMock() {
  bigDisplay.clearBuffer();

  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.drawStr(0, 9, "METROGNOME WEATHER");
  bigDisplay.drawHLine(0, 12, 128);

  bigDisplay.setFont(u8g2_font_7x14B_tf);
  bigDisplay.drawStr(0, 29, "18.4 C");

  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.drawStr(72, 23, "WIND SW");
  bigDisplay.drawStr(72, 34, "14 kt");

  bigDisplay.drawFrame(0, 36, 128, 28);
  bigDisplay.setCursor(4, 48);
  bigDisplay.print("Pressure 1012 hPa");
  bigDisplay.setCursor(4, 60);
  bigDisplay.print("Sea 0.8m  Frame ");
  bigDisplay.print(frame);

  // Moving marker: future home of scanline/barometer animation nonsense.
  uint8_t x = (frame * 2) % 120;
  bigDisplay.drawBox(x, 13, 8, 3);

  bigDisplay.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("Metrognome dual OLED test starting...");

  // Start I2C for the onboard OLED first.
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  smallDisplay.setI2CAddress(0x3C * 2); // U8g2 uses 8-bit I2C address form
  smallDisplay.begin();
  smallDisplay.setContrast(180);
  Serial.println("Small onboard OLED started on GPIO5/6.");

  // Start SPI for the external OLED.
  SPI.begin(PIN_SPI_SCK, -1, PIN_SPI_MOSI, PIN_BIG_CS);
  bigDisplay.begin();
  bigDisplay.setBusClock(1000000); // gentle 1 MHz while testing jumper wires
  bigDisplay.setContrast(200);
  Serial.println("Big external SPI OLED started on GPIO4/7/8/9/10.");

  drawSmallStatus();
  drawBigWeatherMock();
  Serial.println("Both displays should now be drawing. Wahey.");
}

void loop() {
  const uint32_t now = millis();

  if (now - lastDrawMs >= 250) {
    lastDrawMs = now;
    frame++;
    drawSmallStatus();
    drawBigWeatherMock();
  }
}

/*
Arduino IDE settings:

  Tools > Board > ESP32 Arduino > ESP32C3 Dev Module
  Tools > USB CDC On Boot > Enabled
  Serial Monitor: 115200 baud

Expected result:

  Small onboard OLED:
    METROGNOME
    WiFi: TEST
    API : IDLE
    BIG : OK

  Big external OLED:
    METROGNOME WEATHER
    mock temperature, wind, pressure, sea state, and frame counter

Next sensible step:

  Add rotary encoder page switching before adding WiFi and the Stormglass API.
*/
