# Arduino Environment Monitor

A continuous environment monitoring station built with the **Elegoo UNO R3 Super Starter Kit**. Reads temperature, humidity, and ambient light — displays on an LCD, stores data in EEPROM, and uploads to ThingSpeak on demand.

## Hardware

### Required (all in the Elegoo kit)

| Component | Purpose | Arduino Pin |
|-----------|---------|-------------|
| Arduino UNO R3 | Controller | — |
| DHT11 | Temperature + Humidity | Pin 2 (data) |
| LCD 1602 | 16×2 character display | RS→7, E→8, D4→9, D5→10, D6→11, D7→12 |
| 10K potentiometer | LCD contrast | V0 (wiper) |
| 220Ω resistor | LCD backlight | Pin 15 (A) |
| LDR + 10KΩ resistor | Ambient light (voltage divider) | A0 |
| Push button (×2) | Refresh + control | Pin 5 (refresh), Pin 6 (unit/page) |
| 830-point breadboard | Prototyping | — |
| Jumper wires | Connections | — |
| USB cable | Power + serial | — |

### Wiring

```
         DHT11              LCD 1602              LDR
       ┌──────┐          ┌──────────┐         ┌──────┐
       │ VCC  │── 5V     │ VSS  (1) │── GND   │  A0  │── A0
       │ DATA │── Pin 2  │ VDD  (2) │── 5V    │      │
       │ GND  │── GND    │ V0   (3) │── Pot   └──┬───┘
       └──────┘          │ RS   (4) │── Pin 7     │
                         │ RW   (5) │── GND     10KΩ
          Buttons        │ E    (6) │── Pin 8     │
       ┌────────┐        │ D4  (11) │── Pin 9    GND
       │ Pin 5  │── GND  │ D5  (12) │── Pin 10
       │ Pin 6  │── GND  │ D6  (13) │── Pin 11
       └────────┘        │ D7  (14) │── Pin 12
                         │ A   (15) │── 5V (via 220Ω)
                         │ K   (16) │── GND
                         └──────────┘
```

> **No soldering required.** All connections use jumper wires on the breadboard. The LCD 1602 in this kit comes with pre-soldered pin header.

## Features

### Display (LCD 1602)

| Page | Line 0 | Line 1 | Hold Pin6 to reach |
|------|--------|--------|-------------------|
| **Environment** | `Tmp: 25.5°C` | `Hum:  60.0%` | Default |
| **Light** | `Light Sensor` | `Raw: 723  72%` | Hold Pin6 |

### Button Controls

| Button | Short press | Hold (1s) |
|--------|-------------|-----------|
| **Pin 5** (refresh) | Force DHT11 read now | — |
| **Pin 6** (ctrl) | Toggle unit: °C → °F → K or RH% ↔ Dew Pt | Cycle pages: env ↔ light |

- Units persist across power cycles (stored in EEPROM byte 1022-1023)
- Field selection blinks on LCD for 10s after page switch

### Data Logging (EEPROM)

- 255 readings stored in a circular buffer in the Arduino's EEPROM
- One reading every 2 minutes ≈ 8.5-hour backlog
- Effective lifetime of EEPROM exceeds 30 years with wear leveling
- Data not lost when power is removed

### Cloud Upload (on demand)

Run the Python bridge to upload stored data to **ThingSpeak**:

```bash
cd env-monitor/
pip install pyserial requests
python bridge.py
```

This dumps the EEPROM buffer over serial and sends it to your ThingSpeak channel via the bulk JSON API.

**Live demo channel:** [https://thingspeak.com/channels/3415449](https://thingspeak.com/channels/3415449)

### Local CSV Logging

Each bridge run also appends readings to `env-monitor/log.csv` with approximate timestamps:

```csv
timestamp,temp_c,humidity,source
2026-06-24T02:48:44Z,25.5,60.0,buffer
```

## Software Setup

### Arduino Sketch

```bash
# Install arduino-cli (if needed)
# Linux/macOS:
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh
# Windows (PowerShell):
#   winget install Arduino.ArduinoCLI

arduino-cli core install arduino:avr
arduino-cli lib install "DHT sensor library"
arduino-cli lib install LiquidCrystal

cd env-monitor/
arduino-cli compile --fqbn arduino:avr:uno env-monitor.ino
arduino-cli upload --fqbn arduino:avr:uno -p <PORT> env-monitor.ino
```

### Python Bridge

```bash
pip install pyserial requests
python bridge.py                    # auto-detect port, dump + upload
python bridge.py --port COM3        # Windows: specify COM port
python bridge.py --live             # live reading only (skip buffer)
python bridge.py --no-upload        # dump to CSV only
```

Configure your ThingSpeak API key in `env-monitor/.env`:

```
THINGSPEAK_API_KEY=your_write_api_key
THINGSPEAK_CHANNEL=3415449
```

## Project Structure

```
arduino/
├── .gitignore
├── LICENSE
├── README.md
└── env-monitor/
    ├── AGENTS.md          # Wiring reference + build instructions
    ├── env-monitor.ino    # Arduino sketch
    ├── bridge.py          # Python serial → ThingSpeak bridge
    ├── .env               # API keys (gitignored)
    ├── log.csv            # Local data log (appended each bridge run)
    ├── build/             # Compiled hex (gitignored)
    └── __pycache__/       # Python cache (gitignored)
```

## Roadmap / Ideas

- [ ] Add buzzer alarm for temperature thresholds
- [ ] RGB LED status indicator (green = normal, red = hot)
- [ ] RTC module for real timestamps in EEPROM
- [ ] SD card module for standalone logging (no serial bridge)
- [ ] ESP8266 WiFi module for direct cloud upload

## License

MIT
