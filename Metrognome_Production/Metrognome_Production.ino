// Metrognome_Production.ino
// Production-lean Metrognome build after retiring the dud compass module.
//
// Hardware:
//   ESP32-C3 Super Mini
//   Small I2C OLED: SDA GPIO5, SCL GPIO6
//   Big SPI OLED: SCK GPIO4, MOSI GPIO7, RST GPIO8, DC GPIO9, CS GPIO10
//   Rotary encoder: CLK GPIO0, DT GPIO1, SW GPIO2
//   SG90 servo: signal GPIO3, red 5V rail, brown/black common GND
//
// secrets.h should define WiFi, Stormglass, and location values used by the older builds.
// Optional direct lightning feed:
//   const char* LIGHTNING_FEED_URL = "http://example.local/lightning.json";
// If LIGHTNING_FEED_URL is absent or empty, LIGHT mode uses a demo strike.

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <U8g2lib.h>
#include <ESP32Servo.h>
#include "secrets.h"

#ifndef LIGHTNING_FEED_URL
#define LIGHTNING_FEED_URL ""
#endif

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
constexpr uint8_t PIN_SERVO_SIGNAL = 3;

constexpr int DAILY_API_LIMIT = 10;
constexpr int RESERVED_CALLS = 1;

constexpr int SERVO_LEFT_LIMIT = 10;
constexpr int SERVO_CENTER = 90;
constexpr int SERVO_RIGHT_LIMIT = 170;
constexpr bool INVERT_SERVO = false;

// Manual north mode.
// Place the gnome using a real compass, then set this to the bearing the gnome's FRONT faces.
// Front north = 0, east = 90, south = 180, west = 270.
constexpr float GNOME_FRONT_BEARING_DEG = 0.0;

U8G2_SSD1306_72X40_ER_F_HW_I2C smallDisplay(U8G2_R0, U8X8_PIN_NONE);
U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI bigDisplay(U8G2_R0, PIN_BIG_CS, PIN_BIG_DC, PIN_BIG_RST);
Servo lightningServo;

Preferences prefs;

enum Page : uint8_t { WEATHER, SEA, TIDE, MOON, LIGHTNING, SYSTEM, PAGE_COUNT };
const char* pageNames[PAGE_COUNT] = {"WEATHER", "SEA", "TIDE", "MOON", "LIGHT", "SYSTEM"};
Page page = WEATHER;

uint32_t frame = 0;
uint32_t lastDraw = 0;
uint32_t lastButton = 0;
uint32_t pressUntil = 0;
uint32_t lastWifiCheck = 0;
int lastClk = HIGH;
bool buttonDown = false;
bool pressNotice = false;
bool wifiOk = false;
String wifiStatus = "BOOT";
String connectedSsid = "";

int callsToday = 0;
String apiDate = "";
String apiStatus = "IDLE";
String lastUpdate = "never";

struct WeatherData {
  bool valid = false;
  bool stale = true;
  float airTemp = 0;
  float pressure = 0;
  float humidity = 0;
  float windSpeed = 0;
  int windDirection = 0;
  float waveHeight = 0;
  float swellPeriod = 0;
  float waterTemp = 0;
};

struct LightningData {
  bool active = false;
  int strikeCount = 0;
  float nearestKm = 0.0;
  String direction = "--";
  int risk = 0;
  float azimuthDeg = 0.0;
  float headingDeg = 0.0;
  float relativeDeg = 0.0;
  int servoAngle = SERVO_CENTER;
  String status = "READY";
  String lastUpdate = "never";
};

WeatherData weather;
LightningData lightning;

void smallStatus();
void drawBig();
bool connectBestWiFi();
String utcTimeShort();

float normalize360(float angle) {
  while (angle < 0.0) angle += 360.0;
  while (angle >= 360.0) angle -= 360.0;
  return angle;
}

float signedRelativeAngle(float targetBearing, float currentHeading) {
  float rel = normalize360(targetBearing - currentHeading);
  if (rel > 180.0) rel -= 360.0;
  return rel;
}

String bearingToCardinal(float bearing) {
  const char* dirs[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  int index = (int)((normalize360(bearing) + 22.5) / 45.0) % 8;
  return String(dirs[index]);
}

int relativeToServo(float relativeAngle) {
  relativeAngle = constrain(relativeAngle, -90.0, 90.0);
  int angle = SERVO_CENTER + (int)relativeAngle;
  if (INVERT_SERVO) angle = SERVO_CENTER - (int)relativeAngle;
  return constrain(angle, SERVO_LEFT_LIMIT, SERVO_RIGHT_LIMIT);
}

float readManualHeading() {
  return normalize360(GNOME_FRONT_BEARING_DEG);
}

double degToRad(double d) {
  return d * PI / 180.0;
}

float distanceKmBetween(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371.0;
  double dLat = degToRad(lat2 - lat1);
  double dLon = degToRad(lon2 - lon1);
  double a = sin(dLat / 2) * sin(dLat / 2) +
             cos(degToRad(lat1)) * cos(degToRad(lat2)) *
             sin(dLon / 2) * sin(dLon / 2);
  double c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return (float)(R * c);
}

float bearingDegBetween(double lat1, double lon1, double lat2, double lon2) {
  double p1 = degToRad(lat1);
  double p2 = degToRad(lat2);
  double dLon = degToRad(lon2 - lon1);
  double y = sin(dLon) * cos(p2);
  double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dLon);
  return normalize360((float)(atan2(y, x) * 180.0 / PI));
}

float firstSource(JsonVariant v) {
  if (v.isNull()) return NAN;
  const char* sources[] = {"sg", "noaa", "meteo", "dwd", "icon", "yr", "smhi", "fcoo"};
  for (const char* s : sources) {
    if (!v[s].isNull()) return v[s].as<float>();
  }
  return NAN;
}

String readHttpBodyManually(HTTPClient& http, uint32_t timeoutMs = 10000) {
  String payload = "";
  WiFiClient* stream = http.getStreamPtr();
  uint32_t lastDataMs = millis();
  while (http.connected() && millis() - lastDataMs < timeoutMs) {
    while (stream->available()) {
      payload += char(stream->read());
      lastDataMs = millis();
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

bool connectToNetwork(const char* ssid, const char* password, uint32_t timeoutMs = 12000) {
  if (ssid == nullptr || strlen(ssid) == 0) return false;

  WiFi.disconnect(true);
  delay(250);
  wifiStatus = "JOIN";
  connectedSsid = String(ssid);
  smallStatus();

  Serial.print("Trying WiFi: ");
  Serial.println(ssid);

  if (password == nullptr || strlen(password) == 0) WiFi.begin(ssid);
  else WiFi.begin(ssid, password);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
    Serial.print(".");
    frame++;
    smallStatus();
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiOk = true;
    wifiStatus = "OK";
    connectedSsid = String(ssid);
    Serial.print("Connected: "); Serial.println(connectedSsid);
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    Serial.print("RSSI: "); Serial.println(WiFi.RSSI());
    return true;
  }

  Serial.print("Failed: "); Serial.println(ssid);
  return false;
}

bool connectToStrongestOpenNetwork(uint32_t timeoutMs = 12000) {
  wifiStatus = "SCAN";
  connectedSsid = "OPEN?";
  smallStatus();

  int n = WiFi.scanNetworks();
  if (n <= 0) return false;

  int best = -1;
  int bestRssi = -999;
  for (int i = 0; i < n; i++) {
    if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN && WiFi.RSSI(i) > bestRssi) {
      bestRssi = WiFi.RSSI(i);
      best = i;
    }
  }

  if (best < 0) return false;
  String openSsid = WiFi.SSID(best);
  return connectToNetwork(openSsid.c_str(), "", timeoutMs);
}

bool connectBestWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  wifiOk = false;
  wifiStatus = "JOIN";
  connectedSsid = "";

  if (connectToNetwork(OFFICE_WIFI_SSID, OFFICE_WIFI_PASSWORD)) return true;
  for (size_t i = 0; i < KNOWN_WIFI_COUNT; i++) {
    if (connectToNetwork(KNOWN_WIFI_SSIDS[i], KNOWN_WIFI_PASSWORDS[i])) return true;
  }
  if (ALLOW_OPEN_WIFI_FALLBACK) {
    if (connectToStrongestOpenNetwork()) return true;
  }

  wifiOk = false;
  wifiStatus = "FAIL";
  connectedSsid = "";
  return false;
}

void updateWifiStatus() {
  wifiOk = (WiFi.status() == WL_CONNECTED);
  if (wifiOk) {
    wifiStatus = "OK";
    connectedSsid = WiFi.SSID();
  } else {
    wifiStatus = "FAIL";
    connectedSsid = "";
  }
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
  time_t now;
  time(&now);
  struct tm* utc = gmtime(&now);
  char startBuf[32];
  char endBuf[32];
  strftime(startBuf, sizeof(startBuf), "%Y-%m-%dT%H:00:00Z", utc);
  now += 3 * 3600;
  utc = gmtime(&now);
  strftime(endBuf, sizeof(endBuf), "%Y-%m-%dT%H:00:00Z", utc);

  return String("https://api.stormglass.io/v2/weather/point?lat=") +
         String(METROGNOME_LAT, 6) +
         "&lng=" + String(METROGNOME_LON, 6) +
         "&params=airTemperature,pressure,humidity,windSpeed,windDirection,waveHeight,swellPeriod,waterTemperature" +
         "&source=sg" +
         "&start=" + String(startBuf) +
         "&end=" + String(endBuf);
}

bool fetchStormglass() {
  if (!wifiOk) {
    apiStatus = "WIFI";
    if (!connectBestWiFi()) return false;
  }

  syncTime();
  if (!canCallApi()) {
    apiStatus = "LIMIT";
    return false;
  }

  apiStatus = "CALL";
  smallStatus();
  drawBig();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = stormglassUrl();
  Serial.println(url);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.useHTTP10(true);

  if (!http.begin(client, url)) {
    apiStatus = "HTTP";
    return false;
  }

  http.addHeader("Authorization", STORMGLASS_API_KEY);
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");

  int code = http.GET();
  Serial.print("Stormglass HTTP code: "); Serial.println(code);
  if (code != 200) {
    http.end();
    apiStatus = "ERR";
    return false;
  }

  String payload = readHttpBodyManually(http, 10000);
  http.end();
  if (payload.length() == 0) {
    apiStatus = "EMPTY";
    return false;
  }

  DynamicJsonDocument doc(24576);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("JSON error: "); Serial.println(err.c_str());
    apiStatus = "JSON";
    return false;
  }

  JsonObject hour0 = doc["hours"][0];
  if (hour0.isNull()) {
    apiStatus = "EMPTY";
    return false;
  }

  float v;
  v = firstSource(hour0["airTemperature"]); if (!isnan(v)) weather.airTemp = v;
  v = firstSource(hour0["pressure"]); if (!isnan(v)) weather.pressure = v;
  v = firstSource(hour0["humidity"]); if (!isnan(v)) weather.humidity = v;
  v = firstSource(hour0["windSpeed"]); if (!isnan(v)) weather.windSpeed = v;
  v = firstSource(hour0["windDirection"]); if (!isnan(v)) weather.windDirection = (int)v;
  v = firstSource(hour0["waveHeight"]); if (!isnan(v)) weather.waveHeight = v;
  v = firstSource(hour0["swellPeriod"]); if (!isnan(v)) weather.swellPeriod = v;
  v = firstSource(hour0["waterTemperature"]); if (!isnan(v)) weather.waterTemp = v;

  weather.valid = true;
  weather.stale = false;
  lastUpdate = utcTimeShort();
  recordApiCall();
  apiStatus = "OK";
  Serial.println("Stormglass update OK.");
  return true;
}

bool setLightningFromStrike(double strikeLat, double strikeLon, int strikeCount, const String& status) {
  lightning.nearestKm = distanceKmBetween(METROGNOME_LAT, METROGNOME_LON, strikeLat, strikeLon);
  lightning.azimuthDeg = bearingDegBetween(METROGNOME_LAT, METROGNOME_LON, strikeLat, strikeLon);
  lightning.direction = bearingToCardinal(lightning.azimuthDeg);
  lightning.strikeCount = strikeCount;
  lightning.active = true;
  lightning.status = status;
  lightning.lastUpdate = utcTimeShort();
  return true;
}

bool extractLatLon(JsonVariant item, double& lat, double& lon) {
  if (!item["lat"].isNull()) lat = item["lat"].as<double>();
  else if (!item["latitude"].isNull()) lat = item["latitude"].as<double>();
  else return false;

  if (!item["lon"].isNull()) lon = item["lon"].as<double>();
  else if (!item["lng"].isNull()) lon = item["lng"].as<double>();
  else if (!item["longitude"].isNull()) lon = item["longitude"].as<double>();
  else return false;

  return true;
}

bool parseLightningJson(const String& payload) {
  DynamicJsonDocument doc(32768);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("Lightning JSON error: "); Serial.println(err.c_str());
    lightning.status = "JSON";
    return false;
  }

  JsonArray strikes;
  if (doc.is<JsonArray>()) strikes = doc.as<JsonArray>();
  else if (doc["strikes"].is<JsonArray>()) strikes = doc["strikes"].as<JsonArray>();
  else if (doc["data"].is<JsonArray>()) strikes = doc["data"].as<JsonArray>();
  else if (doc["features"].is<JsonArray>()) strikes = doc["features"].as<JsonArray>();
  else {
    lightning.status = "NO ARRAY";
    return false;
  }

  bool found = false;
  double bestLat = 0;
  double bestLon = 0;
  float bestKm = 999999.0;
  int count = 0;

  for (JsonVariant item : strikes) {
    double lat = 0;
    double lon = 0;

    if (extractLatLon(item, lat, lon)) {
      // Direct lat/lon object.
    } else if (item["geometry"]["coordinates"].is<JsonArray>()) {
      JsonArray c = item["geometry"]["coordinates"].as<JsonArray>();
      if (c.size() >= 2) {
        lon = c[0].as<double>();
        lat = c[1].as<double>();
      } else {
        continue;
      }
    } else {
      continue;
    }

    count++;
    float km = distanceKmBetween(METROGNOME_LAT, METROGNOME_LON, lat, lon);
    if (km < bestKm) {
      bestKm = km;
      bestLat = lat;
      bestLon = lon;
      found = true;
    }
  }

  if (!found) {
    lightning.active = false;
    lightning.status = "NO STRIKE";
    return false;
  }

  return setLightningFromStrike(bestLat, bestLon, count, "LIVE");
}

bool fetchLightningDirect() {
  String url = String(LIGHTNING_FEED_URL);
  url.trim();

  if (url.length() == 0) {
    double demoLat = METROGNOME_LAT + 0.22;
    double demoLon = METROGNOME_LON - 0.36;
    return setLightningFromStrike(demoLat, demoLon, 1, "DEMO");
  }

  if (!wifiOk) {
    apiStatus = "WIFI";
    if (!connectBestWiFi()) {
      lightning.status = "WIFI FAIL";
      return false;
    }
  }

  HTTPClient http;
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  bool begun = false;
  WiFiClientSecure secureClient;
  if (url.startsWith("https://")) {
    secureClient.setInsecure();
    begun = http.begin(secureClient, url);
  } else {
    begun = http.begin(url);
  }

  if (!begun) {
    lightning.status = "HTTP";
    return false;
  }

  http.addHeader("Accept", "application/json");
  int code = http.GET();
  Serial.print("Lightning HTTP code: "); Serial.println(code);
  if (code != 200) {
    http.end();
    lightning.status = "HTTP ERR";
    return false;
  }

  String payload = http.getString();
  http.end();
  if (payload.length() == 0) {
    lightning.status = "EMPTY";
    return false;
  }

  return parseLightningJson(payload);
}

void setupLightningVane() {
  lightningServo.setPeriodHertz(50);
  lightningServo.attach(PIN_SERVO_SIGNAL, 500, 2400);
  lightningServo.write(SERVO_CENTER);
  lightning.status = "READY";
  lightning.headingDeg = readManualHeading();
  lightning.servoAngle = SERVO_CENTER;
  Serial.println("Manual-north lightning vane ready.");
}

void updateLightningPage() {
  apiStatus = "LIGHT";
  smallStatus();
  drawBig();

  bool gotStrike = fetchLightningDirect();
  float heading = readManualHeading();
  lightning.headingDeg = heading;

  if (!gotStrike) {
    lightning.active = false;
    lightning.relativeDeg = 0.0;
    lightning.servoAngle = SERVO_CENTER;
    lightningServo.write(SERVO_CENTER);
    apiStatus = "LIGHT ERR";
    return;
  }

  float relative = signedRelativeAngle(lightning.azimuthDeg, heading);
  int servoAngle = relativeToServo(relative);
  lightning.relativeDeg = relative;
  lightning.servoAngle = servoAngle;
  lightningServo.write(servoAngle);
  apiStatus = "LIGHT OK";

  Serial.println("---- LIGHTNING UPDATE ----");
  Serial.print("Distance km: "); Serial.println(lightning.nearestKm);
  Serial.print("Strike azimuth: "); Serial.println(lightning.azimuthDeg);
  Serial.print("Manual front bearing: "); Serial.println(lightning.headingDeg);
  Serial.print("Relative angle: "); Serial.println(lightning.relativeDeg);
  Serial.print("Servo angle: "); Serial.println(lightning.servoAngle);
}

void nextPage() { page = (Page)((page + 1) % PAGE_COUNT); }
void prevPage() { page = (Page)((page + PAGE_COUNT - 1) % PAGE_COUNT); }

void checkEncoder() {
  int clk = digitalRead(PIN_ENC_CLK);
  if (clk != lastClk && clk == LOW) {
    int dt = digitalRead(PIN_ENC_DT);
    if (dt != clk) nextPage();
    else prevPage();
  }
  lastClk = clk;

  uint32_t now = millis();
  bool swDown = (digitalRead(PIN_ENC_SW) == LOW);
  if (swDown && !buttonDown && now - lastButton > 50) buttonDown = true;
  if (!swDown && buttonDown && now - lastButton > 50) {
    buttonDown = false;
    lastButton = now;
    pressNotice = true;
    pressUntil = now + 1200;
    if (page == SYSTEM) fetchStormglass();
    if (page == LIGHTNING) updateLightningPage();
  }
  if (pressNotice && now > pressUntil) pressNotice = false;
}

void smallStatus() {
  smallDisplay.clearBuffer();
  smallDisplay.setFont(u8g2_font_4x6_tf);
  smallDisplay.drawStr(0, 6, "METROGNOME");
  smallDisplay.setCursor(0, 15); smallDisplay.print("WiFi "); smallDisplay.print(wifiStatus);
  smallDisplay.setCursor(0, 24); smallDisplay.print("API  "); smallDisplay.print(apiStatus);
  smallDisplay.setCursor(0, 33); smallDisplay.print("PAGE "); smallDisplay.print(pageNames[page]);
  if ((frame / 4) % 2 == 0) smallDisplay.drawBox(66, 0, 5, 5);
  else smallDisplay.drawFrame(66, 0, 5, 5);
  smallDisplay.sendBuffer();
}

void header(const char* title) {
  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.drawStr(0, 9, "METROGNOME");
  bigDisplay.setCursor(72, 9); bigDisplay.print(title);
  bigDisplay.drawHLine(0, 12, 128);
}

void drawWeather() {
  bigDisplay.clearBuffer(); header("WEATHER");
  bigDisplay.setFont(u8g2_font_7x14B_tf);
  bigDisplay.setCursor(0, 29);
  if (weather.valid) { bigDisplay.print(weather.airTemp, 1); bigDisplay.print(" C"); }
  else bigDisplay.print("--.- C");
  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.setCursor(72, 23); bigDisplay.print("WIND");
  bigDisplay.setCursor(72, 34);
  if (weather.valid) { bigDisplay.print(weather.windSpeed, 1); bigDisplay.print("m/s"); }
  else bigDisplay.print("--");
  bigDisplay.drawFrame(0, 36, 128, 28);
  bigDisplay.setCursor(4, 48); bigDisplay.print("Pressure "); if (weather.valid) bigDisplay.print(weather.pressure, 0); else bigDisplay.print("--"); bigDisplay.print(" hPa");
  bigDisplay.setCursor(4, 60); bigDisplay.print("Hum "); if (weather.valid) bigDisplay.print(weather.humidity, 0); else bigDisplay.print("--"); bigDisplay.print("% Upd "); bigDisplay.print(lastUpdate);
  bigDisplay.sendBuffer();
}

void drawSea() {
  bigDisplay.clearBuffer(); header("SEA");
  bigDisplay.setFont(u8g2_font_7x14B_tf);
  bigDisplay.setCursor(0, 29);
  if (weather.valid) { bigDisplay.print(weather.waveHeight, 1); bigDisplay.print(" m"); }
  else bigDisplay.print("--.- m");
  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.setCursor(72, 23); bigDisplay.print("SWELL");
  bigDisplay.setCursor(72, 34);
  if (weather.valid) { bigDisplay.print(weather.swellPeriod, 1); bigDisplay.print("s"); }
  else bigDisplay.print("--");
  bigDisplay.drawFrame(0, 38, 128, 24);
  bigDisplay.setCursor(4, 49); bigDisplay.print("Water "); if (weather.valid) bigDisplay.print(weather.waterTemp, 1); else bigDisplay.print("--.-"); bigDisplay.print(" C");
  bigDisplay.setCursor(4, 60); bigDisplay.print(weather.valid ? "Stormglass live" : "Press SYSTEM update");
  bigDisplay.sendBuffer();
}

void drawTide() {
  bigDisplay.clearBuffer(); header("TIDE");
  bigDisplay.setFont(u8g2_font_6x12_tf);
  bigDisplay.drawStr(0, 28, "HIGH  --:--");
  bigDisplay.drawStr(0, 43, "LOW   --:--");
  bigDisplay.drawStr(0, 58, "TIDE API LATER");
  bigDisplay.drawFrame(82, 18, 38, 40);
  uint8_t h = 8 + ((frame / 2) % 28);
  bigDisplay.drawBox(88, 58 - h, 26, h);
  bigDisplay.sendBuffer();
}

void drawMoon() {
  bigDisplay.clearBuffer(); header("MOON");
  bigDisplay.setFont(u8g2_font_6x12_tf);
  bigDisplay.drawStr(0, 28, "Moon data later");
  bigDisplay.drawStr(0, 43, "NTP ready soon");
  bigDisplay.drawStr(0, 58, "Stars await");
  bigDisplay.drawCircle(104, 38, 18);
  bigDisplay.drawDisc(110, 38, 16);
  bigDisplay.sendBuffer();
}

void drawLightning() {
  bigDisplay.clearBuffer(); header("LIGHT");
  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.drawStr(0, 24, "Nearest strike");

  bigDisplay.setFont(u8g2_font_7x14B_tf);
  bigDisplay.setCursor(0, 41);
  if (lightning.active) {
    bigDisplay.print(lightning.direction); bigDisplay.print(" "); bigDisplay.print(lightning.nearestKm, 0); bigDisplay.print("km");
  } else {
    bigDisplay.print(lightning.status);
  }

  bigDisplay.setFont(u8g2_font_5x8_tf);
  bigDisplay.setCursor(0, 53); bigDisplay.print("AZ "); bigDisplay.print(lightning.azimuthDeg, 0); bigDisplay.print(" FRONT "); bigDisplay.print(lightning.headingDeg, 0);
  bigDisplay.setCursor(0, 62); bigDisplay.print("REL "); bigDisplay.print(lightning.relativeDeg, 0); bigDisplay.print(" SERVO "); bigDisplay.print(lightning.servoAngle); bigDisplay.print(" "); bigDisplay.print(lightning.lastUpdate);

  int cx = 104;
  int cy = 38;
  bigDisplay.drawCircle(cx, cy, 21);
  bigDisplay.drawCircle(cx, cy, 12);
  bigDisplay.drawLine(cx, 17, cx, 59);
  bigDisplay.drawLine(83, cy, 125, cy);

  float a = (lightning.relativeDeg - 90.0) * PI / 180.0;
  int px = cx + (int)(cos(a) * 19.0);
  int py = cy + (int)(sin(a) * 19.0);
  bigDisplay.drawLine(cx, cy, px, py);
  bigDisplay.drawDisc(px, py, 2);

  if (!lightning.active) bigDisplay.drawCircle(cx, cy, ((frame * 3) % 18) + 2);
  bigDisplay.sendBuffer();
}

void drawSystem() {
  bigDisplay.clearBuffer(); header("SYSTEM");
  bigDisplay.setFont(u8g2_font_6x10_tf);
  bigDisplay.setCursor(0, 24); bigDisplay.print("WiFi "); bigDisplay.print(wifiStatus);
  bigDisplay.setCursor(0, 36); bigDisplay.print("API "); bigDisplay.print(apiStatus); bigDisplay.print(" "); bigDisplay.print(callsToday); bigDisplay.print("/"); bigDisplay.print(DAILY_API_LIMIT);
  bigDisplay.setCursor(0, 48); bigDisplay.print("Last "); bigDisplay.print(lastUpdate);
  bigDisplay.setCursor(0, 60); bigDisplay.print("PRESS: WEATHER UPDATE");
  bigDisplay.sendBuffer();
}

void drawBig() {
  switch (page) {
    case WEATHER: drawWeather(); break;
    case SEA: drawSea(); break;
    case TIDE: drawTide(); break;
    case MOON: drawMoon(); break;
    case LIGHTNING: drawLightning(); break;
    case SYSTEM: drawSystem(); break;
    default: drawWeather(); break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Metrognome Production Manual-North build");

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

  setupLightningVane();
  smallStatus();
  drawBig();

  connectBestWiFi();
  syncTime();
  loadApiCounter();
}

void loop() {
  checkEncoder();

  uint32_t now = millis();
  if (now - lastWifiCheck >= 10000) {
    lastWifiCheck = now;
    updateWifiStatus();
  }

  if (now - lastDraw >= 120) {
    lastDraw = now;
    frame++;
    smallStatus();
    drawBig();
  }
}
