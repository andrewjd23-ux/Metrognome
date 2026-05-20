#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>

// Metrognome SPI OLED test
// Board: ESP32-C3 / Ozobot circuit kit style board
// External OLED4SPI wiring:
//   OLED GND -> ESP32 GND
//   OLED VCC/VOC -> ESP32 3V3
//   OLED SCK -> GPIO4
//   OLED SDA -> GPIO7  // SPI MOSI/data, not I2C SDA
//   OLED RES -> GPIO8
//   OLED DC  -> GPIO9
//   OLED CS  -> GPIO10

constexpr uint8_t PIN_SPI_SCK  = 4;
constexpr uint8_t PIN_SPI_MOSI = 7;
constexpr uint8_t PIN_OLED_RST = 8;
constexpr uint8_t PIN_OLED_DC  = 9;
constexpr uint8_t PIN_OLED_CS  = 10;

// Most 0.96 inch 4-wire SPI OLED modules are SSD1306 128x64.
// If the screen stays blank, try the SH1106 constructor in the notes below.
U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI bigDisplay(
  U8G2_R0,
  PIN_OLED_CS,
  PIN_OLED_DC,
  PIN_OLED_RST
);

uint32_t frame = 0;

void drawTestScreen() {
  bigDisplay.clearBuffer();

  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.drawStr(0, 10, "METROGNOME");
  bigDisplay.drawStr(0, 22, "SPI OLED TEST");

  bigDisplay.drawFrame(0, 28, 128, 36);
  bigDisplay.drawLine(0, 46, 127, 46);

  bigDisplay.setCursor(4, 42);
  bigDisplay.print("SCK 4  MOSI 7");

  bigDisplay.setCursor(4, 58);
  bigDisplay.print("Frame: ");
  bigDisplay.print(frame++);

  // Tiny moving marker so you know loop updates are alive.
  uint8_t x = (frame * 3) % 120;
  bigDisplay.drawBox(x, 30, 8, 8);

  bigDisplay.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("Metrognome external SPI OLED test starting...");

  // ESP32 lets us assign SPI pins. MISO is unused, so use -1.
  SPI.begin(PIN_SPI_SCK, -1, PIN_SPI_MOSI, PIN_OLED_CS);

  bigDisplay.begin();
  bigDisplay.setBusClock(1000000); // gentle 1 MHz while testing jumper wires

  Serial.println("Display begin complete. If screen is blank, check wiring/power/driver.");
}

void loop() {
  drawTestScreen();
  delay(250);
}

/*
Troubleshooting notes:

1. In Arduino IDE use:
   Tools > Board > ESP32 Arduino > ESP32C3 Dev Module
   Tools > USB CDC On Boot > Enabled

2. If upload works but Serial Monitor is blank:
   Set Serial Monitor to 115200 baud and press reset.

3. If the OLED remains blank but wiring is correct, the panel may be SH1106.
   Replace the display constructor with this and upload again:

   U8G2_SH1106_128X64_NONAME_F_4W_HW_SPI bigDisplay(
     U8G2_R0,
     PIN_OLED_CS,
     PIN_OLED_DC,
     PIN_OLED_RST
   );

4. OLED pin label "SDA" means SPI data/MOSI on this module.
*/
