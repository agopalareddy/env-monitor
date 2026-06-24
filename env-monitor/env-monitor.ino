// env-monitor — Continuous DHT11 environment monitor
// Reads temp/humidity every 2 min, displays on LCD 1602,
// stores rolling buffer in EEPROM, dump on serial command or button press.
//
// Wiring (matches breadboard instructions received):
//   DHT11 data → Pin 2
//   Button (to GND, internal pullup) → Pin 6
//   LCD RS → Pin 7,  E → Pin 8,  D4→9,  D5→10,  D6→11,  D7→12
//   LCD V0 → Pot wiper (10K pot between 5V and GND)
//   LCD A → 5V via 220Ω resistor

#include <LiquidCrystal.h>
#include <DHT.h>
#include <EEPROM.h>

// Pins
#define DHTPIN 2
#define DHTTYPE DHT11
#define BTN_PIN 6
#define LCD_RS 7
#define LCD_E 8
#define LCD_D4 9
#define LCD_D5 10
#define LCD_D6 11
#define LCD_D7 12

// EEPROM layout
// Byte 0: write_index (next slot, 0..254, 0xFF = uninitialized)
// Bytes 1..510: data buffer, 255 slots × 2 bytes each (temp, humidity)
#define EEPROM_IDX_ADDR 0
#define EEPROM_DATA_START 1
#define EEPROM_SLOTS 255
#define EEPROM_VALID_MIN 1
#define EEPROM_VALID_MAX (EEPROM_DATA_START + EEPROM_SLOTS * 2)  // 511

// Timing (milliseconds)
#define READ_INTERVAL_MS 120000L  // 2 minutes
#define BTN_DEBOUNCE_MS 50
#define DHT_READ_DELAY_MS 250    // wait after DHT request

LiquidCrystal lcd(LCD_RS, LCD_E, LCD_D4, LCD_D5, LCD_D6, LCD_D7);
DHT dht(DHTPIN, DHTTYPE);

unsigned long lastReadMs = 0;
unsigned long lastBtnMs = 0;
uint8_t writeIdx = 0;
bool lcdInitialized = false;

void setup() {
  Serial.begin(9600);
  dht.begin();
  lcd.begin(16, 2);
  pinMode(BTN_PIN, INPUT_PULLUP);

  lcd.print("  Env Monitor");
  lcd.setCursor(0, 1);
  lcd.print("  v1.0 ready");
  lcdInitialized = true;

  // Restore or initialize EEPROM write index
  writeIdx = EEPROM.read(EEPROM_IDX_ADDR);
  if (writeIdx >= EEPROM_SLOTS) {
    writeIdx = 0;
    EEPROM.write(EEPROM_IDX_ADDR, writeIdx);
  }

  lastReadMs = millis() - READ_INTERVAL_MS + 5000;  // first reading in ~5 seconds
}

void loop() {
  handleSerial();
  handleButton();
  handleReading();
}

// ── Serial commands from bridge.py ──
void handleSerial() {
  if (Serial.available() <= 0) return;
  char cmd = Serial.read();
  // flush any remaining bytes in buffer
  while (Serial.available() > 0) Serial.read();

  switch (cmd) {
    case 'D': case 'd': dumpBuffer(); break;
    case 'L': case 'l': sendLiveReading(); break;
    case 'Q': case 'q': Serial.println("OK,RESUME"); break;
  }
}

// ── Button → dump buffer ──
void handleButton() {
  if (digitalRead(BTN_PIN) == LOW) {
    unsigned long now = millis();
    if (now - lastBtnMs > BTN_DEBOUNCE_MS) {
      lastBtnMs = now;
      // Wait for release (with timeout)
      unsigned long releaseTimeout = now + 5000;
      while (digitalRead(BTN_PIN) == LOW && millis() < releaseTimeout) {
        delay(10);
      }
      Serial.println("BTN,DUMP");
      dumpBuffer();
    }
  }
}

// ── Periodic sensor read ──
void handleReading() {
  unsigned long now = millis();
  if (now - lastReadMs < READ_INTERVAL_MS) return;
  lastReadMs = now;

  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(h) || isnan(t)) {
    if (lcdInitialized) {
      lcd.clear();
      lcd.print("Sensor error");
      lcd.setCursor(0, 1);
      lcd.print("Check DHT11");
    }
    return;
  }

  storeReading(t, h);
  updateLcd(t, h);
}

// ── Storage ──
void storeReading(float temp, float hum) {
  // Clamp to int8/uint8 range
  int8_t t = (int8_t)round(constrain(temp, -128.0, 127.0));
  uint8_t h = (uint8_t)round(constrain(hum, 0.0, 255.0));

  uint16_t addr = EEPROM_DATA_START + writeIdx * 2;
  EEPROM.write(addr, (uint8_t)t);
  EEPROM.write(addr + 1, h);

  writeIdx = (writeIdx + 1) % EEPROM_SLOTS;
  EEPROM.write(EEPROM_IDX_ADDR, writeIdx);
}

// ── LCD display ──
void updateLcd(float t, float h) {
  if (!lcdInitialized) return;
  lcd.clear();
  lcd.print("Temp: ");
  lcd.print(t, 1);
  lcd.print((char)223);  // degree symbol
  lcd.print("C");

  lcd.setCursor(0, 1);
  lcd.print("Hum:  ");
  lcd.print(h, 1);
  lcd.print("%");
}

// ── Dump entire buffer over serial ──
void dumpBuffer() {
  uint8_t idx = EEPROM.read(EEPROM_IDX_ADDR);
  if (idx >= EEPROM_SLOTS) idx = 0;

  // Oldest valid slot is the one after current writeIdx
  uint8_t oldest = (idx == 0) ? (EEPROM_SLOTS - 1) : (idx - 1);

  Serial.println("---BUFFER_START---");
  int count = 0;
  for (uint8_t i = 0; i < EEPROM_SLOTS; i++) {
    uint8_t slot = (oldest + i) % EEPROM_SLOTS;
    uint16_t addr = EEPROM_DATA_START + slot * 2;
    uint8_t raw_t = EEPROM.read(addr);
    uint8_t raw_h = EEPROM.read(addr + 1);

    // Uninitialized slot = 0xFFFF
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

// ── Live reading ──
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
