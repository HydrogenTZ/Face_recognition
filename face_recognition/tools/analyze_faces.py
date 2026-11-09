"""Extract raw FACE_CSV rows and FACE_STATS from a captured UART log."""
import argparse
import csv
import json
import re
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('log', type=Path)
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
text = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', args.log.read_text(encoding='utf-8', errors='replace'))
blocks = re.findall(r'FACE_CSV_BEGIN\s*\n(.*?)FACE_CSV_END', text, re.S)
if not blocks:
    raise SystemExit('No complete CSV block. Send dump after the benchmark, capture it, then retry.')
lines = [line.strip() for line in blocks[-1].splitlines()]
lines = [line for line in lines if line.startswith('frame,') or re.fullmatch(r'\d+,\d+,-?\d+\.\d+(?:,\d+){5}', line)]
rows = list(csv.DictReader(lines))
if not rows:
    raise SystemExit('CSV block has no samples')
args.out.mkdir(parents=True, exist_ok=True)
with (args.out / 'samples.csv').open('w', encoding='utf-8', newline='') as f:
    writer = csv.DictWriter(f, fieldnames=rows[0].keys())
    writer.writeheader()
    writer.writerows(rows)
values = sorted(int(row['algorithm_us']) for row in rows)
n = len(values)
summary = {
    'samples': n,
    'algorithm_mean_ms': sum(values) / n / 1000,
    'algorithm_median_ms': (values[n // 2] if n % 2 else (values[n // 2 - 1] + values[n // 2]) / 2) / 1000,
    'algorithm_p95_ms': values[(95 * n + 99) // 100 - 1] / 1000,
    'unknown_samples': sum(int(row['id']) == 0 for row in rows),
    'device_stats': re.findall(r'^FACE_STATS .*$', text, re.M)[-1:],
}
for field in ('detect_us', 'feature_us', 'match_us', 'pipeline_us'):
    summary[field.replace('_us', '_mean_ms')] = sum(int(row[field]) for row in rows) / n / 1000
(args.out / 'summary.json').write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding='utf-8')
print(json.dumps(summary, indent=2, ensure_ascii=False))
