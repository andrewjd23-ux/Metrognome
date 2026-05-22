#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <U8g2lib.h>
#include "secrets.h"

constexpr uint8_t PIN_I2C_SDA=5, PIN_I2C_SCL=6;
constexpr uint8_t PIN_SPI_SCK=4, PIN_SPI_MOSI=7, PIN_BIG_RST=8, PIN_BIG_DC=9, PIN_BIG_CS=10;
constexpr uint8_t PIN_ENC_CLK=0, PIN_ENC_DT=1, PIN_ENC_SW=2;

constexpr int DAILY_API_LIMIT=10;
constexpr int RESERVED_CALLS=1;

U8G2_SSD1306_72X40_ER_F_HW_I2C smallDisplay(U8G2_R0, U8X8_PIN_NONE);
U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI bigDisplay(U8G2_R0, PIN_BIG_CS, PIN_BIG_DC, PIN_BIG_RST);

enum Page:uint8_t{WEATHER,SEA,TIDE,MOON,LIGHTNING,SYSTEM,PAGE_COUNT};
const char* pageNames[PAGE_COUNT]={"WEATHER","SEA","TIDE","MOON","LIGHT","SYSTEM"};
Page page=WEATHER;

uint32_t frame=0,lastDraw=0,lastButton=0,pressUntil=0,lastWifiCheck=0;
int lastClk=HIGH;
bool buttonDown=false,pressNotice=false,wifiOk=false;
String wifiStatus="BOOT",connectedSsid="";
String apiStatus="IDLE",lastUpdate="never";
Preferences prefs;
int callsToday=0;
String apiDate="";

struct WeatherData{bool valid=false;float airTemp=0,pressure=0,humidity=0,windSpeed=0,waveHeight=0,swellPeriod=0,waterTemp=0;int windDirection=0;};
struct TideGaugeData{bool valid=false;String label="Gauge",time="--:--",trend="--";float level=NAN,lastLevel=NAN;};
struct MoonData{bool valid=false;String phase="--";float age=0,illum=0;};
struct LightningData{int strikeCount=12;float nearestKm=34.0;const char* direction="NW";};

WeatherData weather;
TideGaugeData tide;
MoonData moon;
LightningData lightning;

void smallStatus(); void drawBig();

float firstSource(JsonVariant v){
  if(v.isNull()) return NAN;
  const char* sources[]={"sg","noaa","meteo","dwd","icon","yr","smhi","fcoo"};
  for(const char* s:sources) if(!v[s].isNull()) return v[s].as<float>();
  return NAN;
}

String readHttpBodyManually(HTTPClient& http,uint32_t timeoutMs=10000){
  String payload="";
  WiFiClient* stream=http.getStreamPtr();
  uint32_t lastDataMs=millis();
  while(http.connected()&&millis()-lastDataMs<timeoutMs){
    while(stream->available()){
      payload+=char(stream->read());
      lastDataMs=millis();
    }
    delay(10);
  }
  return payload;
}

String utcDate(){struct tm t;if(!getLocalTime(&t))return "";char b[11];strftime(b,sizeof(b),"%Y-%m-%d",&t);return String(b);}
String utcTimeShort(){struct tm t;if(!getLocalTime(&t))return "--:--";char b[6];strftime(b,sizeof(b),"%H:%M",&t);return String(b);}
String isoUtcNowPlusHours(int h){time_t now;time(&now);now+=h*3600;struct tm* u=gmtime(&now);char b[32];strftime(b,sizeof(b),"%Y-%m-%dT%H:00:00Z",u);return String(b);}
String hhmmFromIso(String iso){return iso.length()>=16?iso.substring(11,16):"--:--";}

void loadApiCounter(){
  prefs.begin("metrognome",false);
  String today=utcDate();
  apiDate=prefs.getString("api_date","");
  callsToday=prefs.getInt("api_calls",0);
  if(today!=""&&today!=apiDate){
    apiDate=today;callsToday=0;
    prefs.putString("api_date",apiDate);
    prefs.putInt("api_calls",callsToday);
  }
}

bool canCallApi(){loadApiCounter();return callsToday<(DAILY_API_LIMIT-RESERVED_CALLS);}
void recordApiCall(){callsToday++;prefs.putInt("api_calls",callsToday);}

bool connectToNetwork(const char* ssid,const char* password,uint32_t timeoutMs=12000){
  if(ssid==nullptr||strlen(ssid)==0)return false;
  WiFi.disconnect(true);delay(250);
  wifiStatus="JOIN";connectedSsid=String(ssid);
  Serial.print("Trying WiFi: ");Serial.println(ssid);
  if(password==nullptr||strlen(password)==0)WiFi.begin(ssid);else WiFi.begin(ssid,password);
  uint32_t start=millis();
  while(WiFi.status()!=WL_CONNECTED&&millis()-start<timeoutMs){delay(250);Serial.print(".");frame++;smallStatus();}
  Serial.println();
  if(WiFi.status()==WL_CONNECTED){
    wifiOk=true;wifiStatus="OK";connectedSsid=String(ssid);
    Serial.print("Connected: ");Serial.println(connectedSsid);
    Serial.print("IP: ");Serial.println(WiFi.localIP());
    return true;
  }
  return false;
}

bool connectToStrongestOpenNetwork(uint32_t timeoutMs=12000){
  wifiStatus="SCAN";connectedSsid="OPEN?";smallStatus();
  int n=WiFi.scanNetworks(); if(n<=0)return false;
  int best=-1,bestRssi=-999;
  for(int i=0;i<n;i++) if(WiFi.encryptionType(i)==WIFI_AUTH_OPEN&&WiFi.RSSI(i)>bestRssi){bestRssi=WiFi.RSSI(i);best=i;}
  if(best<0)return false;
  String ssid=WiFi.SSID(best);
  return connectToNetwork(ssid.c_str(),"",timeoutMs);
}

bool connectBestWiFi(){
  WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(true);WiFi.persistent(false);
  wifiOk=false;wifiStatus="JOIN";connectedSsid="";
  if(connectToNetwork(OFFICE_WIFI_SSID,OFFICE_WIFI_PASSWORD))return true;
  for(size_t i=0;i<KNOWN_WIFI_COUNT;i++) if(connectToNetwork(KNOWN_WIFI_SSIDS[i],KNOWN_WIFI_PASSWORDS[i]))return true;
  if(ALLOW_OPEN_WIFI_FALLBACK&&connectToStrongestOpenNetwork())return true;
  wifiOk=false;wifiStatus="FAIL";connectedSsid="";
  return false;
}

void updateWifiStatus(){
  wifiOk=(WiFi.status()==WL_CONNECTED);
  if(wifiOk){wifiStatus="OK";connectedSsid=WiFi.SSID();}
  else{wifiStatus="FAIL";connectedSsid="";}
}

void syncTime(){
  configTime(0,0,"pool.ntp.org","time.nist.gov");
  uint32_t start=millis();
  while(millis()-start<8000){struct tm t;if(getLocalTime(&t))return;delay(250);}
}

void updateMoon(){
  time_t now;time(&now);
  if(now<1600000000){moon.valid=false;return;}
  const double synodic=29.530588853;
  const double knownNewMoon=947182440.0;
  double days=(double(now)-knownNewMoon)/86400.0;
  double age=fmod(days,synodic); if(age<0)age+=synodic;
  moon.age=age;
  moon.illum=(1.0-cos((age/synodic)*2.0*PI))*50.0;
  moon.valid=true;
  if(age<1.85)moon.phase="New";
  else if(age<5.54)moon.phase="Wax cres";
  else if(age<9.23)moon.phase="First qtr";
  else if(age<12.92)moon.phase="Wax gibb";
  else if(age<16.61)moon.phase="Full";
  else if(age<20.30)moon.phase="Wan gibb";
  else if(age<23.99)moon.phase="Last qtr";
  else if(age<27.68)moon.phase="Wan cres";
  else moon.phase="New";
}

String stormglassUrl(){
  return String("https://api.stormglass.io/v2/weather/point?lat=")+
    String(METROGNOME_LAT,6)+
    "&lng="+String(METROGNOME_LON,6)+
    "&params=airTemperature,pressure,humidity,windSpeed,windDirection,waveHeight,swellPeriod,waterTemperature"+
    "&source=sg"+
    "&start="+isoUtcNowPlusHours(0)+
    "&end="+isoUtcNowPlusHours(3);
}

bool getStormglassJson(DynamicJsonDocument& doc){
  WiFiClientSecure client;client.setInsecure();
  HTTPClient http;
  String url=stormglassUrl();
  Serial.println(url);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.useHTTP10(true);
  if(!http.begin(client,url)){apiStatus="HTTP";return false;}
  http.addHeader("Authorization",STORMGLASS_API_KEY);
  http.addHeader("Accept","application/json");
  http.addHeader("Accept-Encoding","identity");
  int code=http.GET();
  Serial.print("Stormglass code: ");Serial.println(code);
  Serial.print("Stormglass size: ");Serial.println(http.getSize());
  if(code!=200){http.end();apiStatus="ERR";return false;}
  String payload=readHttpBodyManually(http,10000);
  http.end();
  Serial.print("Stormglass len: ");Serial.println(payload.length());
  Serial.println(payload.substring(0,240));
  if(payload.length()==0){apiStatus="EMPTY";return false;}
  DeserializationError err=deserializeJson(doc,payload);
  if(err){Serial.print("JSON error: ");Serial.println(err.c_str());apiStatus="JSON";return false;}
  return true;
}

bool fetchStormglass(){
  if(!wifiOk){apiStatus="WIFI";if(!connectBestWiFi())return false;}
  syncTime();updateMoon();
  if(!canCallApi()){apiStatus="LIMIT";return false;}
  apiStatus="CALL";smallStatus();drawBig();

  DynamicJsonDocument doc(24576);
  if(!getStormglassJson(doc))return false;

  JsonObject h=doc["hours"][0];
  if(h.isNull()){apiStatus="EMPTY";return false;}

  float v;
  v=firstSource(h["airTemperature"]);if(!isnan(v))weather.airTemp=v;
  v=firstSource(h["pressure"]);if(!isnan(v))weather.pressure=v;
  v=firstSource(h["humidity"]);if(!isnan(v))weather.humidity=v;
  v=firstSource(h["windSpeed"]);if(!isnan(v))weather.windSpeed=v;
  v=firstSource(h["windDirection"]);if(!isnan(v))weather.windDirection=(int)v;
  v=firstSource(h["waveHeight"]);if(!isnan(v))weather.waveHeight=v;
  v=firstSource(h["swellPeriod"]);if(!isnan(v))weather.swellPeriod=v;
  v=firstSource(h["waterTemperature"]);if(!isnan(v))weather.waterTemp=v;

  weather.valid=true;
  recordApiCall();
  lastUpdate=utcTimeShort();
  apiStatus="OK";
  return true;
}

bool fetchTideGauge(){
  if(!wifiOk){apiStatus="WIFI";if(!connectBestWiFi())return false;}

  apiStatus="TIDE";
  smallStatus();drawBig();

  WiFiClient client;
  HTTPClient http;

  String url=String(EA_TIDE_READINGS_URL);
  Serial.println(url);

  http.setTimeout(12000);
  if(!http.begin(client,url)){apiStatus="THTTP";return false;}

  int code=http.GET();
  Serial.print("EA tide code: ");Serial.println(code);

  if(code!=200){
    http.end();
    apiStatus="TERR";
    return false;
  }

  String payload=http.getString();
  http.end();

  Serial.print("EA tide len: ");Serial.println(payload.length());
  Serial.println(payload.substring(0,240));

  DynamicJsonDocument doc(12288);
  DeserializationError err=deserializeJson(doc,payload);
  if(err){
    Serial.print("EA JSON error: ");Serial.println(err.c_str());
    apiStatus="TJSON";
    return false;
  }

  JsonVariant item;
  if(!doc["items"].isNull()){
    if(doc["items"].is<JsonArray>())item=doc["items"][0];
    else item=doc["items"];
  }

  if(item.isNull()){
    apiStatus="TEMPT";
    return false;
  }

  float level=item["value"]|NAN;
  String time=item["dateTime"]|"";

  if(isnan(level)){
    apiStatus="TNAN";
    return false;
  }

  tide.label=String(EA_TIDE_LABEL);
  tide.valid=true;
  tide.lastLevel=tide.level;
  tide.level=level;
  tide.time=hhmmFromIso(time);

  if(isnan(tide.lastLevel))tide.trend="first";
  else if(tide.level>tide.lastLevel+0.01)tide.trend="rising";
  else if(tide.level<tide.lastLevel-0.01)tide.trend="falling";
  else tide.trend="steady";

  apiStatus="TOK";
  return true;
}

void nextPage(){page=(Page)((page+1)%PAGE_COUNT);}
void prevPage(){page=(Page)((page+PAGE_COUNT-1)%PAGE_COUNT);}

void checkEncoder(){
  int clk=digitalRead(PIN_ENC_CLK);
  if(clk!=lastClk&&clk==LOW){
    int dt=digitalRead(PIN_ENC_DT);
    if(dt!=clk)nextPage();else prevPage();
  }
  lastClk=clk;

  uint32_t now=millis();
  bool swDown=(digitalRead(PIN_ENC_SW)==LOW);

  if(swDown&&!buttonDown&&now-lastButton>50)buttonDown=true;

  if(!swDown&&buttonDown&&now-lastButton>50){
    buttonDown=false;
    lastButton=now;
    pressNotice=true;
    pressUntil=now+1200;

    if(page==SYSTEM)fetchStormglass();
    else if(page==TIDE)fetchTideGauge();
  }

  if(pressNotice&&now>pressUntil)pressNotice=false;
}

void smallStatus(){
  smallDisplay.clearBuffer();
  smallDisplay.setFont(u8g2_font_4x6_tf);
  smallDisplay.drawStr(0,6,"METROGNOME");
  smallDisplay.setCursor(0,15);smallDisplay.print("WiFi ");smallDisplay.print(wifiStatus);
  smallDisplay.setCursor(0,24);smallDisplay.print("API  ");smallDisplay.print(apiStatus);
  smallDisplay.setCursor(0,33);smallDisplay.print("PAGE ");smallDisplay.print(pageNames[page]);
  if((frame/4)%2==0)smallDisplay.drawBox(66,0,5,5);else smallDisplay.drawFrame(66,0,5,5);
  smallDisplay.sendBuffer();
}

void header(const char* title){
  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.drawStr(0,9,"METROGNOME");
  bigDisplay.setCursor(72,9);bigDisplay.print(title);
  bigDisplay.drawHLine(0,12,128);
}

void drawWeather(){
  bigDisplay.clearBuffer();header("WEATHER");
  bigDisplay.setFont(u8g2_font_7x14B_tf);
  bigDisplay.setCursor(0,29);
  if(weather.valid){bigDisplay.print(weather.airTemp,1);bigDisplay.print(" C");}
  else bigDisplay.print("--.- C");

  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.setCursor(72,23);bigDisplay.print("WIND");
  bigDisplay.setCursor(72,34);
  if(weather.valid){bigDisplay.print(weather.windSpeed,1);bigDisplay.print("m/s");}
  else bigDisplay.print("--");

  bigDisplay.drawFrame(0,36,128,28);
  bigDisplay.setCursor(4,48);bigDisplay.print("Pressure ");
  if(weather.valid)bigDisplay.print(weather.pressure,0);else bigDisplay.print("--");
  bigDisplay.print(" hPa");

  bigDisplay.setCursor(4,60);bigDisplay.print("Hum ");
  if(weather.valid)bigDisplay.print(weather.humidity,0);else bigDisplay.print("--");
  bigDisplay.print("% Upd ");bigDisplay.print(lastUpdate);
  bigDisplay.sendBuffer();
}

void drawSea(){
  bigDisplay.clearBuffer();header("SEA");
  bigDisplay.setFont(u8g2_font_7x14B_tf);
  bigDisplay.setCursor(0,29);
  if(weather.valid){bigDisplay.print(weather.waveHeight,1);bigDisplay.print(" m");}
  else bigDisplay.print("--.- m");

  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.setCursor(72,23);bigDisplay.print("SWELL");
  bigDisplay.setCursor(72,34);
  if(weather.valid){bigDisplay.print(weather.swellPeriod,1);bigDisplay.print("s");}
  else bigDisplay.print("--");

  bigDisplay.drawFrame(0,38,128,24);
  bigDisplay.setCursor(4,49);bigDisplay.print("Water ");
  if(weather.valid)bigDisplay.print(weather.waterTemp,1);else bigDisplay.print("--.-");
  bigDisplay.print(" C");

  bigDisplay.setCursor(4,60);bigDisplay.print(weather.valid?"Stormglass live":"Press SYSTEM update");
  bigDisplay.sendBuffer();
}

void drawTide(){
  bigDisplay.clearBuffer();header("TIDE");
  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.setCursor(0,24);bigDisplay.print(tide.label);
  bigDisplay.setFont(u8g2_font_7x14B_tf);
  bigDisplay.setCursor(0,43);
  if(tide.valid){bigDisplay.print(tide.level,2);bigDisplay.print(" m");}
  else bigDisplay.print("--.-- m");

  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.setCursor(0,57);
  if(tide.valid){
    bigDisplay.print(tide.trend);
    bigDisplay.print(" ");
    bigDisplay.print(tide.time);
  }else bigDisplay.print("PRESS: GAUGE UPDATE");

  bigDisplay.drawFrame(91,20,30,40);
  int h=10;
  if(tide.valid)h=constrain((int)(tide.level*5),4,38);
  bigDisplay.drawBox(96,58-h,20,h);
  bigDisplay.sendBuffer();
}

void drawMoon(){
  bigDisplay.clearBuffer();header("MOON");
  updateMoon();
  bigDisplay.setFont(u8g2_font_6x12_tf);
  bigDisplay.setCursor(0,28);bigDisplay.print(moon.valid?moon.phase:"No time");
  bigDisplay.setCursor(0,43);bigDisplay.print("Illum ");
  if(moon.valid)bigDisplay.print(moon.illum,0);else bigDisplay.print("--");
  bigDisplay.print("%");
  bigDisplay.setCursor(0,58);bigDisplay.print("Age ");
  if(moon.valid)bigDisplay.print(moon.age,1);else bigDisplay.print("--");
  bigDisplay.print("d");

  bigDisplay.drawCircle(104,38,18);
  if(moon.valid&&moon.illum>50)bigDisplay.drawDisc(110,38,16);
  else bigDisplay.drawDisc(98,38,16);
  bigDisplay.sendBuffer();
}

void drawLightning(){
  bigDisplay.clearBuffer();header("LIGHT");
  bigDisplay.setFont(u8g2_font_6x12_tf);
  bigDisplay.drawStr(0,27,"Nearest strike");
  bigDisplay.setFont(u8g2_font_7x14B_tf);
  bigDisplay.setCursor(0,45);bigDisplay.print(lightning.direction);bigDisplay.print(" ");bigDisplay.print(lightning.nearestKm,0);bigDisplay.print("km");
  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.setCursor(0,60);bigDisplay.print(lightning.strikeCount);bigDisplay.print(" strikes/hr RISK HIGH");

  bigDisplay.drawCircle(103,38,21);bigDisplay.drawCircle(103,38,12);
  bigDisplay.drawLine(103,17,103,59);bigDisplay.drawLine(82,38,124,38);
  bigDisplay.drawCircle(103,38,((frame*3)%18)+2);

  int sx=96+((frame*5)%18),sy=25+((frame*7)%25);
  bigDisplay.drawLine(sx,sy,sx-5,sy+8);
  bigDisplay.drawLine(sx-5,sy+8,sx+1,sy+8);
  bigDisplay.drawLine(sx+1,sy+8,sx-6,sy+18);
  bigDisplay.sendBuffer();
}

void drawSystem(){
  bigDisplay.clearBuffer();header("SYSTEM");
  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.setCursor(0,24);bigDisplay.print("WiFi ");bigDisplay.print(wifiStatus);
  bigDisplay.setCursor(0,36);bigDisplay.print("API ");bigDisplay.print(apiStatus);bigDisplay.print(" ");bigDisplay.print(callsToday);bigDisplay.print("/");bigDisplay.print(DAILY_API_LIMIT);
  bigDisplay.setCursor(0,48);bigDisplay.print("Last ");bigDisplay.print(lastUpdate);
  bigDisplay.setCursor(0,60);bigDisplay.print("PRESS: WEATHER");
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
  Serial.begin(115200);delay(1000);
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

  tide.label=String(EA_TIDE_LABEL);

  smallStatus();
  drawBig();
  connectBestWiFi();
  syncTime();
  loadApiCounter();
  updateMoon();
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
