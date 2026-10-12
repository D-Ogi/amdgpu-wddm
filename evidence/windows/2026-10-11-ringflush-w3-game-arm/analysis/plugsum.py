"""Plug watts of one session log (plug-sample.sh): sample count, span, maximum, and the samples inside a UTC span.

    python -P plugsum.py LOG [HH:MM:SS-HH:MM:SS]"""
import json
import statistics
import sys

ws = []
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    line = line.strip()
    if line.startswith('{'):
        d = json.loads(line)
        if d.get('power_w') is not None:
            ws.append((d['queried_utc'][11:19], d['power_w']))
print(f'{len(ws)} samples {ws[0][0]}-{ws[-1][0]} max {max(w for _, w in ws):.1f} W')
if len(sys.argv) > 2:
    a, b = sys.argv[2].split('-')
    win = [(t, w) for t, w in ws if a <= t <= b]
    print(f'window {a}-{b}: {len(win)} samples, mean {statistics.mean(w for _, w in win):.1f} W, '
          f'max {max(w for _, w in win):.1f} W: ' + ', '.join(f'{t} {w:.0f}' for t, w in win))
