/*
 * ═══════════════════════════════════════════════════════════════
 *  CognoSpace Height Measurement System (HMS)
 *  ESP32 Serial Firmware v2.0 (converted from BLE)
 * 
 *  Hardware:
 *   - ESP32 (any variant)
 *   - HC-SR04 Ultrasonic Sensor (mounted on ceiling/top fixture)
 *   - 20×4 I2C LCD Display (address 0x27)
 *   - Active Buzzer
 *   (No physical button - measurement is triggered only from the web UI)
 * 
 *  Communication: USB/Serial cable, 115200 baud
 *  Commands from PC/App over Serial (each line ends with '\n'):
 *    NAME:PersonName,Gender
 *    MEASURE
 *  Data sent from ESP32 over Serial:
 *    HEIGHT:123.4       (cm)
 *    ERROR
 * ═══════════════════════════════════════════════════════════════
 */

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ─── PIN DEFINITIONS ───────────────────────────────────────────
#define TRIG_PIN     5      // HC-SR04 Trigger
#define ECHO_PIN     18     // HC-SR04 Echo
#define BUZZER_PIN   19     // Active Buzzer
#define SDA_PIN      21     // I2C SDA
#define SCL_PIN      22     // I2C SCL

// ─── PHYSICAL SETUP ────────────────────────────────────────────
// The sensor is mounted at ANY height pointing downward.
// No need to hard-code height — the system auto-calibrates each session:
//   Phase 1: sensor reads floor distance (area must be CLEAR)  → baseline
//   Phase 2: person walks under → sensor reads top-of-head distance
//   Height = baseline − person_distance
#define NUM_SAMPLES         7      // Readings averaged per measurement
#define SAMPLE_DELAY_MS    60      // Delay between samples
#define MAX_VALID_DIST_CM  400.0   // Ignore readings above this
#define MIN_VALID_DIST_CM   10.0   // Ignore readings below this (noise)
#define CALIBRATION_SAMPLES 10     // More samples for baseline (accuracy)
#define CALIBRATION_DELAY_MS 80

// ─── LCD ───────────────────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 20, 4);

// ─── STATE MACHINE ─────────────────────────────────────────────
enum State {
  STATE_IDLE,
  STATE_WELCOME,
  STATE_READY,
  STATE_MEASURING,
  STATE_RESULT
};
State currentState = STATE_IDLE;

// ─── GLOBALS ───────────────────────────────────────────────────
String personName   = "";
String personGender = "";
float  lastHeightCM       = 0;
float  baselineDistanceCM = -1;   // floor distance captured during calibration
unsigned long buzzerEndTime  = 0;
unsigned long stateEnteredAt = 0;
bool   buzzerOn      = false;
bool   measureNow    = false;   // set by Serial MEASURE command from the web UI

// Sub-phases inside STATE_MEASURING
enum MeasurePhase { PHASE_CALIBRATE, PHASE_WAIT_PERSON, PHASE_MEASURE_PERSON };
MeasurePhase measurePhase = PHASE_CALIBRATE;

String serialBuffer  = "";     // accumulates incoming serial chars until newline

// ═══════════════════════════════════════════════════════════════
//  SERIAL COMMAND HANDLING (replaces BLE command characteristic)
// ═══════════════════════════════════════════════════════════════
void handleSerialCommand(String raw) {
  raw.trim();
  if (raw.length() == 0) return;

  Serial.print("[CMD] Received: "); Serial.println(raw);

  if (raw.startsWith("NAME:")) {
    // Format: NAME:PersonName,Gender
    raw.remove(0, 5);                      // strip "NAME:"
    int comma = raw.indexOf(',');
    if (comma > 0) {
      personName   = raw.substring(0, comma);
      personGender = raw.substring(comma + 1);
    } else {
      personName = raw;
    }
    currentState = STATE_WELCOME;
    stateEnteredAt = millis();
  }
  else if (raw == "MEASURE") {
    if (currentState == STATE_READY || currentState == STATE_WELCOME) {
      measureNow = true;
      currentState = STATE_MEASURING;
      stateEnteredAt = millis();
    }
  }
}

// Call this every loop() to read Serial line-by-line without blocking
void pollSerialCommands() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n') {
      handleSerialCommand(serialBuffer);
      serialBuffer = "";
    } else if (c != '\r') {
      serialBuffer += c;
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  LCD HELPERS
// ═══════════════════════════════════════════════════════════════
void lcdClear() {
  lcd.clear();
}

// Print a string left-padded to 20 chars on a given row
void lcdPrint(uint8_t row, String text, bool center = false) {
  lcd.setCursor(0, row);
  if (center) {
    int pad = (20 - text.length()) / 2;
    for (int i = 0; i < pad; i++) lcd.print(' ');
  }
  lcd.print(text.substring(0, min((int)text.length(), 20)));
  // clear remainder
  int start = center ? ((20 - text.length()) / 2) + text.length() : text.length();
  for (int i = start; i < 20; i++) lcd.print(' ');
}

void showLCDIdle() {
  lcdClear();
  lcdPrint(0, "  CognoSpace HMS   ", true);
  lcdPrint(1, "  Height Measure   ", true);
  lcdPrint(2, "  System v2.0      ", true);
  lcdPrint(3, "  Serial Ready     ", true);
}

void showLCDWelcome() {
  String shortName = personName.length() > 14 ? personName.substring(0, 14) : personName;
  lcdClear();
  lcdPrint(0, "    Welcome!       ", true);
  lcdPrint(1, "Hi, " + shortName, false);
  lcdPrint(2, "Gender: " + personGender, false);
  lcdPrint(3, "Please stand ready ", false);
}

void showLCDReady() {
  lcdClear();
  lcdPrint(0, "  Stand under the  ", true);
  lcdPrint(1, "    sensor now!    ", true);
  lcdPrint(2, "Click Start        ", false);
  lcdPrint(3, "Measuring on app   ", false);
}

void showLCDCalibrating() {
  lcdClear();
  lcdPrint(0, " Wait, Calculating ", true);
  lcdPrint(1, " Floor distance... ", true);
  lcdPrint(2, " Keep area clear!  ", true);
  lcdPrint(3, " Please wait...    ", true);
}

void showLCDComeForward() {
  lcdClear();
  lcdPrint(0, "  ** Come Forward**", true);
  lcdPrint(1, "  Stand under      ", true);
  lcdPrint(2, "  the sensor now!  ", true);
  lcdPrint(3, "  Stand still...   ", true);
}

void showLCDMeasuring() {
  lcdClear();
  lcdPrint(0, "  ** MEASURING **  ", true);
  lcdPrint(1, "  Please stand     ", true);
  lcdPrint(2, "  still & upright  ", true);
  lcdPrint(3, "  Do not move...   ", true);
}

void showLCDResult(float cm, float inch) {
  lcdClear();
  lcdPrint(0, " Height Result :)  ", true);
  lcdPrint(1, "  CM  : " + String(cm, 1) + " cm", false);
  lcdPrint(2, "  IN  : " + String(inch, 1) + " in", false);
  lcdPrint(3, "  ** COMPLETE! **  ", true);
}

// ═══════════════════════════════════════════════════════════════
//  ULTRASONIC MEASUREMENT
// ═══════════════════════════════════════════════════════════════
float readRawDistanceCM() {
  // Send 10µs pulse
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000); // 30ms timeout
  if (duration == 0) return -1.0;
  return (duration * 0.0343) / 2.0;
}

// Phase 1: Measure floor/baseline distance (area must be CLEAR)
// Returns baseline in cm; -1 if failed
float calibrateBaseline() {
  float readings[CALIBRATION_SAMPLES];
  int validCount = 0;

  for (int i = 0; i < CALIBRATION_SAMPLES; i++) {
    float d = readRawDistanceCM();
    if (d >= MIN_VALID_DIST_CM && d <= MAX_VALID_DIST_CM) {
      readings[validCount++] = d;
    }
    delay(CALIBRATION_DELAY_MS);
  }

  if (validCount < 5) return -1.0;

  // Sort and average the middle values
  for (int i = 0; i < validCount - 1; i++)
    for (int j = i + 1; j < validCount; j++)
      if (readings[i] > readings[j]) { float t = readings[i]; readings[i] = readings[j]; readings[j] = t; }

  int midStart = (validCount - 5) / 2;
  float sum = 0;
  for (int k = midStart; k < midStart + 5; k++) sum += readings[k];
  float baseline = sum / 5.0;

  Serial.print("[CALIB] Baseline (sensor→floor): ");
  Serial.print(baseline, 1);
  Serial.println(" cm");

  if (baseline < 30 || baseline > 400) return -1.0; // sanity
  return baseline;
}

// Phase 2: Measure person height using saved baseline
// Returns height in cm; -1 if failed
float measurePersonHeight() {
  if (baselineDistanceCM < 0) return -1.0;

  float readings[NUM_SAMPLES];
  int validCount = 0;

  for (int i = 0; i < NUM_SAMPLES; i++) {
    float d = readRawDistanceCM();
    if (d >= MIN_VALID_DIST_CM && d <= (baselineDistanceCM - 10)) {
      readings[validCount++] = d;
    }
    delay(SAMPLE_DELAY_MS);
  }

  if (validCount < 3) return -1.0;

  // Sort and take median 3
  for (int i = 0; i < validCount - 1; i++)
    for (int j = i + 1; j < validCount; j++)
      if (readings[i] > readings[j]) { float t = readings[i]; readings[i] = readings[j]; readings[j] = t; }

  int start = (validCount - 3) / 2;
  float avg = (readings[start] + readings[start + 1] + readings[start + 2]) / 3.0;

  // Height = baseline (floor distance) - person-top distance
  float height = baselineDistanceCM - avg;
  if (height < 50 || height > 250) return -1.0; // Sanity: 50–250 cm
  return height;
}

// ═══════════════════════════════════════════════════════════════
//  BUZZER
// ═══════════════════════════════════════════════════════════════
void startBuzzer(uint32_t durationMs) {
  digitalWrite(BUZZER_PIN, HIGH);
  buzzerOn = true;
  buzzerEndTime = millis() + durationMs;
}

void updateBuzzer() {
  if (buzzerOn && millis() >= buzzerEndTime) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerOn = false;
  }
}

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("\n[HMS] CognoSpace Height Measurement System v2.0 (Serial mode)");

  // Pin modes
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // LCD init
  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcdPrint(0, "  CognoSpace HMS   ", true);
  lcdPrint(1, "   Initializing... ", true);
  lcdPrint(2, "   Please wait...  ", true);
  delay(1500);

  Serial.println("[Serial] Ready. Send NAME:Name,Gender then MEASURE");

  showLCDIdle();
  currentState = STATE_IDLE;
}

// ═══════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
  updateBuzzer();
  pollSerialCommands();   // replaces BLE onWrite callback

  // ─── STATE MACHINE ─────────────────────────────────────────
  switch (currentState) {

    case STATE_IDLE:
      // Waiting for NAME command from PC/app over Serial
      break;

    case STATE_WELCOME:
      showLCDWelcome();
      delay(2500);
      currentState = STATE_READY;
      stateEnteredAt = millis();
      showLCDReady();
      break;

    case STATE_READY:
      // LCD already showing ready, waiting for Serial MEASURE cmd from web UI
      break;

    case STATE_MEASURING: {
      if (measureNow) {
        measureNow = false;
        showLCDMeasuring();
        delay(500); // small pause for person to stop swaying

        float heightCM = measurePersonHeight();

        if (heightCM < 0) {
          // Measurement failed
          lcdClear();
          lcdPrint(0, "  Measurement      ", true);
          lcdPrint(1, "  ERROR!           ", true);
          lcdPrint(2, "  Ensure person is ", true);
          lcdPrint(3, "  under sensor     ", true);
          // Notify error over Serial
          Serial.println("ERROR");
          delay(3000);
          currentState = STATE_READY;
          showLCDReady();
        } else {
          lastHeightCM = heightCM;
          float heightIN = heightCM / 2.54;

          // Show result on LCD
          showLCDResult(heightCM, heightIN);

          // Send via Serial - plain number only (webpage does parseFloat on each line)
          Serial.println(String(heightCM, 1));

          // Buzzer for 4 seconds
          startBuzzer(4000);

          currentState = STATE_RESULT;
          stateEnteredAt = millis();
        }
      }
      break;
    }

    case STATE_RESULT:
      // Stay on result screen for 8 seconds, then return to idle
      if (millis() - stateEnteredAt > 8000) {
        personName   = "";
        personGender = "";
        currentState = STATE_IDLE;
        showLCDIdle();
      }
      break;
  }

  delay(20);
}

/*
 * ═══════════════════════════════════════════════════════════════
 *  WIRING GUIDE
 * ═══════════════════════════════════════════════════════════════
 *
 *  HC-SR04 Ultrasonic Sensor (mounted on ceiling/top beam)
 *  ┌──────┬──────────┐
 *  │ VCC  │  3.3V    │
 *  │ GND  │  GND     │
 *  │ TRIG │  GPIO 5  │
 *  │ ECHO │  GPIO 18 │  ← Use a 1kΩ + 2kΩ voltage divider if using 5V HC-SR04
 *  └──────┴──────────┘
 *
 *  20×4 I2C LCD Display
 *  ┌──────┬──────────┐
 *  │ VCC  │  5V      │
 *  │ GND  │  GND     │
 *  │ SDA  │  GPIO 21 │
 *  │ SCL  │  GPIO 22 │
 *  └──────┴──────────┘
 *  Note: I2C address is 0x27 (change in code if yours is 0x3F)
 *
 *  Active Buzzer
 *  ┌──────┬──────────┐
 *  │ +    │  GPIO 19 │  (with 100Ω series resistor)
 *  │ -    │  GND     │
 *  └──────┴──────────┘
 *
 *  (No physical button needed - "Start Measuring" in the web UI
 *   sends the MEASURE command over serial to trigger a reading)
 *
 * ═══════════════════════════════════════════════════════════════
 *  SERIAL PROTOCOL (replaces BLE)
 * ═══════════════════════════════════════════════════════════════
 *  Connect ESP32 via USB cable. Open Serial Monitor / your PC app
 *  at 115200 baud, line ending = Newline ("\n").
 *
 *  Commands you send TO the ESP32 (one per line):
 *    NAME:John,Male        -> sets name/gender, shows welcome screen
 *    MEASURE                -> triggers a measurement
 *
 *  Data the ESP32 sends back:
 *    [CMD] Received: ...    -> debug echo of your command
 *    HEIGHT:123.4           -> measured height in cm
 *    ERROR                  -> measurement failed, try again
 *    [HMS] Height: 123.4 cm (48.6 in)  -> human-readable log line
 *
 *  On your PC side (Python example):
 *    import serial
 *    ser = serial.Serial('COM5', 115200, timeout=2)  # adjust port
 *    ser.write(b"NAME:John,Male\n")
 *    ser.write(b"MEASURE\n")
 *    line = ser.readline().decode().strip()
 *
 * ═══════════════════════════════════════════════════════════════
 *  CALIBRATION
 * ═══════════════════════════════════════════════════════════════
 *  1. Mount sensor firmly on ceiling or overhead fixture
 *  2. Measure exact vertical distance from floor to sensor face
 *  3. Set CEILING_HEIGHT_CM to that value (e.g., 240.0 for 2.4m room)
 *  4. Upload firmware and open Serial Monitor at 115200 baud
 *  5. Place a known-height object (e.g., 170cm stick) under sensor
 *  6. Verify reading matches; adjust CEILING_HEIGHT_CM if needed
 *
 * ═══════════════════════════════════════════════════════════════
 *  REQUIRED LIBRARIES (install via Arduino Library Manager)
 * ═══════════════════════════════════════════════════════════════
 *  - LiquidCrystal I2C  (by Frank de Brabander)
 *  - No BLE libraries needed anymore (Serial is built-in)
 *
 *  Arduino IDE Board: "ESP32 Dev Module"
 *  Upload Speed: 921600
 *  Flash Frequency: 80MHz
 * ═══════════════════════════════════════════════════════════════
 */
