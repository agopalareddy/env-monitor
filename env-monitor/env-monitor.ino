// env-monitor — Continuous DHT11 environment monitor
// Reads temp/humidity every 2 min, displays on LCD 1602,
// stores rolling buffer in EEPROM, dump on serial command.
//
// Buttons:
//   Pin5 click → force DHT11 read now (refresh)
//   Pin6 click → toggle unit for selected field (°C↔°F), none on light page
//   Pin6 hold  → cycle display pages (env → light → env)
//
// Wiring:
//   DHT11 data → Pin 2
//   Button1 (refresh, to GND) → Pin 5
//   Button2 (unit/page, to GND) → Pin 6
//   LDR + 10kΩ voltage divider → A0
//   LCD RS → Pin 7,  E → Pin 8,  D4→9,  D5→10,  D6→11,  D7→12
//   LCD V0 → Pot wiper (10K pot between 5V and GND)
//   LCD A → 5V via 220Ω resistor

#include <LiquidCrystal.h>
#include <DHT.h>
#include <EEPROM.h>

// ── Pins ──
#define DHTPIN 2
#define DHTTYPE DHT11
#define BTN_REFRESH 5
#define BTN_CTRL 6
#define LCD_RS 7
#define LCD_E 8
#define LCD_D4 9
#define LCD_D5 10
#define LCD_D6 11
#define LCD_D7 12
#define LDR_PIN A0

// ── EEPROM layout (1024 bytes total) ──
#define EEPROM_IDX_ADDR 0
#define EEPROM_TUNIT_ADDR 1023
#define EEPROM_HUNIT_ADDR 1022
#define EEPROM_DATA_START 1
#define EEPROM_SLOTS 255

// Timings
#define READ_INTERVAL_MS 120000L
#define LDR_INTERVAL_MS 500
#define BTN_DEBOUNCE_MS 50
#define HOLD_THRESHOLD_MS 1000
#define FIELD_SELECT_TIMEOUT_MS 10000

// Button actions
enum BtnAction { ACT_NONE, ACT_TOGGLE_UNIT, ACT_SWITCH_FIELD };
enum TempUnit { UNIT_C = 0, UNIT_F = 1, UNIT_K = 2 };
enum HumUnit { UNIT_RH = 0, UNIT_DEW = 1 };

LiquidCrystal lcd(LCD_RS, LCD_E, LCD_D4, LCD_D5, LCD_D6, LCD_D7);
DHT dht(DHTPIN, DHTTYPE);

// ── State ──
uint8_t writeIdx;
uint8_t displayPage = 0;  // 0=env, 1=light
TempUnit tempUnit = UNIT_C;
HumUnit humUnit = UNIT_RH;
bool fieldSelected = false;
bool fieldIsTemp = true;
unsigned long lastFieldActivity = 0;
unsigned long lastReadMs = 0;
float currentTemp = NAN, currentHum = NAN;
bool sensorError = false;

// Refresh button (Pin5) state
bool refreshWasLow = false;

// Ctrl button (Pin6) state machine
enum CtrlSm { CTRL_IDLE, CTRL_PRESSED };
CtrlSm ctrlSm = CTRL_IDLE;
unsigned long ctrlPressMs = 0;
unsigned long lastCtrlMs = 0;

// LDR
unsigned long lastLdrMs = 0;
int currentLightRaw = 0;

void setup() {
  Serial.begin(9600);
  dht.begin();
  lcd.begin(16, 2);
  pinMode(BTN_REFRESH, INPUT_PULLUP);
  pinMode(BTN_CTRL, INPUT_PULLUP);

  writeIdx = EEPROM.read(EEPROM_IDX_ADDR);
  if (writeIdx >= EEPROM_SLOTS) writeIdx = 0;

  uint8_t u = EEPROM.read(EEPROM_TUNIT_ADDR);
  tempUnit = (u == UNIT_F) ? UNIT_F : UNIT_C;
  humUnit = (EEPROM.read(EEPROM_HUNIT_ADDR) == UNIT_DEW) ? UNIT_DEW : UNIT_RH;

  lcd.print("  Env Monitor");
  lcd.setCursor(0, 1);
  lcd.print("  v1.2 ready");

  lastReadMs = millis() - READ_INTERVAL_MS + 5000;
}

void loop() {
  handleSerial();
  pollRefreshBtn();
  handleButtonAction(pollCtrlBtn());
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

// ── Refresh button (Pin5): edge-triggered → read now ──
void pollRefreshBtn() {
  bool low = (digitalRead(BTN_REFRESH) == LOW);
  if (low && !refreshWasLow) {
    refreshWasLow = true;
    doRead();
  }
  refreshWasLow = low;
}

// ── Ctrl button (Pin6): short press=toggle unit, hold=switch field ──
BtnAction pollCtrlBtn() {
  bool pressed = (digitalRead(BTN_CTRL) == LOW);
  unsigned long now = millis();

  switch (ctrlSm) {
    case CTRL_IDLE:
      if (pressed && (now - lastCtrlMs > BTN_DEBOUNCE_MS)) {
        lastCtrlMs = now;
        ctrlPressMs = now;
        ctrlSm = CTRL_PRESSED;
      }
      return ACT_NONE;

    case CTRL_PRESSED:
      if (!pressed) {
        ctrlSm = CTRL_IDLE;
        if (now - ctrlPressMs < HOLD_THRESHOLD_MS) {
          return ACT_TOGGLE_UNIT;  // short → unit
        }
        return ACT_NONE;
      }
      // Still held — fire once on threshold
      if (now - ctrlPressMs >= HOLD_THRESHOLD_MS) {
        ctrlPressMs = now + HOLD_THRESHOLD_MS * 2;  // prevent re-fire
        return ACT_SWITCH_FIELD;
      }
      return ACT_NONE;
  }
  return ACT_NONE;
}

// ── Execute button actions ──
void handleButtonAction(BtnAction action) {
  switch (action) {
    case ACT_TOGGLE_UNIT:
      if (displayPage == 0 && fieldIsTemp) {
        tempUnit = (TempUnit)((tempUnit + 1) % 3);
        EEPROM.write(EEPROM_TUNIT_ADDR, (uint8_t)tempUnit);
      } else if (displayPage == 0 && !fieldIsTemp) {
        humUnit = (humUnit == UNIT_RH) ? UNIT_DEW : UNIT_RH;
        EEPROM.write(EEPROM_HUNIT_ADDR, (uint8_t)humUnit);
      }
      if (displayPage == 0) doRead();
      break;
    case ACT_SWITCH_FIELD:
      displayPage = (displayPage + 1) % 2;
      fieldSelected = true;
      lastFieldActivity = millis();
      break;
    default: break;
  }
}

// ── Reading ──
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
  if (displayPage == 0) {
    updateLcdEnv();
  } else {
    updateLcdLight();
  }
}

void updateLcdEnv() {
  if (sensorError) {
    lcd.clear();
    lcd.print("Sensor error");
    lcd.setCursor(0, 1);
    lcd.print("Check DHT11");
    lcd.noBlink();
    return;
  }
  if (isnan(currentTemp) || isnan(currentHum)) return;

  lcd.clear();

  // Display temp
  lcd.setCursor(0, 0);
  lcd.print("Tmp:");
  if (tempUnit == UNIT_K) {
    lcd.print(currentTemp + 273.15, 1);
    lcd.print("K ");
  } else {
    float dispTemp = (tempUnit == UNIT_F) ? (currentTemp * 9.0 / 5.0 + 32.0) : currentTemp;
    lcd.print(dispTemp, 1);
    lcd.print((char)223);
    lcd.print(tempUnit == UNIT_F ? 'F' : 'C');
  }

  // Line 1: humidity or dew point
  lcd.setCursor(0, 1);
  if (humUnit == UNIT_RH) {
    lcd.print("Hum:  ");
    lcd.print(currentHum, 1);
    lcd.print("%");
  } else {
    float dewPt = calcDewPoint(currentTemp, currentHum);
    lcd.print("DP: ");
    if (tempUnit == UNIT_K) {
      lcd.print(dewPt + 273.15, 1);
      lcd.print("K ");
    } else {
      if (tempUnit == UNIT_F) dewPt = dewPt * 9.0 / 5.0 + 32.0;
      lcd.print(dewPt, 1);
      lcd.print((char)223);
      lcd.print(tempUnit == UNIT_F ? 'F' : 'C');
    }
  }

  // Blink cursor
  unsigned long now = millis();
  if (fieldSelected && (now - lastFieldActivity < FIELD_SELECT_TIMEOUT_MS)) {
    if (fieldIsTemp) {
      lcd.setCursor(12, 0);
    } else {
      lcd.setCursor(humUnit == UNIT_RH ? 11 : 9, 1);
    }
    lcd.blink();
  } else {
    lcd.noBlink();
    fieldSelected = false;
  }
}

// Dew point: Magnus formula (0..50°C, 0..100% RH)
float calcDewPoint(float t, float rh) {
  float a = 17.27, b = 237.7;
  float gamma = log(rh / 100.0) + (a * t) / (b + t);
  return (b * gamma) / (a - gamma);
}

// ── LCD page: light sensor (LDR on A0, updated every 500ms) ──
void updateLcdLight() {
  unsigned long now = millis();
  if (now - lastLdrMs >= LDR_INTERVAL_MS) {
    lastLdrMs = now;
    currentLightRaw = analogRead(LDR_PIN);
  }
  int pct = constrain(map(currentLightRaw, 0, 1023, 0, 100), 0, 100);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Light Sensor");

  lcd.setCursor(0, 1);
  lcd.print("Raw:");
  lcd.print(currentLightRaw);
  lcd.print("  ");
  lcd.print(pct);
  lcd.print("%");
  lcd.noBlink();
}

// ── Serial: dump EEPROM buffer ──
void dumpBuffer() {
  uint8_t idx = EEPROM.read(EEPROM_IDX_ADDR);
  if (idx >= EEPROM_SLOTS) idx = 0;

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
