# env-monitor AGENTS.md

Continuous environment monitor: DHT11 → LCD 1602 → EEPROM buffer → ThingSpeak cloud on demand.

## Wiring

| Component | Arduino Pin |
|-----------|-------------|
| DHT11 data | Pin 2 |
| Button 1 (refresh → read now) | Pin 5 (to GND) |
| Button 2 (click=unit, hold=field) | Pin 6 (to GND) |
| LCD RS | Pin 7 |
| LCD E | Pin 8 |
| LCD D4 | Pin 9 |
| LCD D5 | Pin 10 |
| LCD D6 | Pin 11 |
| LCD D7 | Pin 12 |
| LCD V0 → Pot wiper (10K pot 5V-GND) | |
| LCD A → 5V via 220Ω | |
| LCD K → GND | |
| LCD VSS → GND, VDD → 5V, RW → GND | |

## Upload sketch to Arduino

```bash
# From laptop with Arduino connected:
arduino-cli compile --fqbn arduino:avr:uno env-monitor.ino
arduino-cli upload --fqbn arduino:avr:uno --port /dev/ttyACM0 env-monitor.ino
```

Or open `env-monitor.ino` in Arduino IDE and upload.

## Run bridge (on-demand from laptop)

```bash
pip install pyserial requests
python bridge.py                    # auto-detect port, dump + upload
python bridge.py --port /dev/ttyACM0  # explicit port
python bridge.py --live             # live reading only
python bridge.py --no-upload        # dump to CSV only
```

Bridge sends `D` → Arduino dumps EEPROM buffer → uploaded to ThingSpeak via bulk JSON + live reading via GET.

## Expected behavior

- **Standalone**: LCD shows "Temp: XX.X°C" / "Hum: XX.X%", updates every 2 min
- **Pin5 button press**: forces DHT11 read now (LCD updates immediately)
- **Pin6 click**: toggles unit °C↔°F for selected field
- **Pin6 hold (1s)**: switches which field blinks (temp ↔ hum)
- **Bridge run**: pulls buffer + live reading, uploads to ThingSpeak, appends `log.csv`
- **ThingSpeak**: https://thingspeak.com/channels/3415449

## EEPROM layout

| Address | Size | Content |
|---------|------|---------|
| 0 | 1 byte | write index (0–254, next slot) |
| 1–510 | 2×255 bytes | data buffer: temp (int8) + humidity (uint8) per slot |

- 255 slots × 2 min = ~8.5 hr backlog at 2 min interval. Rolling buffer, oldest overwritten.
- Wear leveling via circular buffer → ~195 yr lifetime.

## Serial protocol

| Command | Response |
|---------|----------|
| `D` | `---BUFFER_START---` ... `idx,temp,hum\n` ... `---BUFFER_END---` `COUNT,N` |
| `L` | `LIVE,temp,hum` or `ERR,SENSOR` |
| `Q` | `OK,RESUME` |
| Pin5 button | Forces DHT11 read now |
| Pin6 click | Toggle °C/°F unit |
| Pin6 hold | Switch selected field (temp↔hum) |

## Files

| File | Purpose |
|------|---------|
| `env-monitor.ino` | Arduino sketch (DHT11 → LCD → EEPROM) |
| `bridge.py` | Python on-demand serial → ThingSpeak + CSV |
| `.env` | API keys (gitignored) |
| `log.csv` | Local data log (appended each bridge run) |
| `build/` | Compiled hex binary (gitignored) |
