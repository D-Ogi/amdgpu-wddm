"""Assemble evidence/windows/2026-09-27-E45-wsi-kmt-present-plumbing from the wsi-kmt runs.

Screenshots stay private (hashes only). KMD logs are converted from UTF-16 to UTF-8. vkcube stderr
keeps only the winsys lines (the IB dumps of BC250_TRACE_SUBMITS are 1-2 MB of noise).
"""
import csv
import hashlib
import json
import pathlib
import re
import shutil
import statistics

S = pathlib.Path(r'P:\bc-250\scratch\wsi-kmt-2026-09-27')
E = pathlib.Path(r'P:\bc-250\bc250-win\evidence\windows\2026-09-27-E45-wsi-kmt-present-plumbing')
E.mkdir(parents=True, exist_ok=True)
(E / 'scripts').mkdir(exist_ok=True)
(E / 'patches').mkdir(exist_ok=True)


def text(p):
    b = pathlib.Path(p).read_bytes()
    return b.decode('utf-16') if b[:2] in (b'\xff\xfe', b'\xfe\xff') else b.decode('utf-8', 'replace')


def put_text(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(text(src), encoding='utf-8', newline='\n')


for n in ['labcheck.ps1', 'run-kmt-002.ps1', 'run-kmt-003.ps1', 'worker-kmt-003.ps1', 'launch-kmt-003.ps1',
          'status-kmt-003.ps1', 'cleanup-kmt-003.ps1', 'make002.py', 'make003.py', 'pull-003.py', 'finish-e45.py']:
    shutil.copyfile(S / n, E / 'scripts' / n)

stats = {}
for run in ('run001', 'run002', 'run003'):
    r = S / run
    d = E / run
    d.mkdir(exist_ok=True)
    put_text(r / f'run-kmt-{run[3:]}.log', d / f'run-kmt-{run[3:]}.log')
    for n in ['done-kmt-%s.json' % run[3:], 'modules-A-kmt.json', 'modules-B-cpu.json', 'before-health.txt',
              'after-health.txt', 'before-clock.txt', 'after-clock.txt', 'logman-stop.txt']:
        if (r / n).exists():
            put_text(r / n, d / n)
    for n in ['before-log-summary.txt', 'after-A-kmt-log-summary.txt', 'after-B-cpu-log-summary.txt', 'after-log-summary.txt']:
        if (r / n).exists():
            t = text(r / n)
            keep = [l for l in t.splitlines() if re.search(r'wddm summary: (blit|last blit|widest|presents|Present )', l)]
            (d / n.replace('.txt', '.blit-lines.txt')).write_text('\n'.join(keep) + '\n', encoding='utf-8')
    for stage in ('A-kmt', 'B-cpu'):
        src = r / f'vkcube-{stage}.stderr.txt'
        if src.exists():
            lines = [l for l in src.read_text(errors='replace').splitlines() if 'wddm2:' in l or 'radv_wddm2_winsys' in l]
            (d / f'vkcube-{stage}.stderr.wddm2-lines.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
        p = r / f'present-{stage}.csv'
        if p.exists() and p.stat().st_size:
            shutil.copyfile(p, d / p.name)
            rows = [l.strip().split(',') for l in p.read_text().splitlines()
                    if l.strip() and not l.startswith('#') and not l.startswith('chain,')]
            headers = [l for l in p.read_text().splitlines() if l.startswith('#')]
            enter = [int(x[3]) for x in rows]
            iv = [b - a for a, b in zip(enter, enter[1:]) if 0 < b - a < 1_000_000]
            col = lambda i: [int(x[i]) for x in rows]
            def q(v, f):
                v = sorted(v)
                return v[min(len(v) - 1, int(len(v) * f))] if v else None
            stats[f'{run}/{stage}'] = {
                'presents': len(rows),
                'chain_headers': len(headers),
                'distinct_chains': len({x[0] for x in rows}),
                'results': {k: sum(1 for x in rows if x[-1] == k) for k in sorted({x[-1] for x in rows})},
                'interval_us': {'median': statistics.median(iv), 'p95': q(iv, .95)} if iv else None,
                'copy_us_median': statistics.median(col(4)),
                'blit_us': {'median': statistics.median(col(5)), 'p95': q(col(5), .95), 'max': max(col(5))},
                'dwmflush_us': {'median': statistics.median(col(6)), 'p95': q(col(6), .95)},
                'span_s': round((enter[-1] - enter[0]) / 1e6, 2),
            }
(E / 'present-stats.json').write_text(json.dumps(stats, indent=1), encoding='utf-8')

caps = {}
for run in ('run001', 'run002', 'run003'):
    for p in sorted((S / run).glob('*.png')):
        caps[f'{run}/{p.name}'] = hashlib.sha256(p.read_bytes()).hexdigest().upper()
caps['run003-live-1.png (half scale, during stage B)'] = hashlib.sha256((S / 'run003-live-1.png').read_bytes()).hexdigest().upper()
(E / 'capture-hashes.json').write_text(json.dumps(caps, indent=2), encoding='utf-8')
print(json.dumps(stats, indent=1))
