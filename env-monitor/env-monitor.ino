// env-monitor — Continuous DHT11 environment monitor
// Reads temp/humidity every 2 min, displays on LCD 1602,
// stores rolling buffer in EEPROM, dump on serial command.
//
// Button:
//   single press → force DHT11 read now
//   double press → select which field (temp/hum) to toggle
//   hold 1s+    → toggle unit for selected field (°C↔°F)
//   selected field blinks on LCD
//
// Wiring:
//   DHT11 data → Pin 2
//   Button (to GND, internal pullup) → Pin 6
//   LCD RS → Pin 7,  E → Pin 8,  D4→9,  D5→10,  D6→11,  D7→12
//   LCD V0 → Pot wiper (10K pot between 5V and GND)
//   LCD A → 5V via 220Ω resistor

#include <LiquidCrystal.h>
#include <DHT.h>
#include <EEPROM.h>

// ── Pins ──
#define DHTPIN 2
#define DHTTYPE DHT11
#define BTN_PIN 6
#define LCD_RS 7
#define LCD_E 8
#define LCD_D4 9
#define LCD_D5 10
#define LCD_D6 11
#define LCD_D7 12

// ── EEPROM layout (1024 bytes total) ──
//   Byte 0:      write_index
//   Bytes 1..510: data buffer (255 slots × 2 bytes)
//   Byte 1023:    temp unit (0=°C, 1=°F)
#define EEPROM_IDX_ADDR 0
#define EEPROM_TUNIT_ADDR 1023
#define EEPROM_DATA_START 1
#define EEPROM_SLOTS 255

// Timings
#define READ_INTERVAL_MS 120000L
#define BTN_DEBOUNCE_MS 50
#define HOLD_THRESHOLD_MS 1000
#define DOUBLE_CLICK_WINDOW_MS 400
#define FIELD_SELECT_TIMEOUT_MS 10000
#define BLINK_INTERVAL_MS 500

// Button actions
enum BtnAction { ACT_NONE, ACT_READ_NOW, ACT_TOGGLE_UNIT, ACT_SWITCH_FIELD };
enum TempUnit { UNIT_C = 0, UNIT_F = 1 };

LiquidCrystal lcd(LCD_RS, LCD_E, LCD_D4, LCD_D5, LCD_D6, LCD_D7);
DHT dht(DHTPIN, DHTTYPE);

// ── State ──
uint8_t writeIdx;
TempUnit tempUnit = UNIT_C;
bool fieldSelected = false;     // true when double-press activated selection
bool fieldIsTemp = true;        // true=temp selected, false=humidity selected
unsigned long lastFieldActivity = 0;
unsigned long lastReadMs = 0;
float currentTemp = NAN, currentHum = NAN;
bool sensorError = false;

// Button state machine
enum BtnSm { SM_IDLE, SM_PRESSED, SM_RELEASE_WAIT };
BtnSm btnSm = SM_IDLE;
unsigned long btnPressMs = 0;
unsigned long btnReleaseMs = 0;
bool holdFired = false;

void setup() {
  Serial.begin(9600);
  dht.begin();
  lcd.begin(16, 2);
  pinMode(BTN_PIN, INPUT_PULLUP);

  // Restore EEPROM write index
  writeIdx = EEPROM.read(EEPROM_IDX_ADDR);
  if (writeIdx >= EEPROM_SLOTS) writeIdx = 0;

  // Restore temp unit
  uint8_t u = EEPROM.read(EEPROM_TUNIT_ADDR);
  tempUnit = (u == UNIT_F) ? UNIT_F : UNIT_C;

  lcd.print("  Env Monitor");
  lcd.setCursor(0, 1);
  lcd.print("  v1.1 ready");

  lastReadMs = millis() - READ_INTERVAL_MS + 5000;  // first read in ~5s
}

void loop() {
  handleSerial();
  BtnAction action = pollButton();
  handleButtonAction(action);
  handleReading();
  updateLcd();
}

// ── Serial commands ──
void handleSerial() {
  if (Serial.available() <= 0) return;
  char cmd = Serial.read();
  while (Serial.available() > 0) Serial.read();
  switch (cmd) {
    case 'D': case 'd': dumpBuffer(); break;
    case 'L': case 'l': sendLiveReading(); break;
    case 'Q': case 'q': Serial.println("OK,RESUME"); break;
  }
}

// ── Non-blocking button state machine ──
BtnAction pollButton() {
  bool pressed = (digitalRead(BTN_PIN) == LOW);
  unsigned long now = millis();

  switch (btnSm) {
    case SM_IDLE:
      if (pressed) {
        btnPressMs = now;
        holdFired = false;
        btnSm = SM_PRESSED;
      }
      return ACT_NONE;

    case SM_PRESSED:
      if (!pressed) {
        // Released before hold threshold → short click
        unsigned long dur = now - btnPressMs;
        if (dur >= BTN_DEBOUNCE_MS) {
          btnReleaseMs = now;
          btnSm = SM_RELEASE_WAIT;
        } else {
          btnSm = SM_IDLE;
        }
        return ACT_NONE;
      }
      // Still pressed — check hold
      if (!holdFired && (now - btnPressMs >= HOLD_THRESHOLD_MS)) {
        holdFired = true;
        btnSm = SM_PRESSED;  // stay in pressed until release
        return ACT_TOGGLE_UNIT;
      }
      return ACT_NONE;

    case SM_RELEASE_WAIT:
      // Wait for double-click window or second press
      if (pressed && (now - btnReleaseMs < DOUBLE_CLICK_WINDOW_MS)) {
        // Second press detected → double click
        btnSm = SM_IDLE;
        return ACT_SWITCH_FIELD;
      }
      if (now - btnReleaseMs >= DOUBLE_CLICK_WINDOW_MS) {
        // Timeout → single click
        btnSm = SM_IDLE;
        return ACT_READ_NOW;
      }
      return ACT_NONE;
  }
  return ACT_NONE;
}

// ── Execute button actions ──
void handleButtonAction(BtnAction action) {
  switch (action) {
    case ACT_READ_NOW:
      doRead();
      break;
    case ACT_TOGGLE_UNIT:
      tempUnit = (tempUnit == UNIT_C) ? UNIT_F : UNIT_C;
      EEPROM.write(EEPROM_TUNIT_ADDR, (uint8_t)tempUnit);
      doRead();
      break;
    case ACT_SWITCH_FIELD:
      fieldSelected = true;
      fieldIsTemp = !fieldIsTemp;
      lastFieldActivity = millis();
      break;
    default: break;
  }
}

// ── Periodic sensor read ──
void handleReading() {
  if (millis() - lastReadMs < READ_INTERVAL_MS) return;
  doRead();
}

void doRead() {
  lastReadMs = millis();
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(h) || isnan(t)) {
    sensorError = true;
    return;
  }

  sensorError = false;
  currentTemp = t;
  currentHum = h;
  storeReading(t, h);
}

// ── EEPROM storage ──
void storeReading(float temp, float hum) {
  int8_t t = (int8_t)round(constrain(temp, -128.0, 127.0));
  uint8_t h = (uint8_t)round(constrain(hum, 0.0, 255.0));
  uint16_t addr = EEPROM_DATA_START + writeIdx * 2;
  EEPROM.write(addr, (uint8_t)t);
  EEPROM.write(addr + 1, h);
  writeIdx = (writeIdx + 1) % EEPROM_SLOTS;
  EEPROM.write(EEPROM_IDX_ADDR, writeIdx);
}

// ── LCD display ──
void updateLcd() {
  if (sensorError) {
    lcd.clear();
    lcd.print("Sensor error");
    lcd.setCursor(0, 1);
    lcd.print("Check DHT11");
    lcd.noBlink();
    return;
  }
  if (isnan(currentTemp) || isnan(currentHum)) return;

  // Convert temperature
  float dispTemp = (tempUnit == UNIT_F) ? (currentTemp * 9.0 / 5.0 + 32.0) : currentTemp;
  char unitChar = (tempUnit == UNIT_F) ? 'F' : 'C';

  lcd.clear();

  // Line 0: "Temp: XX.X°C"
  lcd.setCursor(0, 0);
  lcd.print("Temp: ");
  lcd.print(dispTemp, 1);
  lcd.print((char)223);
  lcd.print(unitChar);

  // Line 1: "Hum:  XX.X%"
  lcd.setCursor(0, 1);
  lcd.print("Hum:  ");
  lcd.print(currentHum, 1);
  lcd.print("%");

  // Blink cursor on selected field
  unsigned long now = millis();
  if (fieldSelected && (now - lastFieldActivity < FIELD_SELECT_TIMEOUT_MS)) {
    if (fieldIsTemp) {
      lcd.setCursor(12, 0);  // after "Temp: XX.X°C"
    } else {
      lcd.setCursor(11, 1);  // after "Hum:  XX.X%"
    }
    lcd.blink();
  } else {
    lcd.noBlink();
    fieldSelected = false;
  }
}

// ── Serial: dump EEPROM buffer ──
void dumpBuffer() {
  uint8_t idx = EEPROM.read(EEPROM_IDX_ADDR);
  if (idx >= EEPROM_SLOTS) idx = 0;

  // Compute current temp unit for the dump metadata
  uint8_t rawUnit = EEPROM.read(EEPROM_TUNIT_ADDR);
  Serial.print("TUNIT,");
  Serial.println(rawUnit == UNIT_F ? 1 : 0);

  uint8_t oldest = (idx == 0) ? (EEPROM_SLOTS - 1) : (idx - 1);
  Serial.println("---BUFFER_START---");
  int count = 0;
  for (uint8_t i = 0; i < EEPROM_SLOTS; i++) {
    uint8_t slot = (oldest + i) % EEPROM_SLOTS;
    uint16_t addr = EEPROM_DATA_START + slot * 2;
    uint8_t raw_t = EEPROM.read(addr);
    uint8_t raw_h = EEPROM.read(addr + 1);
    if (raw_t == 0xFF && raw_h == 0xFF) continue;
    int8_t t = (int8_t)raw_t;
    Serial.print(count);
    Serial.print(",");
    Serial.print(t);
    Serial.print(",");
    Serial.println(raw_h);
    count++;
  }
  Serial.println("---BUFFER_END---");
  Serial.print("COUNT,");
  Serial.println(count);
}

// ── Serial: live reading ──
void sendLiveReading() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (isnan(h) || isnan(t)) {
    Serial.println("ERR,SENSOR");
    return;
  }
  Serial.print("LIVE,");
  Serial.print(t, 1);
  Serial.print(",");
  Serial.println(h, 1);
}
