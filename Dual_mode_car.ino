#include <AFMotor.h>
#include <Servo.h>

// --- Speed & sensors ---
int speedBase = 150;           // base speed (updated by "Vxxx")
const uint8_t DIAG_PCT  = 1;   // inner wheel percentage for curves (0 = strongest)
const uint8_t INNER_MIN = 15;  // tiny minimum inner speed to avoid spikes (8–15 is good)

#define Trig A0
#define Echo A1
#define ServoCenter 90

bool hasStarted = false;
int prevDistance = 0;
int sameDistanceCount = 0;
const int SAME_DISTANCE_LIMIT = 5;
const int DISTANCE_TOLERANCE = 2;

const int LED_MODE = 13; // ON = manual mode

int distance;
int leftDistance;
int rightDistance;

Servo servo;
AF_DCMotor M1(1);
AF_DCMotor M2(2);
AF_DCMotor M3(3);
AF_DCMotor M4(4);

int mode = 0;  // 0 = automatic, 1 = manual

String rxBuffer = "";     // accumulate serial line

unsigned long lastCommandTime = 0;        // timestamp of last manual command
const unsigned long MANUAL_TIMEOUT = 100; // ms to stop if no input

// ---------- Helpers for curves ----------
inline int pct(int v, int p) { return (long)v * p / 100L; }

// Drive left and right sides with signed speeds (-255..255)
// Left side = M1,M2 ; Right side = M3,M4
void driveLR(int left, int right) {
  int ls = constrain(abs(left), 0, 255);
  int rs = constrain(abs(right), 0, 255);

  M1.setSpeed(ls); M2.setSpeed(ls);   // left
  M3.setSpeed(rs); M4.setSpeed(rs);   // right

  // Keep direction set even at speed 0 (avoid RELEASE to reduce spikes)
  if (left >= 0) { M1.run(FORWARD);  M2.run(FORWARD); }
  else           { M1.run(BACKWARD); M2.run(BACKWARD); }

  if (right >= 0) { M3.run(FORWARD); M4.run(FORWARD); }
  else            { M3.run(BACKWARD); M4.run(BACKWARD); }
}

void setup() {
  pinMode(Trig, OUTPUT);
  pinMode(Echo, INPUT);
  pinMode(LED_MODE, OUTPUT);

  servo.attach(10);            // attached at boot
  setMotorSpeed(speedBase);    // initialize base speed
  centerServo();
  delay(2000);

  Serial.begin(9600); // HC-05 on pins 0 & 1
  Serial.println("READY");
}

void loop() {
  readSerialCommands();

  digitalWrite(LED_MODE, mode ? HIGH : LOW);

  if (!hasStarted) {
    stopMotors();
    return;
  }

  if (mode == 0) {
    automaticMode();
  } else {
    manualMode();
  }
}

/* ---------- Serial command handling ---------- */
void readSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      rxBuffer.trim();
      if (rxBuffer.length() > 0) processCommand(rxBuffer);
      rxBuffer = "";
    } else {
      rxBuffer += c;
      if (rxBuffer.length() > 32) rxBuffer = rxBuffer.substring(rxBuffer.length() - 32);
    }
  }
}

/* ---------- Command processing ---------- */
void processCommand(const String &line) {
  String cmd = line;
  cmd.trim();
  cmd.toUpperCase();

  // Mode switching
  if (cmd == "A") {
    mode = 0;
    hasStarted = true;
    stopMotors();
    if (!servo.attached()) servo.attach(10);  // re-attach for scanning in Auto
    centerServo();
    Serial.println("MODE:A");
    return;
  }
  if (cmd == "M") {
    mode = 1;
    hasStarted = true;
    stopMotors();
    if (servo.attached()) servo.detach();     // detach in Manual to avoid jitter
    Serial.println("MODE:M");
    return;
  }

  // Speed control
  if (cmd.startsWith("V")) {
    int spd = cmd.substring(1).toInt();
    setMotorSpeed(spd);
    Serial.print("SPEED:");
    Serial.println(spd);
    return;
  }

  if (hasStarted && mode == 1) { // Manual movement
    lastCommandTime = millis(); // update last command timestamp

    if (cmd == "F") { forward(); Serial.println("CMD:F"); }
    else if (cmd == "B") { backward(); Serial.println("CMD:B"); }
    else if (cmd == "L") { turnLeft(); Serial.println("CMD:L"); }
    else if (cmd == "R") { turnRight(); Serial.println("CMD:R"); }

    // Diagonals now curve instead of pivot
    else if (cmd == "FR") { forwardRight();  Serial.println("CMD:FR"); }
    else if (cmd == "FL") { forwardLeft();   Serial.println("CMD:FL"); }
    else if (cmd == "BR") { backwardRight(); Serial.println("CMD:BR"); }
    else if (cmd == "BL") { backwardLeft();  Serial.println("CMD:BL"); }

    else if (cmd == "S") { stopMotors(); Serial.println("CMD:S"); }
    else { stopMotors(); Serial.println("IGNORED"); }
  } else {
    Serial.println("IGNORED");
  }
}

/* ---------- Motor & speed control ---------- */
void setMotorSpeed(int speed) {
  // Only store the base; driveLR() sets per-side speeds each command tick
  speedBase = constrain(speed, 0, 255);
}

/* ---------- Automatic & manual behavior ---------- */
void automaticMode() {
  // ensure servo is available in Auto (in case app switched modes while running)
  if (!servo.attached()) servo.attach(10);

  distance = readUltrasonic();

  if (abs(distance - prevDistance) <= DISTANCE_TOLERANCE && distance <= 20) {
    sameDistanceCount++;
  } else {
    sameDistanceCount = 0;
  }
  prevDistance = distance;

  if (sameDistanceCount >= SAME_DISTANCE_LIMIT) {
    stopMotors();
    backward();
    delay(150);
    stopMotors();
    delay(100);

    leftDistance = scanLeft();
    rightDistance = scanRight();

    centerServo();
    delay(100);

    if (leftDistance > rightDistance) turnLeft();
    else turnRight();

    delay(500);
    stopMotors();
    delay(200);

    sameDistanceCount = 0;
  }
  else if (distance <= 12) {
    stopMotors();
    backward();
    delay(150);
    stopMotors();
    delay(100);

    leftDistance = scanLeft();
    rightDistance = scanRight();

    centerServo();
    delay(100);

    if (leftDistance > rightDistance) turnLeft();
    else turnRight();

    delay(500);
    stopMotors();
    delay(200);
  }
  else {
    forward();
  }
}

void manualMode() {
  if (millis() - lastCommandTime > MANUAL_TIMEOUT) stopMotors();
}

/* ---------- Motor helpers ---------- */
void forward()        { driveLR(  speedBase,  speedBase); }
void backward()       { driveLR( -speedBase, -speedBase); }
// Pivot turns (used by auto mode)
void turnLeft()       { driveLR( -speedBase,  speedBase); }
void turnRight()      { driveLR(  speedBase, -speedBase); }
void stopMotors()     { M1.run(RELEASE); M2.run(RELEASE); M3.run(RELEASE); M4.run(RELEASE); }

// Curved diagonals with inner-wheel minimum
void forwardRight()   { int inner = max(pct(speedBase, DIAG_PCT), (int)INNER_MIN); driveLR(  speedBase,  inner ); }
void forwardLeft()    { int inner = max(pct(speedBase, DIAG_PCT), (int)INNER_MIN); driveLR(  inner,      speedBase ); }
void backwardRight()  { int inner = max(pct(speedBase, DIAG_PCT), (int)INNER_MIN); driveLR( -speedBase, -inner ); }
void backwardLeft()   { int inner = max(pct(speedBase, DIAG_PCT), (int)INNER_MIN); driveLR( -inner,     -speedBase ); }

/* ---------- Servo helpers ---------- */
void centerServo() { if (servo.attached()) { servo.write(ServoCenter); delay(700); } }
int scanLeft()  { if (!servo.attached()) servo.attach(10); servo.write(30);  delay(700); return readUltrasonic(); }
int scanRight() { if (!servo.attached()) servo.attach(10); servo.write(150); delay(700); return readUltrasonic(); }

/* ---------- Ultrasonic ---------- */
int readUltrasonic() {
  digitalWrite(Trig, LOW); delayMicroseconds(4);
  digitalWrite(Trig, HIGH); delayMicroseconds(10);
  digitalWrite(Trig, LOW);
  long duration = pulseIn(Echo, HIGH, 30000);
  if (duration == 0) return 250;
  return duration / 29 / 2;
}