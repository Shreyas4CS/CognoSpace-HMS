/*
 * ═══════════════════════════════════════════════════════════════
 *  CognoSpace Height Measurement System (HMS)
 *  ESP32 Serial Firmware v2.0 (with Anti-Noise Debounce)
 * ═══════════════════════════════════════════════════════════════
 */

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ─── PIN DEFINITIONS ───────────────────────────────────────────
#define TRIG_PIN     5      
#define ECHO_PIN     18     
#define BUZZER_PIN   19     
#define SDA_PIN      21     
#define SCL_PIN      22     

// ─── PHYSICAL SETUP ────────────────────────────────────────────
#define NUM_SAMPLES         7      
#define SAMPLE_DELAY_MS    60      
#define MAX_VALID_DIST_CM  400.0   
#define MIN_VALID_DIST_CM   10.0   
#define CALIBRATION_SAMPLES 10     
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
float  baselineDistanceCM = -1;   
float  defaultDistanceCM  = -1;   
unsigned long buzzerEndTime  = 0;
unsigned long stateEnteredAt = 0;
bool   buzzerOn      = false;
bool   measureNow    = false;   

String serialBuffer  = "";     

// ═══════════════════════════════════════════════════════════════
//  SERIAL COMMAND HANDLING
// ═══════════════════════════════════════════════════════════════
void handleSerialCommand(String raw) {
  raw.trim();
  if (raw.length() == 0) return;

  Serial.print("[CMD] Received: "); Serial.println(raw);

  if (raw.startsWith("NAME:")) {
    raw.remove(0, 5);                      
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
  else if (raw.startsWith("SETDIST:")) {
    float d = raw.substring(8).toFloat();
    if (d >= 30.0 && d <= 500.0) {
      defaultDistanceCM = d;
      Serial.print("[HMS] Default distance set to: ");
      Serial.print(defaultDistanceCM, 1);
      Serial.println(" cm");
    }
  }
  else if (raw == "MEASURE") {
    if (currentState == STATE_READY || currentState == STATE_WELCOME) {
      measureNow = true;
      currentState = STATE_MEASURING;
      stateEnteredAt = millis();
    }
  }
}

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

void lcdPrint(uint8_t row, String text, bool center = false) {
  lcd.setCursor(0, row);
  if (center) {
    int pad = (20 - text.length()) / 2;
    for (int i = 0; i < pad; i++) lcd.print(' ');
  }
  lcd.print(text.substring(0, min((int)text.length(), 20)));
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
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000); 
  if (duration == 0) return -1.0;
  return (duration * 0.0343) / 2.0;
}

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

  if (baseline < 30 || baseline > 400) return -1.0; 
  return baseline;
}

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

  for (int i = 0; i < validCount - 1; i++)
    for (int j = i + 1; j < validCount; j++)
      if (readings[i] > readings[j]) { float t = readings[i]; readings[i] = readings[j]; readings[j] = t; }

  int start = (validCount - 3) / 2;
  float avg = (readings[start] + readings[start + 1] + readings[start + 2]) / 3.0;

  float height = baselineDistanceCM - avg;
  if (height < 50 || height > 250) return -1.0; 
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

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcdPrint(0, "  CognoSpace HMS   ", true);
  lcdPrint(1, "   Initializing... ", true);
  lcdPrint(2, "   Please wait...  ", true);
  delay(1500);

  showLCDIdle();
  currentState = STATE_IDLE;
}

// ═══════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
  updateBuzzer();
  pollSerialCommands();   

  switch (currentState) {

    case STATE_IDLE:
      break;

    case STATE_WELCOME:
      showLCDWelcome();
      delay(2500);
      currentState = STATE_READY;
      stateEnteredAt = millis();
      showLCDReady();
      break;

    case STATE_READY:
      break;

    case STATE_MEASURING: {
      if (measureNow) {
        measureNow = false;

        if (defaultDistanceCM > 0) {
          // ── MODE A: Admin has set a fixed default distance ─────────────
          showLCDComeForward();
          
          unsigned long waitStart = millis();
          bool personDetected = false;
          int detectCount = 0; // Tracks consecutive positive readings
          
          // Wait up to 15 seconds for a person
          while (millis() - waitStart < 15000) {
            float currentDist = readRawDistanceCM();
            
            // If distance drops >20cm, someone might be there
            if (currentDist > 0 && currentDist < (defaultDistanceCM - 20.0)) {
              detectCount++;
              if (detectCount >= 3) { // Require 3 solid hits in a row (debouncing)
                personDetected = true;
                break;
              }
            } else {
              detectCount = 0; // Reset if it was just a random spike
            }
            delay(100);
          }

          if (!personDetected) {
            lcdClear();
            lcdPrint(0, "  Timeout...       ", true);
            lcdPrint(1, "  No person found  ", true);
            Serial.println("ERROR");
            delay(3000);
            currentState = STATE_READY;
            showLCDReady();
            break; 
          }

          showLCDMeasuring();
          delay(1500); // give person time to stand still

          float readings[NUM_SAMPLES];
          int validCount = 0;
          for (int i = 0; i < NUM_SAMPLES; i++) {
            float d = readRawDistanceCM();
            if (d >= MIN_VALID_DIST_CM && d <= MAX_VALID_DIST_CM) {
              readings[validCount++] = d;
            }
            delay(SAMPLE_DELAY_MS);
          }

          if (validCount < 3) {
            lcdClear();
            lcdPrint(0, "  Measurement      ", true);
            lcdPrint(1, "  ERROR!           ", true);
            lcdPrint(2, "  Ensure person is ", true);
            lcdPrint(3, "  under sensor     ", true);
            Serial.println("ERROR");
            delay(3000);
            currentState = STATE_READY;
            showLCDReady();
          } else {
            for (int i = 0; i < validCount - 1; i++)
              for (int j = i + 1; j < validCount; j++)
                if (readings[i] > readings[j]) { float t = readings[i]; readings[i] = readings[j]; readings[j] = t; }
            int start = (validCount - 3) / 2;
            float rawDist = (readings[start] + readings[start+1] + readings[start+2]) / 3.0;

            float heightCM = defaultDistanceCM - rawDist;
            float heightIN = heightCM / 2.54;

            Serial.print("[HMS] Default dist: "); Serial.print(defaultDistanceCM, 1);
            Serial.print(" cm | Raw dist to head: "); Serial.print(rawDist, 1);
            Serial.print(" cm | Height: "); Serial.print(heightCM, 1); Serial.println(" cm");

            if (heightCM < 50 || heightCM > 260) {
              lcdClear();
              lcdPrint(0, "  Out of Range     ", true);
              lcdPrint(1, "  Check default    ", true);
              lcdPrint(2, "  distance setting ", true);
              lcdPrint(3, "  & try again      ", true);
              Serial.println("ERROR");
              delay(3000);
              currentState = STATE_READY;
              showLCDReady();
            } else {
              lastHeightCM = heightCM;
              showLCDResult(heightCM, heightIN);
              Serial.print("DIST:");
              Serial.println(String(rawDist, 1));
              startBuzzer(4000);
              currentState = STATE_RESULT;
              stateEnteredAt = millis();
            }
          }

        } else {
          // ── MODE B: No default set — original auto-calibration flow ───
          showLCDCalibrating();
          baselineDistanceCM = calibrateBaseline();

          if (baselineDistanceCM < 0) {
            lcdClear();
            lcdPrint(0, "  Calibration      ", true);
            lcdPrint(1, "  FAILED!          ", true);
            lcdPrint(2, "  Clear the area   ", true);
            lcdPrint(3, "  and try again    ", true);
            Serial.println("ERROR");
            delay(3000);
            currentState = STATE_READY;
            showLCDReady();
          } else {
            showLCDComeForward();
            
            unsigned long waitStart = millis();
            bool personDetected = false;
            int detectCount = 0; // Tracks consecutive positive readings
            
            // Wait up to 15 seconds for a person
            while (millis() - waitStart < 15000) {
              float currentDist = readRawDistanceCM();
              if (currentDist > 0 && currentDist < (baselineDistanceCM - 20.0)) {
                detectCount++;
                if (detectCount >= 3) { // Require 3 solid hits
                  personDetected = true;
                  break;
                }
              } else {
                detectCount = 0; // Reset if it was a noise spike
              }
              delay(100);
            }

            if (!personDetected) {
              lcdClear();
              lcdPrint(0, "  Timeout...       ", true);
              lcdPrint(1, "  No person found  ", true);
              Serial.println("ERROR");
              delay(3000);
              currentState = STATE_READY;
              showLCDReady();
              break; 
            }

            showLCDMeasuring();
            delay(1500); // give person time to stand still

            float heightCM = measurePersonHeight();

            if (heightCM < 0) {
              lcdClear();
              lcdPrint(0, "  Measurement      ", true);
              lcdPrint(1, "  ERROR!           ", true);
              lcdPrint(2, "  Ensure person is ", true);
              lcdPrint(3, "  under sensor     ", true);
              Serial.println("ERROR");
              delay(3000);
              currentState = STATE_READY;
              showLCDReady();
            } else {
              lastHeightCM = heightCM;
              float heightIN = heightCM / 2.54;
              showLCDResult(heightCM, heightIN);
              Serial.println(String(heightCM, 1));
              startBuzzer(4000);
              currentState = STATE_RESULT;
              stateEnteredAt = millis();
            }
          }
        }
      }
      break;
    }

    case STATE_RESULT:
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