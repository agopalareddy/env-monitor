#!/usr/bin/env python3
"""
bridge.py — On-demand serial bridge: Arduino DHT11 EEPROM buffer → ThingSpeak + CSV.

Usage:
    python bridge.py                     # auto-detect, dump EEPROM + live, upload
    python bridge.py --port /dev/ttyACM0  # specify serial port
    python bridge.py --live              # live reading only (ignore buffer)
    python bridge.py --no-upload         # dump to CSV only, skip ThingSpeak
"""

import argparse
import csv
import os
import sys
import time
import json
from datetime import datetime, timezone, timedelta

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("ERROR: pyserial not installed. Run: pip install pyserial")
    sys.exit(1)

try:
    import requests
except ImportError:
    print("ERROR: requests not installed. Run: pip install requests")
    sys.exit(1)


# ── Config ──────────────────────────────────────────────────────────────
ENV_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env")

API_KEY = None
CHANNEL_ID = None
THINGSPEAK_URL = None
THINGSPEAK_UPDATE_URL = "https://api.thingspeak.com/update"
READ_INTERVAL_MIN = 2

def load_env():
    global API_KEY, CHANNEL_ID, THINGSPEAK_URL, READ_INTERVAL_MIN
    if not os.path.exists(ENV_PATH):
        print("WARNING: .env not found. Create it with THINGSPEAK_API_KEY and THINGSPEAK_CHANNEL.")
        return
    with open(ENV_PATH) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            if k == "THINGSPEAK_API_KEY":
                API_KEY = v
            elif k == "THINGSPEAK_CHANNEL":
                CHANNEL_ID = v
            elif k == "READ_INTERVAL_MIN":
                READ_INTERVAL_MIN = int(v)
    if CHANNEL_ID:
        THINGSPEAK_URL = f"https://api.thingspeak.com/channels/{CHANNEL_ID}/bulk_update.json"

load_env()


# ── Serial ──────────────────────────────────────────────────────────────

def find_port():
    """Auto-detect Arduino serial port."""
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if "Arduino" in p.description or "arduino" in p.description.lower():
            return p.device
        if "USB" in p.description and ("ACM" in p.device or "COM" in p.device):
            return p.device
    # Fallback: common Linux paths
    for path in ["/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyUSB0", "/dev/ttyUSB1"]:
        if os.path.exists(path):
            return path
    return None


def open_serial(port=None):
    """Open serial connection to Arduino."""
    if port is None:
        port = find_port()
    if port is None:
        print("ERROR: No Arduino serial port found.")
        print("  Specify with: --port /dev/ttyACM0")
        print("  Detected ports:")
        for p in serial.tools.list_ports.comports():
            print(f"    {p.device} — {p.description}")
        sys.exit(1)
    print(f"  Serial: {port}")
    ser = serial.Serial(port, 9600, timeout=5)
    time.sleep(2)  # wait for Arduino reset
    # flush any startup messages
    while ser.in_waiting:
        ser.readline()
    return ser


def send_command(ser, cmd):
    """Send single-char command, read response lines until terminator."""
    ser.write(cmd.encode())
    ser.flush()
    time.sleep(0.5)
    lines = []
    while True:
        try:
            line = ser.readline().decode("utf-8", errors="replace").strip()
        except serial.SerialTimeoutException:
            break
        if not line:
            time.sleep(0.2)
            continue
        lines.append(line)
        if line == "---BUFFER_END---":
            break
        if line.startswith("OK,") or line.startswith("ERR,") or line.startswith("LIVE,"):
            break
        if len(lines) > 600:  # safety limit
            break
        if ser.in_waiting == 0:
            time.sleep(0.3)
            if ser.in_waiting == 0:
                break
    return lines


# ── Parsing ─────────────────────────────────────────────────────────────

def parse_dump(lines):
    """Parse dumped buffer into list of (index, temp_c, humidity) tuples."""
    readings = []
    in_buffer = False
    for line in lines:
        if line == "---BUFFER_START---":
            in_buffer = True
            continue
        if line == "---BUFFER_END---":
            in_buffer = False
            continue
        if line.startswith("COUNT,"):
            continue
        if in_buffer and "," in line:
            parts = line.split(",")
            if len(parts) >= 3:
                try:
                    idx = int(parts[0])
                    temp = float(parts[1])
                    hum = float(parts[2])
                    readings.append((idx, temp, hum))
                except ValueError:
                    continue
    return readings


def parse_live(line):
    """Parse a live reading line: LIVE,temp,humidity."""
    if not line.startswith("LIVE,"):
        return None
    parts = line.split(",")
    if len(parts) >= 3:
        try:
            return float(parts[1]), float(parts[2])
        except ValueError:
            return None
    return None


# ── Storage ─────────────────────────────────────────────────────────────

CSV_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "log.csv")

def append_csv(readings, live=None):
    """Append parsed readings to local CSV file."""
    file_exists = os.path.exists(CSV_PATH)
    with open(CSV_PATH, "a", newline="") as f:
        writer = csv.writer(f)
        if not file_exists:
            writer.writerow(["timestamp", "temp_c", "humidity", "source"])
        now = datetime.now(timezone.utc)
        count = len(readings)
        for i, (idx, temp, hum) in enumerate(readings):
            # Reconstruct approximate timestamp from buffer position
            offset_min = (count - 1 - i) * READ_INTERVAL_MIN
            ts = now - timedelta(minutes=offset_min)
            writer.writerow([ts.strftime("%Y-%m-%dT%H:%M:%SZ"), temp, hum, "buffer"])
        if live:
            writer.writerow([now.strftime("%Y-%m-%dT%H:%M:%SZ"), live[0], live[1], "live"])
    print(f"  CSV: {len(readings) + (1 if live else 0)} rows appended to {CSV_PATH}")


# ── ThingSpeak Upload ──────────────────────────────────────────────────

def upload_bulk(readings, live=None):
    """Upload historical readings via bulk JSON, then live reading individually."""
    if not readings and not live:
        print("  ThingSpeak: nothing to upload")
        return
    if not API_KEY or not CHANNEL_ID:
        print("  ThingSpeak: configure .env with THINGSPEAK_API_KEY and THINGSPEAK_CHANNEL")
        return

    # Bulk upload (historical buffer)
    if readings:
        now = datetime.now(timezone.utc)
        updates = []
        count = len(readings)
        for i, (idx, temp, hum) in enumerate(readings):
            offset_min = (count - 1 - i) * READ_INTERVAL_MIN
            ts = now - timedelta(minutes=offset_min)
            updates.append({
                "created_at": ts.strftime("%Y-%m-%dT%H:%M:%SZ"),
                "field1": round(temp, 1),
                "field2": round(hum, 1),
            })

        payload = {"write_api_key": API_KEY, "updates": updates}
        try:
            resp = requests.post(THINGSPEAK_URL, json=payload, timeout=15)
            if resp.status_code == 202:
                print(f"  ThingSpeak: {len(updates)} readings uploaded (bulk)")
            else:
                print(f"  ThingSpeak bulk upload: HTTP {resp.status_code} {resp.text[:200]}")
        except requests.RequestException as e:
            print(f"  ThingSpeak bulk upload error: {e}")

        # Rate limit: 15s between writes on free tier
        print("  Waiting 15s for rate limit...")
        time.sleep(15)

    # Live reading (individual update)
    if live:
        temp, hum = live
        params = {"api_key": API_KEY, "field1": round(temp, 1), "field2": round(hum, 1)}
        try:
            resp = requests.get(THINGSPEAK_UPDATE_URL, params=params, timeout=15)
            if resp.text and resp.text != "0":
                print(f"  ThingSpeak: live reading uploaded (entry {resp.text})")
            else:
                print(f"  ThingSpeak live upload: no response (check API key)")
        except requests.RequestException as e:
            print(f"  ThingSpeak live upload error: {e}")


# ── Main ───────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Arduino DHT11 monitor → ThingSpeak bridge")
    parser.add_argument("--port", help="Serial port (e.g. /dev/ttyACM0)")
    parser.add_argument("--live", action="store_true", help="Live reading only")
    parser.add_argument("--no-upload", action="store_true", help="Skip ThingSpeak upload")
    args = parser.parse_args()

    print("=== Env Monitor Bridge ===")
    print(f"  ThingSpeak channel: {CHANNEL_ID}")
    print(f"  Mode: {'live only' if args.live else 'dump buffer + live'}")
    print(f"  Upload: {'disabled' if args.no_upload else 'enabled'}")

    ser = open_serial(args.port)

    # ── Dump buffer ──
    readings = []
    if not args.live:
        print("\n  Requesting buffer dump...")
        lines = send_command(ser, b"D")
        readings = parse_dump(lines)
        print(f"  Received {len(readings)} historical readings")

    # ── Live reading ──
    print("\n  Requesting live reading...")
    lines = send_command(ser, b"L")
    live = parse_live(lines[0] if lines else "")
    if live:
        print(f"  Live: {live[0]:.1f}°C, {live[1]:.1f}%")
    else:
        print("  Live: no response")

    # ── Resume Arduino ──
    send_command(ser, b"Q")
    ser.close()

    # ── Save to CSV ──
    if readings or live:
        append_csv(readings, live)

    # ── Upload to ThingSpeak ──
    if not args.no_upload:
        print()
        upload_bulk(readings, live)

    # ── Summary ──
    print(f"\n  Done. {len(readings)} historical + {'1 live' if live else '0 live'} = {len(readings) + (1 if live else 0)} readings")
    thing_url = f"https://thingspeak.com/channels/{CHANNEL_ID}"
    print(f"  Dashboard: {thing_url}")


if __name__ == "__main__":
    main()
