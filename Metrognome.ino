#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <U8g2lib.h>
#include "secrets.h"

constexpr uint8_t PIN_I2C_SDA=5, PIN_I2C_SCL=6;
constexpr uint8_t PIN_SPI_SCK=4, PIN_SPI_MOSI=7, PIN_BIG_RST=8, PIN_BIG_DC=9, PIN_BIG_CS=10;
constexpr uint8_t PIN_ENC_CLK=0, PIN_ENC_DT=1, PIN_ENC_SW=2;

constexpr int DAILY_API_LIMIT = 10;
constexpr int RESERVED_CALLS = 1;

U8G2_SSD1306_72X40_ER_F_HW_I2C smallDisplay(U8G2_R0, U8X8_PIN_NONE);
U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI bigDisplay(U8G2_R0, PIN_BIG_CS, PIN_BIG_DC, PIN_BIG_RST);

enum Page:uint8_t{WEATHER,SEA,TIDE,MOON,LIGHTNING,SYSTEM,PAGE_COUNT};
const char* pageNames[PAGE_COUNT]={"WEATHER","SEA","TIDE","MOON","LIGHT","SYSTEM"};
Page page=WEATHER;

uint32_t frame=0,lastDraw=0,lastButton=0,pressUntil=0,lastWifiCheck=0;
int lastClk=HIGH;
bool buttonDown=false, pressNotice=false;
bool wifiOk=false;
String wifiStatus="BOOT";
String connectedSsid="";

Preferences prefs;
int callsToday = 0;
String apiDate = "";
String apiStatus = "IDLE";
String lastUpdate = "never";

struct WeatherData {
  bool valid=false;
  bool stale=true;
  float airTemp=0;
  float pressure=0;
  float humidity=0;
  float windSpeed=0;
  int windDirection=0;
  float waveHeight=0;
  float swellPeriod=0;
  float waterTemp=0;
};

struct LightningData{
  bool active=true;
  int strikeCount=12;
  float nearestKm=34.0;
  const char* direction="NW";
  int risk=3;
};

WeatherData weather;
LightningData lightning;

void smallStatus();
void drawBig();

float firstSource(JsonVariant v) {
  if (v.isNull()) return NAN;
  const char* sources[] = {"sg","noaa","meteo","dwd","icon","yr","smhi","fcoo"};
  for (const char* s : sources) {
    if (!v[s].isNull()) return v[s].as<float>();
  }
  return NAN;
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

String utcDate() {
  struct tm t;
  if (!getLocalTime(&t)) return "";
  char buf[11];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
  return String(buf);
}

String utcTimeShort() {
  struct tm t;
  if (!getLocalTime(&t)) return "--:--";
  char buf[6];
  strftime(buf, sizeof(buf), "%H:%M", &t);
  return String(buf);
}

void loadApiCounter() {
  prefs.begin("metrognome", false);
  String today = utcDate();
  apiDate = prefs.getString("api_date", "");
  callsToday = prefs.getInt("api_calls", 0);
  if (today != "" && today != apiDate) {
    apiDate = today;
    callsToday = 0;
    prefs.putString("api_date", apiDate);
    prefs.putInt("api_calls", callsToday);
  }
}

bool canCallApi() {
  loadApiCounter();
  return callsToday < (DAILY_API_LIMIT - RESERVED_CALLS);
}

void recordApiCall() {
  callsToday++;
  prefs.putInt("api_calls", callsToday);
}

bool connectToNetwork(const char* ssid,const char* password,uint32_t timeoutMs=12000){
  if(ssid==nullptr || strlen(ssid)==0) return false;
  WiFi.disconnect(true); delay(250);
  wifiStatus="JOIN"; connectedSsid=String(ssid);
  Serial.print("Trying WiFi: "); Serial.println(ssid);
  if(password==nullptr || strlen(password)==0) WiFi.begin(ssid);
  else WiFi.begin(ssid,password);
  uint32_t start=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-start<timeoutMs){ delay(250); Serial.print("."); frame++; smallStatus(); }
  Serial.println();
  if(WiFi.status()==WL_CONNECTED){
    wifiOk=true; wifiStatus="OK"; connectedSsid=String(ssid);
    Serial.print("Connected: "); Serial.println(connectedSsid);
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    Serial.print("RSSI: "); Serial.println(WiFi.RSSI());
    return true;
  }
  Serial.print("Failed: "); Serial.println(ssid);
  return false;
}

bool connectToStrongestOpenNetwork(uint32_t timeoutMs=12000){
  wifiStatus="SCAN"; connectedSsid="OPEN?"; smallStatus();
  int n=WiFi.scanNetworks();
  if(n<=0) return false;
  int best=-1, bestRssi=-999;
  for(int i=0;i<n;i++){
    if(WiFi.encryptionType(i)==WIFI_AUTH_OPEN && WiFi.RSSI(i)>bestRssi){ bestRssi=WiFi.RSSI(i); best=i; }
  }
  if(best<0) return false;
  String openSsid=WiFi.SSID(best);
  return connectToNetwork(openSsid.c_str(),"",timeoutMs);
}

bool connectBestWiFi(){
  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true); WiFi.persistent(false);
  wifiOk=false; wifiStatus="JOIN"; connectedSsid="";
  if(connectToNetwork(OFFICE_WIFI_SSID,OFFICE_WIFI_PASSWORD)) return true;
  for(size_t i=0;i<KNOWN_WIFI_COUNT;i++){
    if(connectToNetwork(KNOWN_WIFI_SSIDS[i],KNOWN_WIFI_PASSWORDS[i])) return true;
  }
  if(ALLOW_OPEN_WIFI_FALLBACK){ if(connectToStrongestOpenNetwork()) return true; }
  wifiOk=false; wifiStatus="FAIL"; connectedSsid="";
  return false;
}

void updateWifiStatus(){
  wifiOk=(WiFi.status()==WL_CONNECTED);
  if(wifiOk){ wifiStatus="OK"; connectedSsid=WiFi.SSID(); }
  else { wifiStatus="FAIL"; connectedSsid=""; }
}

void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  uint32_t start = millis();
  while (millis() - start < 8000) {
    struct tm t;
    if (getLocalTime(&t)) return;
    delay(250);
  }
}

String stormglassUrl() {
  return String("https://api.stormglass.io/v2/weather/point?lat=") +
         String(METROGNOME_LAT, 6) +
         "&lng=" + String(METROGNOME_LON, 6) +
         "&params=airTemperature,pressure,humidity,windSpeed,windDirection,waveHeight,swellPeriod,waterTemperature";
}

bool fetchStormglass() {
  if (!wifiOk) {
    apiStatus="WIFI";
    if (!connectBestWiFi()) return false;
  }
  syncTime();
  if (!canCallApi()) {
    apiStatus="LIMIT";
    return false;
  }
  apiStatus="CALL";
  smallStatus();
  drawBig();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = stormglassUrl();
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.useHTTP10(true);
  if (!http.begin(client, url)) {
    apiStatus="HTTP";
    return false;
  }
  http.addHeader("Authorization", STORMGLASS_API_KEY);
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  int code = http.GET();
  Serial.print("Stormglass HTTP code: "); Serial.println(code);
  Serial.print("Stormglass HTTP size: "); Serial.println(http.getSize());
  if (code != 200) {
    http.end();
    apiStatus="ERR";
    return false;
  }

  String payload = readHttpBodyManually(http,10000);
  http.end();
  Serial.print("Stormglass payload length: "); Serial.println(payload.length());
  Serial.println(payload.substring(0,300));
  if (payload.length()==0) {
    apiStatus="EMPTY";
    return false;
  }

  DynamicJsonDocument doc(24576);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("JSON error: "); Serial.println(err.c_str());
    apiStatus="JSON";
    return false;
  }

  JsonObject hour0 = doc["hours"][0];
  if (hour0.isNull()) {
    apiStatus="EMPTY";
    return false;
  }

  float v;
  v = firstSource(hour0["airTemperature"]); if (!isnan(v)) weather.airTemp=v;
  v = firstSource(hour0["pressure"]); if (!isnan(v)) weather.pressure=v;
  v = firstSource(hour0["humidity"]); if (!isnan(v)) weather.humidity=v;
  v = firstSource(hour0["windSpeed"]); if (!isnan(v)) weather.windSpeed=v;
  v = firstSource(hour0["windDirection"]); if (!isnan(v)) weather.windDirection=(int)v;
  v = firstSource(hour0["waveHeight"]); if (!isnan(v)) weather.waveHeight=v;
  v = firstSource(hour0["swellPeriod"]); if (!isnan(v)) weather.swellPeriod=v;
  v = firstSource(hour0["waterTemperature"]); if (!isnan(v)) weather.waterTemp=v;

  weather.valid=true;
  weather.stale=false;
  lastUpdate=utcTimeShort();
  recordApiCall();
  apiStatus="OK";
  Serial.println("Stormglass update OK.");
  return true;
}

void nextPage(){ page=(Page)((page+1)%PAGE_COUNT); }
void prevPage(){ page=(Page)((page+PAGE_COUNT-1)%PAGE_COUNT); }

void checkEncoder(){
  int clk=digitalRead(PIN_ENC_CLK);
  if(clk!=lastClk && clk==LOW){ int dt=digitalRead(PIN_ENC_DT); if(dt!=clk) nextPage(); else prevPage(); }
  lastClk=clk;
  uint32_t now=millis();
  bool swDown=(digitalRead(PIN_ENC_SW)==LOW);
  if(swDown && !buttonDown && now-lastButton>50) buttonDown=true;
  if(!swDown && buttonDown && now-lastButton>50){
    buttonDown=false; lastButton=now; pressNotice=true; pressUntil=now+1200;
    if(page==SYSTEM) fetchStormglass();
  }
  if(pressNotice && now>pressUntil) pressNotice=false;
}

void smallStatus(){
  smallDisplay.clearBuffer();
  smallDisplay.setFont(u8g2_font_4x6_tf);
  smallDisplay.drawStr(0,6,"METROGNOME");
  smallDisplay.setCursor(0,15); smallDisplay.print("WiFi "); smallDisplay.print(wifiStatus);
  smallDisplay.setCursor(0,24); smallDisplay.print("API  "); smallDisplay.print(apiStatus);
  smallDisplay.setCursor(0,33); smallDisplay.print("PAGE "); smallDisplay.print(pageNames[page]);
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
  bigDisplay.setFont(u8g2_font_7x14B_tf);
  bigDisplay.setCursor(0,29);
  if(weather.valid){ bigDisplay.print(weather.airTemp,1); bigDisplay.print(" C"); }
  else bigDisplay.print("--.- C");
  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.setCursor(72,23); bigDisplay.print("WIND");
  bigDisplay.setCursor(72,34);
  if(weather.valid){ bigDisplay.print(weather.windSpeed,1); bigDisplay.print("m/s"); }
  else bigDisplay.print("--");
  bigDisplay.drawFrame(0,36,128,28);
  bigDisplay.setCursor(4,48); bigDisplay.print("Pressure "); if(weather.valid) bigDisplay.print(weather.pressure,0); else bigDisplay.print("--"); bigDisplay.print(" hPa");
  bigDisplay.setCursor(4,60); bigDisplay.print("Hum "); if(weather.valid) bigDisplay.print(weather.humidity,0); else bigDisplay.print("--"); bigDisplay.print("% Upd "); bigDisplay.print(lastUpdate);
  bigDisplay.sendBuffer();
}

void drawSea(){
  bigDisplay.clearBuffer(); header("SEA");
  bigDisplay.setFont(u8g2_font_7x14B_tf);
  bigDisplay.setCursor(0,29);
  if(weather.valid){ bigDisplay.print(weather.waveHeight,1); bigDisplay.print(" m"); }
  else bigDisplay.print("--.- m");
  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.setCursor(72,23); bigDisplay.print("SWELL");
  bigDisplay.setCursor(72,34);
  if(weather.valid){ bigDisplay.print(weather.swellPeriod,1); bigDisplay.print("s"); }
  else bigDisplay.print("--");
  bigDisplay.drawFrame(0,38,128,24);
  bigDisplay.setCursor(4,49); bigDisplay.print("Water "); if(weather.valid) bigDisplay.print(weather.waterTemp,1); else bigDisplay.print("--.-"); bigDisplay.print(" C");
  bigDisplay.setCursor(4,60); bigDisplay.print(weather.valid ? "Stormglass live" : "Press SYSTEM update");
  bigDisplay.sendBuffer();
}

void drawTide(){
  bigDisplay.clearBuffer(); header("TIDE");
  bigDisplay.setFont(u8g2_font_6x12_tf);
  bigDisplay.drawStr(0,28,"HIGH  --:--");
  bigDisplay.drawStr(0,43,"LOW   --:--");
  bigDisplay.drawStr(0,58,"TIDE API LATER");
  bigDisplay.drawFrame(82,18,38,40);
  uint8_t h=8+((frame/2)%28);
  bigDisplay.drawBox(88,58-h,26,h);
  bigDisplay.sendBuffer();
}

void drawMoon(){
  bigDisplay.clearBuffer(); header("MOON");
  bigDisplay.setFont(u8g2_font_6x12_tf);
  bigDisplay.drawStr(0,28,"Moon data later");
  bigDisplay.drawStr(0,43,"NTP ready soon");
  bigDisplay.drawStr(0,58,"Stars await");
  bigDisplay.drawCircle(104,38,18);
  bigDisplay.drawDisc(110,38,16);
  bigDisplay.sendBuffer();
}

void drawLightning(){
  bigDisplay.clearBuffer(); header("LIGHT");
  bigDisplay.setFont(u8g2_font_6x12_tf);
  bigDisplay.drawStr(0,27,"Nearest strike");
  bigDisplay.setFont(u8g2_font_7x14B_tf);
  bigDisplay.setCursor(0,45); bigDisplay.print(lightning.direction); bigDisplay.print(" "); bigDisplay.print(lightning.nearestKm,0); bigDisplay.print("km");
  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.setCursor(0,60); bigDisplay.print(lightning.strikeCount); bigDisplay.print(" strikes/hr RISK HIGH");
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
  bigDisplay.clearBuffer(); header("SYSTEM");
  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.setCursor(0,24); bigDisplay.print("WiFi "); bigDisplay.print(wifiStatus);
  bigDisplay.setCursor(0,36); bigDisplay.print("API "); bigDisplay.print(apiStatus); bigDisplay.print(" "); bigDisplay.print(callsToday); bigDisplay.print("/"); bigDisplay.print(DAILY_API_LIMIT);
  bigDisplay.setCursor(0,48); bigDisplay.print("Last "); bigDisplay.print(lastUpdate);
  bigDisplay.setCursor(0,60); bigDisplay.print("PRESS: UPDATE ALL");
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
    default:drawWeather();break;
  }
}

void setup(){
  Serial.begin(115200);
  delay(1000);
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
  syncTime();
  loadApiCounter();
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
