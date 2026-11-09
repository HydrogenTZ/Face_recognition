"""Decode the board's RGB565 hexadecimal diagnostic snapshot using stdlib only."""
import argparse
import re
import struct
import zlib
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('log', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
text = args.log.read_text(encoding='utf-8', errors='replace')
blocks = list(re.finditer(r'FACE_SNAPSHOT_BEGIN width=(\d+) height=(\d+) faces=(\d+)\s*\n(.*?)FACE_SNAPSHOT_END', text, re.S))
if not blocks:
    raise SystemExit('No complete snapshot in log')
block = blocks[-1]
width, height = int(block[1]), int(block[2])
parts = []
for line in block[4].splitlines():
    match = re.match(r'^([0-9a-f]+)(?:E \(|$)', line)
    if match:
        parts.append(match[1])
pixels_hex = ''.join(parts)
assert len(pixels_hex) == width * height * 4, (len(pixels_hex), width * height * 4)
rows = [pixels_hex[y * width * 4:(y + 1) * width * 4] for y in range(height)]
raw = bytearray()
for row in rows:
    raw.append(0)
    for x in range(width):
        value = int(row[4*x:4*x+4], 16)
        r, g, b = (value >> 11) & 31, (value >> 5) & 63, value & 31
        raw.extend(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)

png = b'\x89PNG\r\n\x1a\n'
png += chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_bytes(png)
print(f'Saved {width}x{height}; detected faces={block[3]}: {args.output}')
