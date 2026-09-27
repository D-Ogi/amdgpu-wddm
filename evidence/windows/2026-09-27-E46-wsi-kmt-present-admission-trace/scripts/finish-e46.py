"""Assemble evidence/windows/2026-09-27-E46-wsi-kmt-present-admission-trace from wsi-kmt-004.

Screenshots and the raw 367 MB DxgKrnl trace stay private (hashes only). KMD logs are converted from
UTF-16 to UTF-8 and reduced to the blit lines; vkcube stderr keeps only the winsys lines.
"""
import hashlib
import json
import pathlib
import re
import shutil
import statistics

S = pathlib.Path(r'P:\bc-250\scratch\wsi-kmt-2026-09-27')
E = pathlib.Path(r'P:\bc-250\bc250-win\evidence\windows\2026-09-27-E46-wsi-kmt-present-admission-trace')
E.mkdir(parents=True, exist_ok=True)
(E / 'scripts').mkdir(exist_ok=True)
(E / 'patches').mkdir(exist_ok=True)
(E / 'run004').mkdir(exist_ok=True)


def text(p):
    b = pathlib.Path(p).read_bytes()
    return b.decode('utf-16') if b[:2] in (b'\xff\xfe', b'\xfe\xff') else b.decode('utf-8', 'replace')


def put_text(src, dst):
    dst.write_text(text(src), encoding='utf-8', newline='\n')


for n in ['run-kmt-004.ps1', 'worker-kmt-004.ps1', 'launch-kmt-004.ps1', 'status-kmt-004.ps1', 'cleanup-kmt-004.ps1',
          'makenext.py', 'pull-004.py', 'decode-004.ps1', 'finish-e46.py', 'labcheck.ps1']:
    shutil.copyfile(S / n, E / 'scripts' / n)

r = S / 'run004'
d = E / 'run004'
put_text(r / 'run-kmt-004.log', d / 'run-kmt-004.log')
for n in ['done-kmt-004.json', 'modules-A-kmt.json', 'modules-B-cpu.json', 'before-health.txt', 'after-health.txt',
          'before-clock.txt', 'after-clock.txt', 'logman-stop.txt', 'logman-start-kmt004.txt', 'logman-query-kmt004.txt',
          'logman-stop-kmt004.txt', 'decode.log']:
    if (r / n).exists():
        put_text(r / n, d / n)
for n in ['dxgkrnl-present-events.csv', 'dxgkrnl-presenthistory-events.csv', 'present-A-kmt.csv', 'present-B-cpu.csv']:
    shutil.copyfile(r / n, d / n)
for n in ['before-log-summary.txt', 'after-A-kmt-log-summary.txt', 'after-B-cpu-log-summary.txt', 'after-log-summary.txt']:
    t = text(r / n)
    keep = [l for l in t.splitlines() if re.search(r'wddm summary: (blit|last blit|widest|presents|Present )', l)]
    (d / n.replace('.txt', '.blit-lines.txt')).write_text('\n'.join(keep) + '\n', encoding='utf-8')
for stage in ('A-kmt', 'B-cpu'):
    lines = [l for l in (r / f'vkcube-{stage}.stderr.txt').read_text(errors='replace').splitlines()
             if 'wddm2:' in l or 'radv_wddm2_winsys' in l]
    (d / f'vkcube-{stage}.stderr.wddm2-lines.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')

stats = {}
for stage in ('A-kmt', 'B-cpu'):
    p = r / f'present-{stage}.csv'
    rows = [l.strip().split(',') for l in p.read_text().splitlines()
            if l.strip() and not l.startswith('#') and not l.startswith('chain,')]
    enter = [int(x[3]) for x in rows]
    iv = [b - a for a, b in zip(enter, enter[1:]) if 0 < b - a < 1_000_000]
    col = lambda i: [int(x[i]) for x in rows]
    q = lambda v, f: sorted(v)[min(len(v) - 1, int(len(v) * f))]
    stats[stage] = {
        'presents': len(rows), 'distinct_chains': len({x[0] for x in rows}),
        'results': {k: sum(1 for x in rows if x[-1] == k) for k in sorted({x[-1] for x in rows})},
        'interval_us': {'median': statistics.median(iv), 'p95': q(iv, .95)},
        'copy_us_median': statistics.median(col(4)),
        'blit_us': {'median': statistics.median(col(5)), 'p95': q(col(5), .95), 'max': max(col(5))},
        'dwmflush_us': {'median': statistics.median(col(6)), 'p95': q(col(6), .95)},
        'span_s': round((enter[-1] - enter[0]) / 1e6, 2),
    }
(E / 'present-stats.json').write_text(json.dumps(stats, indent=1), encoding='utf-8')

caps = {}
for p in sorted(r.glob('*.png')):
    caps[f'run004/{p.name}'] = hashlib.sha256(p.read_bytes()).hexdigest().upper()
caps['run004/dxgkrnl.etl (raw DxgKrnl trace, 367001600 bytes, private)'] = hashlib.sha256((r / 'dxgkrnl.etl').read_bytes()).hexdigest().upper()
(E / 'capture-hashes.json').write_text(json.dumps(caps, indent=2), encoding='utf-8')
print(json.dumps(stats, indent=1))
