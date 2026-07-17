/*
 * ═══════════════════════════════════════════════════════════════
 *  CognoSpace Height Measurement System (HMS)
 *  ESP32 BLE Firmware v2.0
 * 
 *  Hardware:
 *   - ESP32 (any variant)
 *   - HC-SR04 Ultrasonic Sensor (mounted on ceiling/top fixture)
 *   - 20×4 I2C LCD Display (address 0x27)
 *   - Active Buzzer
 *   - Push Button (trigger measurement)
 * 
 *  BLE Device Name: CognoSpace HMS
 *  Service UUID:    4fafc201-1fb5-459e-8fcc-c5c9c331914b
 *  Height Char:     beb5483e-36e1-4688-b7f5-ea07361b26a8 (Notify)
 *  Command Char:    beb5483e-36e1-4688-b7f5-ea07361b26a9 (Write)
 * ═══════════════════════════════════════════════════════════════
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ─── PIN DEFINITIONS ───────────────────────────────────────────
#define TRIG_PIN     5      // HC-SR04 Trigger
#define ECHO_PIN     18     // HC-SR04 Echo
#define BUZZER_PIN   19     // Active Buzzer
#define BUTTON_PIN   23     // Push Button (INPUT_PULLUP)
#define SDA_PIN      21     // I2C SDA
#define SCL_PIN      22     // I2C SCL

// ─── PHYSICAL SETUP ────────────────────────────────────────────
// The sensor is mounted on the ceiling pointing downward.
// CEILING_HEIGHT_CM = the measured distance from the floor to the sensor.
// Calibrate this value precisely with a tape measure.
#define CEILING_HEIGHT_CM  220.0   // ← Set your actual ceiling/mount height in cm
#define NUM_SAMPLES         7      // Readings averaged per measurement
#define SAMPLE_DELAY_MS    50      // Delay between samples
#define MAX_VALID_DIST_CM  210.0   // Ignore readings above this (empty room)
#define MIN_VALID_DIST_CM   20.0   // Ignore readings below this (noise)

// ─── LCD ───────────────────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 20, 4);

// ─── BLE UUIDs ─────────────────────────────────────────────────
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define HEIGHT_CHAR_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CMD_CHAR_UUID       "beb5483e-36e1-4688-b7f5-ea07361b26a9"

BLEServer*          pServer      = nullptr;
BLECharacteristic*  heightChar   = nullptr;
BLECharacteristic*  cmdChar      = nullptr;
bool                deviceConnected = false;
bool                wasConnected    = false;

// ─── STATE MACHINE ─────────────────────────────────────────────
enum State {
  STATE_IDLE,
  STATE_WELCOME,
  STATE_READY,
  STATE_MEASURING,
  STATE_RESULT,
  STATE_BLE_WAIT
};
State currentState = STATE_BLE_WAIT;

// ─── GLOBALS ───────────────────────────────────────────────────
String personName   = "";
String personGender = "";
float  lastHeightCM = 0;
unsigned long buzzerEndTime  = 0;
unsigned long stateEnteredAt = 0;
bool   buzzerOn     = false;
bool   measureNow   = false;   // set by BLE CMD or button ISR


// ═══════════════════════════════════════════════════════════════
//  BLE SERVER CALLBACKS
// ═══════════════════════════════════════════════════════════════
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pSvr) override {
    deviceConnected = true;
    Serial.println("[BLE] Client connected");
  }
  void onDisconnect(BLEServer* pSvr) override {
    deviceConnected = false;
    Serial.println("[BLE] Client disconnected");
  }
};

// ─── Command characteristic: receives NAME and MEASURE from web ─
class CmdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    String raw = String(pChar->getValue().c_str());
    raw.trim();
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
};

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
  lcdPrint(3, deviceConnected ? "  BLE Connected    " : "  Waiting BLE...   ", true);
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
  lcdPrint(2, "Press button or    ", false);
  lcdPrint(3, "click START on app ", false);
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

// Returns person height in cm; -1 if failed
float measurePersonHeight() {
  float readings[NUM_SAMPLES];
  int validCount = 0;

  for (int i = 0; i < NUM_SAMPLES; i++) {
    float d = readRawDistanceCM();
    if (d >= MIN_VALID_DIST_CM && d <= MAX_VALID_DIST_CM) {
      readings[validCount++] = d;
    }
    delay(SAMPLE_DELAY_MS);
  }

  if (validCount < 3) return -1.0; // Too few valid readings

  // Sort and take median 3
  for (int i = 0; i < validCount - 1; i++)
    for (int j = i + 1; j < validCount; j++)
      if (readings[i] > readings[j]) { float t = readings[i]; readings[i] = readings[j]; readings[j] = t; }

  int start = (validCount - 3) / 2;
  float avg = (readings[start] + readings[start + 1] + readings[start + 2]) / 3.0;

  float height = CEILING_HEIGHT_CM - avg;
  if (height < 50 || height > 250) return -1.0; // Sanity check
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
  Serial.println("\n[HMS] CognoSpace Height Measurement System v2.0");

  // Pin modes
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
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

  // BLE init
  BLEDevice::init("CognoSpace HMS");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  // Height characteristic: notify
  heightChar = pService->createCharacteristic(HEIGHT_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  heightChar->addDescriptor(new BLE2902());

  // Command characteristic: write
  cmdChar = pService->createCharacteristic(CMD_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE);
  cmdChar->setCallbacks(new CmdCallbacks());

  pService->start();

  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->setScanResponse(true);
  pAdv->setMinPreferred(0x06);
  BLEDevice::startAdvertising();

  Serial.println("[BLE] Advertising as 'CognoSpace HMS'");

  showLCDIdle();
  currentState = STATE_BLE_WAIT;
}

// ═══════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
  updateBuzzer();

  // BLE reconnect
  if (!deviceConnected && wasConnected) {
    delay(300);
    pServer->startAdvertising();
    Serial.println("[BLE] Re-advertising...");
    wasConnected = false;
    currentState = STATE_BLE_WAIT;
    showLCDIdle();
  }
  if (deviceConnected && !wasConnected) {
    wasConnected = true;
    currentState = STATE_IDLE;
    lcdClear();
    lcdPrint(0, "  BLE Connected!   ", true);
    lcdPrint(1, "  CognoSpace HMS   ", true);
    lcdPrint(2, "  Ready for        ", true);
    lcdPrint(3, "  measurement      ", true);
    delay(1500);
    showLCDIdle();
  }

  // Physical button triggers measurement
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50); // debounce
    if (digitalRead(BUTTON_PIN) == LOW) {
      if (currentState == STATE_READY || currentState == STATE_WELCOME) {
        measureNow = true;
        currentState = STATE_MEASURING;
        stateEnteredAt = millis();
      }
      // Wait for release
      while (digitalRead(BUTTON_PIN) == LOW) delay(10);
    }
  }

  // ─── STATE MACHINE ─────────────────────────────────────────
  switch (currentState) {

    case STATE_BLE_WAIT:
      // Waiting for BLE connection - LCD already showing
      if (millis() - stateEnteredAt > 3000) {
        showLCDIdle();
        stateEnteredAt = millis();
      }
      break;

    case STATE_IDLE:
      // Waiting for NAME command from app
      break;

    case STATE_WELCOME:
      showLCDWelcome();
      delay(2500);
      currentState = STATE_READY;
      stateEnteredAt = millis();
      showLCDReady();
      break;

    case STATE_READY:
      // LCD already showing ready, waiting for button or BLE MEASURE cmd
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
          // Notify error
          heightChar->setValue("ERROR");
          heightChar->notify();
          delay(3000);
          currentState = STATE_READY;
          showLCDReady();
        } else {
          lastHeightCM = heightCM;
          float heightIN = heightCM / 2.54;

          // Show result on LCD
          showLCDResult(heightCM, heightIN);

          // Send via BLE
          String payload = String(heightCM, 1);
          heightChar->setValue(payload.c_str());
          heightChar->notify();
          Serial.printf("[HMS] Height: %.1f cm (%.1f in)\n", heightCM, heightIN);

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
        currentState = deviceConnected ? STATE_IDLE : STATE_BLE_WAIT;
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
 *  Push Button
 *  ┌──────┬──────────────────────────────────┐
 *  │ PIN1 │  GPIO 23                         │
 *  │ PIN2 │  GND (uses internal pull-up)     │
 *  └──────┴──────────────────────────────────┘
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
 *  - BLE is built into ESP32 Arduino core (no extra install needed)
 *
 *  Arduino IDE Board: "ESP32 Dev Module"
 *  Upload Speed: 921600
 *  Flash Frequency: 80MHz
 * ═══════════════════════════════════════════════════════════════
 */
