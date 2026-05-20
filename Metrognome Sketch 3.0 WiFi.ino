#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <U8g2lib.h>
#include "secrets.h"

constexpr uint8_t PIN_I2C_SDA=5, PIN_I2C_SCL=6;
constexpr uint8_t PIN_SPI_SCK=4, PIN_SPI_MOSI=7, PIN_BIG_RST=8, PIN_BIG_DC=9, PIN_BIG_CS=10;
constexpr uint8_t PIN_ENC_CLK=0, PIN_ENC_DT=1, PIN_ENC_SW=2;

U8G2_SSD1306_72X40_ER_F_HW_I2C smallDisplay(U8G2_R0, U8X8_PIN_NONE);
U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI bigDisplay(U8G2_R0, PIN_BIG_CS, PIN_BIG_DC, PIN_BIG_RST);

enum Page:uint8_t{WEATHER,SEA,TIDE,MOON,SYSTEM,PAGE_COUNT};
const char* pageNames[PAGE_COUNT]={"WEATHER","SEA","TIDE","MOON","SYSTEM"};
Page page=WEATHER;

uint32_t frame=0,lastDraw=0,lastButton=0,pressUntil=0,lastWifiCheck=0;
int lastClk=HIGH;
bool buttonDown=false, pressNotice=false;
bool wifiOk=false;
String wifiStatus="BOOT";

void nextPage(){ page=(Page)((page+1)%PAGE_COUNT); Serial.println(pageNames[page]); }
void prevPage(){ page=(Page)((page+PAGE_COUNT-1)%PAGE_COUNT); Serial.println(pageNames[page]); }

void updateWifiStatus(){
  wifiOk = (WiFi.status() == WL_CONNECTED);
  if(wifiOk) wifiStatus="OK"; else wifiStatus="FAIL";
}

void connectWifi(){
  wifiStatus="JOIN";
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi: "); Serial.println(WIFI_SSID);

  uint32_t start=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-start<15000){
    delay(250);
    Serial.print(".");
    frame++;
    smallDisplay.clearBuffer();
    smallDisplay.setFont(u8g2_font_4x6_tf);
    smallDisplay.drawStr(0,6,"METROGNOME");
    smallDisplay.drawStr(0,15,"WiFi JOIN");
    smallDisplay.setCursor(0,24); smallDisplay.print("Try "); smallDisplay.print((millis()-start)/1000); smallDisplay.print("s");
    if((frame/2)%2==0) smallDisplay.drawBox(66,0,5,5); else smallDisplay.drawFrame(66,0,5,5);
    smallDisplay.sendBuffer();
  }
  Serial.println();
  updateWifiStatus();
  if(wifiOk){
    Serial.print("WiFi connected. IP: "); Serial.println(WiFi.localIP());
  }else{
    Serial.println("WiFi connect failed. Continuing offline.");
  }
}

void checkEncoder(){
  int clk=digitalRead(PIN_ENC_CLK);
  if(clk!=lastClk && clk==LOW){
    int dt=digitalRead(PIN_ENC_DT);
    if(dt!=clk) nextPage(); else prevPage();
  }
  lastClk=clk;

  uint32_t now=millis();
  bool swDown=(digitalRead(PIN_ENC_SW)==LOW);
  if(swDown && !buttonDown && now-lastButton>50) buttonDown=true;
  if(!swDown && buttonDown && now-lastButton>50){
    buttonDown=false; lastButton=now; pressNotice=true; pressUntil=now+1200;
    Serial.print("Press: "); Serial.println(pageNames[page]);
    if(page==SYSTEM) connectWifi();
  }
  if(pressNotice && now>pressUntil) pressNotice=false;
}

void smallStatus(){
  smallDisplay.clearBuffer();
  smallDisplay.setFont(u8g2_font_4x6_tf);
  smallDisplay.drawStr(0,6,"METROGNOME");
  smallDisplay.setCursor(0,15); smallDisplay.print("WiFi "); smallDisplay.print(wifiStatus);
  smallDisplay.setCursor(0,24); smallDisplay.print("PAGE "); smallDisplay.print(pageNames[page]);
  smallDisplay.drawStr(0,33,pressNotice?"PUSH OK":"API  IDLE");
  if((frame/4)%2==0) smallDisplay.drawBox(66,0,5,5); else smallDisplay.drawFrame(66,0,5,5);
  smallDisplay.sendBuffer();
}

void header(const char* title){
  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.drawStr(0,9,"METROGNOME");
  bigDisplay.setCursor(72,9); bigDisplay.print(title);
  bigDisplay.drawHLine(0,12,128);
}

void drawWeather(){
  bigDisplay.clearBuffer(); header("WEATHER");
  bigDisplay.setFont(u8g2_font_7x14B_tf); bigDisplay.drawStr(0,29,"18.4 C");
  bigDisplay.setFont(u8g2_font_6x10_tf); bigDisplay.drawStr(72,23,"WIND SW"); bigDisplay.drawStr(72,34,"14 kt");
  bigDisplay.drawFrame(0,36,128,28); bigDisplay.drawStr(4,48,"Pressure 1012 hPa"); bigDisplay.drawStr(4,60,"Rain 23%  Mock");
  bigDisplay.drawBox((frame*2)%120,13,8,3); bigDisplay.sendBuffer();
}

void drawSea(){
  bigDisplay.clearBuffer(); header("SEA");
  bigDisplay.setFont(u8g2_font_7x14B_tf); bigDisplay.drawStr(0,29,"0.8 m");
  bigDisplay.setFont(u8g2_font_6x10_tf); bigDisplay.drawStr(72,23,"SWELL W"); bigDisplay.drawStr(72,34,"7 sec");
  bigDisplay.drawFrame(0,38,128,24); bigDisplay.drawStr(4,49,"Sea temp 11.2 C"); bigDisplay.drawStr(4,60,"Current 0.4 kt NE");
  bigDisplay.sendBuffer();
}

void drawTide(){
  bigDisplay.clearBuffer(); header("TIDE");
  bigDisplay.setFont(u8g2_font_6x12_tf); bigDisplay.drawStr(0,28,"HIGH  03:12"); bigDisplay.drawStr(0,43,"LOW   09:47"); bigDisplay.drawStr(0,58,"NEXT  RISING");
  bigDisplay.drawFrame(82,18,38,40); uint8_t h=8+((frame/2)%28); bigDisplay.drawBox(88,58-h,26,h);
  bigDisplay.sendBuffer();
}

void drawMoon(){
  bigDisplay.clearBuffer(); header("MOON");
  bigDisplay.setFont(u8g2_font_6x12_tf); bigDisplay.drawStr(0,28,"Waxing gibbous"); bigDisplay.drawStr(0,43,"Rise 18:42"); bigDisplay.drawStr(0,58,"Set  03:16");
  bigDisplay.drawCircle(104,38,18); bigDisplay.drawDisc(110,38,16); bigDisplay.sendBuffer();
}

void drawSystem(){
  bigDisplay.clearBuffer(); header("SYSTEM");
  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.setCursor(0,25); bigDisplay.print("WiFi: "); bigDisplay.print(wifiStatus);
  bigDisplay.setCursor(0,37); bigDisplay.print("RSSI: "); if(wifiOk) bigDisplay.print(WiFi.RSSI()); else bigDisplay.print("--"); bigDisplay.print(" dBm");
  bigDisplay.setCursor(0,49); bigDisplay.print("IP: "); if(wifiOk) bigDisplay.print(WiFi.localIP()); else bigDisplay.print("offline");
  bigDisplay.setCursor(0,61); bigDisplay.print("Press: reconnect");
  bigDisplay.sendBuffer();
}

void drawBig(){
  switch(page){case WEATHER:drawWeather();break;case SEA:drawSea();break;case TIDE:drawTide();break;case MOON:drawMoon();break;case SYSTEM:drawSystem();break;default:drawWeather();break;}
}

void setup(){
  Serial.begin(115200); delay(1000); Serial.println("Metrognome WiFi test");
  pinMode(PIN_ENC_CLK,INPUT_PULLUP); pinMode(PIN_ENC_DT,INPUT_PULLUP); pinMode(PIN_ENC_SW,INPUT_PULLUP); lastClk=digitalRead(PIN_ENC_CLK);
  Wire.begin(PIN_I2C_SDA,PIN_I2C_SCL); Wire.setClock(100000); smallDisplay.setI2CAddress(0x3C*2); smallDisplay.begin(); smallDisplay.setContrast(180);
  SPI.begin(PIN_SPI_SCK,-1,PIN_SPI_MOSI,PIN_BIG_CS); bigDisplay.begin(); bigDisplay.setBusClock(1000000); bigDisplay.setContrast(200);
  smallStatus(); drawBig(); connectWifi();
}

void loop(){
  checkEncoder();
  uint32_t now=millis();
  if(now-lastWifiCheck>=10000){ lastWifiCheck=now; updateWifiStatus(); }
  if(now-lastDraw>=120){ lastDraw=now; frame++; smallStatus(); drawBig(); }
}
