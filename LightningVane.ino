// LightningVane.ino
// Metrognome LIGHT page: Home Assistant Blitzortung + QMC5883/HMC5883-style compass + SG90 servo.
//
// Wiring used for the ESP32-C3 Super Mini build:
//   Compass VCC -> 3V3
//   Compass GND -> GND
//   Compass SDA -> GPIO5
//   Compass SCL -> GPIO6
//   Compass DRDY left unconnected
//   Servo signal -> GPIO3
//   Servo VCC -> 5V rail
//   Servo GND -> common GND

#include <ESP32Servo.h>
#include <QMC5883LCompass.h>

#ifndef HA_BASE_URL
#define HA_BASE_URL "http://homeassistant.local:8123"
#endif

#ifndef HA_TOKEN
#define HA_TOKEN "PASTE_HOME_ASSISTANT_LONG_LIVED_ACCESS_TOKEN_HERE"
#endif

#ifndef HA_ENTITY_DISTANCE
#define HA_ENTITY_DISTANCE "sensor.blitzortung_lightning_distance"
#endif

#ifndef HA_ENTITY_AZIMUTH
#define HA_ENTITY_AZIMUTH "sensor.blitzortung_lightning_azimuth"
#endif

#ifndef HA_ENTITY_COUNTER
#define HA_ENTITY_COUNTER "sensor.blitzortung_lightning_counter"
#endif

constexpr uint8_t PIN_SERVO_SIGNAL = 3;

// SG90s vary. These are deliberately conservative so the little dish doesn't try to escape.
constexpr int SERVO_LEFT_LIMIT = 10;
constexpr int SERVO_CENTER = 90;
constexpr int SERVO_RIGHT_LIMIT = 170;

// Adjust after test fitting.
constexpr bool INVERT_SERVO = false;
constexpr float DECLINATION_DEGREES = 0.0;     // UK is close enough to zero for gnome purposes.
constexpr float COMPASS_MOUNT_OFFSET = 0.0;   // Use this if the chip is mounted rotated in the case.

Servo lightningServo;
QMC5883LCompass lightningCompass;

float normalize360(float angle){
  while(angle < 0.0) angle += 360.0;
  while(angle >= 360.0) angle -= 360.0;
  return angle;
}

float signedRelativeAngle(float targetBearing, float currentHeading){
  float rel = normalize360(targetBearing - currentHeading);
  if(rel > 180.0) rel -= 360.0;
  return rel;
}

String bearingToCardinal(float bearing){
  const char* dirs[] = {"N","NE","E","SE","S","SW","W","NW"};
  int index = (int)((normalize360(bearing) + 22.5) / 45.0) % 8;
  return String(dirs[index]);
}

int relativeToServo(float relativeAngle){
  // The physical SG90 can only indicate a front-facing 180 degree arc.
  // Strikes behind the device are clamped left/right rather than spinning madly.
  relativeAngle = constrain(relativeAngle, -90.0, 90.0);

  int angle = SERVO_CENTER + (int)relativeAngle;
  if(INVERT_SERVO) angle = SERVO_CENTER - (int)relativeAngle;

  return constrain(angle, SERVO_LEFT_LIMIT, SERVO_RIGHT_LIMIT);
}

float readCompassHeading(){
  lightningCompass.read();

  int x = lightningCompass.getX();
  int y = lightningCompass.getY();

  float heading = atan2((float)y, (float)x) * 180.0 / PI;
  heading += DECLINATION_DEGREES;
  heading += COMPASS_MOUNT_OFFSET;

  return normalize360(heading);
}

String haStateUrl(const char* entityId){
  return String(HA_BASE_URL) + "/api/states/" + entityId;
}

bool getHAStateFloat(const char* entityId, float &valueOut){
  if(WiFi.status() != WL_CONNECTED){
    lightning.status = "WIFI";
    return false;
  }

  HTTPClient http;
  String url = haStateUrl(entityId);

  http.setTimeout(8000);
  http.begin(url);
  http.addHeader("Authorization", String("Bearer ") + HA_TOKEN);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");

  int code = http.GET();
  if(code != 200){
    Serial.print("HA GET failed for ");
    Serial.print(entityId);
    Serial.print(" HTTP ");
    Serial.println(code);
    http.end();
    lightning.status = "HA ERR";
    return false;
  }

  String payload = http.getString();
  http.end();

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if(err){
    Serial.print("HA JSON error: ");
    Serial.println(err.c_str());
    lightning.status = "JSON";
    return false;
  }

  const char* state = doc["state"];
  if(!state){
    lightning.status = "NO STATE";
    return false;
  }

  String s = String(state);
  if(s == "unknown" || s == "unavailable" || s.length() == 0){
    lightning.status = "NO DATA";
    return false;
  }

  valueOut = s.toFloat();
  return true;
}

bool fetchLightningFromHomeAssistant(){
  if(!wifiOk){
    apiStatus = "WIFI";
    if(!connectBestWiFi()){
      lightning.status = "WIFI FAIL";
      return false;
    }
  }

  lightning.status = "CALL";
  smallStatus();
  drawBig();

  float distanceKm = 0.0;
  float azimuthDeg = 0.0;
  float counter = 0.0;

  bool gotDistance = getHAStateFloat(HA_ENTITY_DISTANCE, distanceKm);
  bool gotAzimuth = getHAStateFloat(HA_ENTITY_AZIMUTH, azimuthDeg);
  bool gotCounter = getHAStateFloat(HA_ENTITY_COUNTER, counter);

  if(!gotDistance || !gotAzimuth){
    if(lightning.status == "CALL") lightning.status = "NO STRIKE";
    lightning.active = false;
    apiStatus = "LIGHT ERR";
    return false;
  }

  lightning.active = true;
  lightning.nearestKm = distanceKm;
  lightning.azimuthDeg = normalize360(azimuthDeg);
  lightning.direction = bearingToCardinal(lightning.azimuthDeg);
  if(gotCounter) lightning.strikeCount = (int)counter;
  lightning.lastUpdate = utcTimeShort();
  lightning.status = "OK";
  apiStatus = "LIGHT OK";

  return true;
}

void setupLightningVane(){
  // Wire has already been started in Metrognome.ino because the small OLED is also I2C.
  lightningCompass.init();

  lightningServo.setPeriodHertz(50);
  lightningServo.attach(PIN_SERVO_SIGNAL, 500, 2400);
  lightningServo.write(SERVO_CENTER);

  lightning.status = "READY";
  lightning.servoAngle = SERVO_CENTER;

  Serial.println("Lightning vane ready.");
}

void updateLightningPage(){
  bool gotStrike = fetchLightningFromHomeAssistant();

  // Read compass before moving the servo, so the motor doesn't bully the magnetometer.
  float heading = readCompassHeading();
  lightning.headingDeg = heading;

  if(!gotStrike){
    lightning.relativeDeg = 0.0;
    lightning.servoAngle = SERVO_CENTER;
    lightningServo.write(SERVO_CENTER);
    return;
  }

  float relative = signedRelativeAngle(lightning.azimuthDeg, heading);
  int servoAngle = relativeToServo(relative);

  lightning.relativeDeg = relative;
  lightning.servoAngle = servoAngle;

  lightningServo.write(servoAngle);

  Serial.println("---- LIGHTNING UPDATE ----");
  Serial.print("Distance km: "); Serial.println(lightning.nearestKm);
  Serial.print("Strike azimuth: "); Serial.println(lightning.azimuthDeg);
  Serial.print("Compass heading: "); Serial.println(lightning.headingDeg);
  Serial.print("Relative angle: "); Serial.println(lightning.relativeDeg);
  Serial.print("Servo angle: "); Serial.println(lightning.servoAngle);
}
