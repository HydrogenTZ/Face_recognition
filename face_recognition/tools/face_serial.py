"""Capture ESP32 console without toggling reset; optionally send newline commands."""
import argparse
import time
from pathlib import Path
import serial

parser = argparse.ArgumentParser()
parser.add_argument('--port', default='COM3')
parser.add_argument('--seconds', type=float, default=15)
parser.add_argument('--command', action='append', default=[])
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--reset', action='store_true')
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
port = serial.Serial(port=None, baudrate=115200, timeout=0.2)
port.dtr = False
port.rts = False
port.port = args.port
port.open()
with port, args.output.open('w', encoding='utf-8') as log:
    if args.reset:
        port.dtr = False
        port.rts = True
        time.sleep(0.1)
        port.rts = False
        time.sleep(0.1)
    started = time.monotonic()
    sent = False
    while time.monotonic() - started < args.seconds:
        if not sent and time.monotonic() - started > (8 if args.reset else 0.5):
            for command in args.command:
                port.write((command + '\n').encode('ascii'))
                time.sleep(0.15)
            sent = True
        data = port.read(port.in_waiting or 1)
        if data:
            text = data.decode('utf-8', errors='replace')
            print(text, end='', flush=True)
            log.write(text)
            log.flush()
