#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <U8g2lib.h>
#include "secrets.h"

constexpr uint8_t PIN_I2C_SDA=5, PIN_I2C_SCL=6;
constexpr uint8_t PIN_SPI_SCK=4, PIN_SPI_MOSI=7, PIN_BIG_RST=8, PIN_BIG_DC=9, PIN_BIG_CS=10;
constexpr uint8_t PIN_ENC_CLK=0, PIN_ENC_DT=1, PIN_ENC_SW=2;

U8G2_SSD1306_72X40_ER_F_HW_I2C smallDisplay(U8G2_R0, U8X8_PIN_NONE);
U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI bigDisplay(U8G2_R0, PIN_BIG_CS, PIN_BIG_DC, PIN_BIG_RST);

enum Page:uint8_t{WEATHER,SEA,TIDE,MOON,LIGHTNING,SYSTEM,PAGE_COUNT};
const char* pageNames[PAGE_COUNT]={"WEATHER","SEA","TIDE","MOON","LIGHT","SYSTEM"};
Page page=SYSTEM;

uint32_t frame=0,lastDraw=0,lastButton=0,pressUntil=0,lastWifiCheck=0;
int lastClk=HIGH;
bool buttonDown=false, pressNotice=false;
bool wifiOk=false;
String wifiStatus="BOOT";
String connectedSsid="";

String apiStatus="IDLE";
String lastUpdate="never";
String testPayloadPreview="";
int testHttpCode=0;
int testPayloadLength=0;

void smallStatus();
void drawBig();

bool connectToNetwork(const char* ssid,const char* password,uint32_t timeoutMs=12000){
  if(ssid==nullptr || strlen(ssid)==0) return false;
  WiFi.disconnect(true);
  delay(250);
  wifiStatus="JOIN";
  connectedSsid=String(ssid);
  Serial.print("Trying WiFi: ");
  Serial.println(ssid);
  if(password==nullptr || strlen(password)==0) WiFi.begin(ssid);
  else WiFi.begin(ssid,password);
  uint32_t start=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-start<timeoutMs){
    delay(250);
    Serial.print(".");
    frame++;
    smallStatus();
  }
  Serial.println();
  if(WiFi.status()==WL_CONNECTED){
    wifiOk=true;
    wifiStatus="OK";
    connectedSsid=String(ssid);
    Serial.print("Connected: ");
    Serial.println(connectedSsid);
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("RSSI: ");
    Serial.println(WiFi.RSSI());
    return true;
  }
  Serial.print("Failed: ");
  Serial.println(ssid);
  return false;
}

bool connectToStrongestOpenNetwork(uint32_t timeoutMs=12000){
  wifiStatus="SCAN";
  connectedSsid="OPEN?";
  smallStatus();
  int n=WiFi.scanNetworks();
  if(n<=0) return false;
  int best=-1;
  int bestRssi=-999;
  for(int i=0;i<n;i++){
    if(WiFi.encryptionType(i)==WIFI_AUTH_OPEN && WiFi.RSSI(i)>bestRssi){
      bestRssi=WiFi.RSSI(i);
      best=i;
    }
  }
  if(best<0) return false;
  String openSsid=WiFi.SSID(best);
  return connectToNetwork(openSsid.c_str(),"",timeoutMs);
}

bool connectBestWiFi(){
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  wifiOk=false;
  wifiStatus="JOIN";
  connectedSsid="";
  if(connectToNetwork(OFFICE_WIFI_SSID,OFFICE_WIFI_PASSWORD)) return true;
  for(size_t i=0;i<KNOWN_WIFI_COUNT;i++){
    if(connectToNetwork(KNOWN_WIFI_SSIDS[i],KNOWN_WIFI_PASSWORDS[i])) return true;
  }
  if(ALLOW_OPEN_WIFI_FALLBACK){
    if(connectToStrongestOpenNetwork()) return true;
  }
  wifiOk=false;
  wifiStatus="FAIL";
  connectedSsid="";
  return false;
}

void updateWifiStatus(){
  wifiOk=(WiFi.status()==WL_CONNECTED);
  if(wifiOk){
    wifiStatus="OK";
    connectedSsid=WiFi.SSID();
  } else {
    wifiStatus="FAIL";
    connectedSsid="";
  }
}

String readHttpBodyManually(HTTPClient& http, uint32_t timeoutMs=10000){
  String payload="";
  WiFiClient* stream=http.getStreamPtr();
  uint32_t lastDataMs=millis();
  while(http.connected() && millis()-lastDataMs<timeoutMs){
    while(stream->available()){
      payload += char(stream->read());
      lastDataMs=millis();
    }
    delay(10);
  }
  return payload;
}

bool fetchExampleDotComTest(){
  if(!wifiOk){
    apiStatus="WIFI";
    smallStatus();
    drawBig();
    if(!connectBestWiFi()){
      apiStatus="NO WIFI";
      return false;
    }
  }
  apiStatus="CALL";
  smallStatus();
  drawBig();
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url="https://example.com/";
  Serial.println("TEST URL:");
  Serial.println(url);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.useHTTP10(true);
  if(!http.begin(client,url)){
    Serial.println("HTTP begin failed");
    apiStatus="HTTP";
    return false;
  }
  http.addHeader("Accept","text/html");
  http.addHeader("Accept-Encoding","identity");
  int code=http.GET();
  testHttpCode=code;
  Serial.print("HTTP code: ");
  Serial.println(code);
  Serial.print("HTTP size: ");
  Serial.println(http.getSize());
  String payload=readHttpBodyManually(http,10000);
  http.end();
  testPayloadLength=payload.length();
  testPayloadPreview=payload.substring(0,80);
  Serial.print("Payload length: ");
  Serial.println(testPayloadLength);
  Serial.println("Payload preview:");
  Serial.println(payload.substring(0,500));
  if(code<=0){
    apiStatus="HTTPERR";
    return false;
  }
  if(payload.length()==0){
    apiStatus="EMPTY";
    return false;
  }
  apiStatus="OK";
  lastUpdate="example";
  return true;
}

void nextPage(){
  page=(Page)((page+1)%PAGE_COUNT);
}

void prevPage(){
  page=(Page)((page+PAGE_COUNT-1)%PAGE_COUNT);
}

void checkEncoder(){
  int clk=digitalRead(PIN_ENC_CLK);
  if(clk!=lastClk && clk==LOW){
    int dt=digitalRead(PIN_ENC_DT);
    if(dt!=clk) nextPage();
    else prevPage();
  }
  lastClk=clk;
  uint32_t now=millis();
  bool swDown=(digitalRead(PIN_ENC_SW)==LOW);
  if(swDown && !buttonDown && now-lastButton>50){
    buttonDown=true;
  }
  if(!swDown && buttonDown && now-lastButton>50){
    buttonDown=false;
    lastButton=now;
    pressNotice=true;
    pressUntil=now+1200;
    if(page==SYSTEM){
      fetchExampleDotComTest();
    }
  }
  if(pressNotice && now>pressUntil){
    pressNotice=false;
  }
}

void smallStatus(){
  smallDisplay.clearBuffer();
  smallDisplay.setFont(u8g2_font_4x6_tf);
  smallDisplay.drawStr(0,6,"METROGNOME");
  smallDisplay.setCursor(0,15);
  smallDisplay.print("WiFi ");
  smallDisplay.print(wifiStatus);
  smallDisplay.setCursor(0,24);
  smallDisplay.print("API  ");
  smallDisplay.print(apiStatus);
  smallDisplay.setCursor(0,33);
  smallDisplay.print("PAGE ");
  smallDisplay.print(pageNames[page]);
  if((frame/4)%2==0) smallDisplay.drawBox(66,0,5,5);
  else smallDisplay.drawFrame(66,0,5,5);
  smallDisplay.sendBuffer();
}

void header(const char* title){
  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.drawStr(0,9,"METROGNOME");
  bigDisplay.setCursor(72,9);
  bigDisplay.print(title);
  bigDisplay.drawHLine(0,12,128);
}

void drawWeather(){
  bigDisplay.clearBuffer();
  header("WEATHER");
  bigDisplay.setFont(u8g2_font_6x12_tf);
  bigDisplay.drawStr(0,28,"API test build");
  bigDisplay.drawStr(0,43,"No Stormglass");
  bigDisplay.drawStr(0,58,"quota safe");
  bigDisplay.sendBuffer();
}

void drawSea(){
  bigDisplay.clearBuffer();
  header("SEA");
  bigDisplay.setFont(u8g2_font_6x12_tf);
  bigDisplay.drawStr(0,28,"Sea data paused");
  bigDisplay.drawStr(0,43,"Testing HTTPS");
  bigDisplay.drawStr(0,58,"body reader");
  bigDisplay.sendBuffer();
}

void drawTide(){
  bigDisplay.clearBuffer();
  header("TIDE");
  bigDisplay.setFont(u8g2_font_6x12_tf);
  bigDisplay.drawStr(0,28,"Tide later");
  bigDisplay.drawStr(0,43,"First:");
  bigDisplay.drawStr(0,58,"fix HTTP read");
  bigDisplay.sendBuffer();
}

void drawMoon(){
  bigDisplay.clearBuffer();
  header("MOON");
  bigDisplay.setFont(u8g2_font_6x12_tf);
  bigDisplay.drawStr(0,28,"Moon waits");
  bigDisplay.drawStr(0,43,"The web must");
  bigDisplay.drawStr(0,58,"speak first");
  bigDisplay.drawCircle(104,38,18);
  bigDisplay.drawDisc(110,38,16);
  bigDisplay.sendBuffer();
}

void drawLightning(){
  bigDisplay.clearBuffer();
  header("LIGHT");
  bigDisplay.setFont(u8g2_font_6x12_tf);
  bigDisplay.drawStr(0,27,"Fake lightning");
  bigDisplay.setFont(u8g2_font_7x14B_tf);
  bigDisplay.drawStr(0,45,"NW 34km");
  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.drawStr(0,60,"12/hr RISK HIGH");
  bigDisplay.drawCircle(103,38,21);
  bigDisplay.drawCircle(103,38,12);
  bigDisplay.drawLine(103,17,103,59);
  bigDisplay.drawLine(82,38,124,38);
  bigDisplay.drawCircle(103,38,((frame*3)%18)+2);
  int sx=96+((frame*5)%18);
  int sy=25+((frame*7)%25);
  bigDisplay.drawLine(sx,sy,sx-5,sy+8);
  bigDisplay.drawLine(sx-5,sy+8,sx+1,sy+8);
  bigDisplay.drawLine(sx+1,sy+8,sx-6,sy+18);
  bigDisplay.sendBuffer();
}

void drawSystem(){
  bigDisplay.clearBuffer();
  header("SYSTEM");
  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.setCursor(0,24);
  bigDisplay.print("WiFi ");
  bigDisplay.print(wifiStatus);
  bigDisplay.setCursor(0,36);
  bigDisplay.print("HTTP ");
  bigDisplay.print(testHttpCode);
  bigDisplay.setCursor(0,48);
  bigDisplay.print("Len ");
  bigDisplay.print(testPayloadLength);
  bigDisplay.setCursor(0,60);
  if(testPayloadLength>0){
    bigDisplay.print(testPayloadPreview.substring(0,20));
  } else {
    bigDisplay.print("PRESS: TEST HTTPS");
  }
  bigDisplay.sendBuffer();
}

void drawBig(){
  switch(page){
    case WEATHER:drawWeather();break;
    case SEA:drawSea();break;
    case TIDE:drawTide();break;
    case MOON:drawMoon();break;
    case LIGHTNING:drawLightning();break;
    case SYSTEM:drawSystem();break;
    default:drawSystem();break;
  }
}

void setup(){
  Serial.begin(115200);
  delay(1000);
  Serial.println("Metrognome HTTPS body-read diagnostic build");
  pinMode(PIN_ENC_CLK,INPUT_PULLUP);
  pinMode(PIN_ENC_DT,INPUT_PULLUP);
  pinMode(PIN_ENC_SW,INPUT_PULLUP);
  lastClk=digitalRead(PIN_ENC_CLK);
  Wire.begin(PIN_I2C_SDA,PIN_I2C_SCL);
  Wire.setClock(100000);
  smallDisplay.setI2CAddress(0x3C*2);
  smallDisplay.begin();
  smallDisplay.setContrast(180);
  SPI.begin(PIN_SPI_SCK,-1,PIN_SPI_MOSI,PIN_BIG_CS);
  bigDisplay.begin();
  bigDisplay.setBusClock(1000000);
  bigDisplay.setContrast(200);
  smallStatus();
  drawBig();
  connectBestWiFi();
}

void loop(){
  checkEncoder();
  uint32_t now=millis();
  if(now-lastWifiCheck>=10000){
    lastWifiCheck=now;
    updateWifiStatus();
  }
  if(now-lastDraw>=120){
    lastDraw=now;
    frame++;
    smallStatus();
    drawBig();
  }
}
