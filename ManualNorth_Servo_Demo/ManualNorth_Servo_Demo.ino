// ManualNorth_Servo_Demo.ino
// Compass-free Metrognome lightning vane servo test.
// Use this after removing the dud HP5883/HMC5883/QMC5883 compass module.
//
// Hardware:
//   SG90 brown/black -> common GND
//   SG90 red         -> 5V rail
//   SG90 orange/yel  -> signal pin below
//
// Serial Monitor: 115200 baud.

#include <Arduino.h>
#include <ESP32Servo.h>

// Try GPIO3 first because that is how the Metrognome build was wired.
// If no movement, move servo signal to TX and set this to 21.
// If still no movement, try RX and set this to 20.
constexpr uint8_t PIN_SERVO_SIGNAL = 3;

constexpr int SERVO_LEFT_LIMIT = 10;
constexpr int SERVO_CENTER = 90;
constexpr int SERVO_RIGHT_LIMIT = 170;
constexpr bool INVERT_SERVO = false;

// Manual north mode:
// Put the gnome down using a tiny glued-on compass.
// If the gnome's front points north, leave this at 0.
// If the gnome's front points east, use 90. South 180. West 270.
constexpr float GNOME_FRONT_BEARING_DEG = 0.0;

Servo vaneServo;

float normalize360(float angle) {
  while (angle < 0.0) angle += 360.0;
  while (angle >= 360.0) angle -= 360.0;
  return angle;
}

float signedRelativeAngle(float targetBearing, float currentFrontBearing) {
  float rel = normalize360(targetBearing - currentFrontBearing);
  if (rel > 180.0) rel -= 360.0;
  return rel;
}

int relativeToServo(float relativeAngle) {
  relativeAngle = constrain(relativeAngle, -90.0, 90.0);
  int angle = SERVO_CENTER + (int)relativeAngle;
  if (INVERT_SERVO) angle = SERVO_CENTER - (int)relativeAngle;
  return constrain(angle, SERVO_LEFT_LIMIT, SERVO_RIGHT_LIMIT);
}

void moveToBearing(float targetBearing) {
  float relative = signedRelativeAngle(targetBearing, GNOME_FRONT_BEARING_DEG);
  int servoAngle = relativeToServo(relative);

  Serial.println();
  Serial.println("---- MANUAL NORTH SERVO MOVE ----");
  Serial.print("Gnome front bearing: "); Serial.println(GNOME_FRONT_BEARING_DEG);
  Serial.print("Target bearing:      "); Serial.println(targetBearing);
  Serial.print("Relative angle:      "); Serial.println(relative);
  Serial.print("Servo angle:         "); Serial.println(servoAngle);

  vaneServo.write(servoAngle);
}

void startupSweep() {
  Serial.println("Startup sweep: 10 -> 170 -> 90");
  vaneServo.write(10);
  delay(900);
  vaneServo.write(170);
  delay(900);
  vaneServo.write(90);
  delay(900);
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("ManualNorth_Servo_Demo alive");
  Serial.println("No compass. No I2C. No blue goblin.");

  vaneServo.setPeriodHertz(50);
  vaneServo.attach(PIN_SERVO_SIGNAL, 500, 2400);

  startupSweep();
}

void loop() {
  // Demo bearings: north, east, south, west, northwest.
  // This lets us test whether the vane moves sensibly without any compass dependency.
  moveToBearing(0.0);
  delay(1500);

  moveToBearing(90.0);
  delay(1500);

  moveToBearing(180.0);
  delay(1500);

  moveToBearing(270.0);
  delay(1500);

  moveToBearing(315.0);
  delay(2500);
}
