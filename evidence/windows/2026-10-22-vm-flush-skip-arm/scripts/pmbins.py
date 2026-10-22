"""pmbins.py <bin s> <run dir>...: align presentmon.csv with benchmarkResults/frames.csv (best match of the
frame-time sequence) and print per-bin fps, GPU busy per frame, GPU busy share and CPU busy per frame."""
import csv
import os
import sys

w = float(sys.argv[1])


def num(x):
    try:
        return float(x)
    except (ValueError, TypeError):
        return None


for d in sys.argv[2:]:
    bench = [float(r[1]) for r in list(csv.reader(open(os.path.join(d, 'benchmarkResults', 'frames.csv'))))[1:] if len(r) > 1]
    pm = [r for r in csv.DictReader(open(os.path.join(d, 'presentmon.csv'), encoding='utf-8-sig'))
          if r['Application'].lower().startswith('cyberpunk')]
    ft = [num(r['MsBetweenPresents']) or 0.0 for r in pm]
    k = min(300, len(bench))
    best = min(range(0, len(ft) - len(bench) + 1),
               key=lambda o: sum(abs(ft[o + i] - bench[i]) for i in range(k)))
    err = sum(abs(ft[best + i] - bench[i]) for i in range(k)) / k
    seg = pm[best:best + len(bench)]
    print(f'{os.path.basename(d)}: offset {best} of {len(pm)}, mean |dt| {err:.2f} ms over {k} frames')
    t, b = 0.0, []
    for r, f in zip(seg, bench):
        t += f / 1000
        i = int(t // w)
        while len(b) <= i:
            b.append([])
        b[i].append(r)
    for i, rows in enumerate(b):
        if not rows:
            continue
        fr = sum(num(r['MsBetweenPresents']) or 0 for r in rows)
        g = [num(r['MsGPUBusy']) for r in rows if num(r['MsGPUBusy']) is not None]
        c = [num(r['MsCPUBusy']) for r in rows if num(r['MsCPUBusy']) is not None]
        gl = [num(r['MsGPULatency']) for r in rows if num(r['MsGPULatency']) is not None]
        print(f'  {i * w:5.0f}s fps {1000 * len(rows) / fr:5.1f}  gpu busy/frame {sum(g) / len(g):5.2f} ms  share {sum(g) / fr:.3f}'
              f'  cpu busy/frame {sum(c) / len(c):5.2f} ms  gpu latency {sum(gl) / len(gl):5.2f}')
