"""Summarise the WSI present log (BC250_WSI_PRESENT_LOG) of one run.

Usage: python analyze-present-log.py <present.csv> [--bucket 30] [--json out.json]

Columns written by wsi_common_win32.cpp: chain,path,present_id,enter_us,copy_us,blit_us,dwmflush_us,result.
Header comment lines start with '#'. Frame time here is the interval between consecutive present entries of
the same chain (enter_us deltas); copy/blit/dwmflush are the parts of the present call itself. With
launch.json next to the CSV the bucket starts are given in UTC (enter_us is os_time_get_nano()/1000,
QueryPerformanceCounter based, same clock as qpc_start_ms in launch.json).
"""
import argparse, json, math, statistics
from collections import defaultdict
from datetime import datetime, timedelta, timezone
from pathlib import Path


def pct(s, p):
    if not s:
        return math.nan
    k = (len(s) - 1) * p / 100.0
    f = math.floor(k)
    c = min(f + 1, len(s) - 1)
    return s[f] + (s[c] - s[f]) * (k - f)


def stats(vals):
    vals = [v for v in vals if v is not None]
    if not vals:
        return {'n': 0}
    s = sorted(vals)
    return {'n': len(s), 'mean': statistics.fmean(s), 'median': pct(s, 50), 'p1': pct(s, 1), 'p5': pct(s, 5),
            'p95': pct(s, 95), 'p99': pct(s, 99), 'max': s[-1], 'min': s[0]}


def fmt(st):
    if st.get('n', 0) == 0:
        return 'n=0'
    return 'n={n} mean={mean:.2f} med={median:.2f} p5={p5:.2f} p95={p95:.2f} p99={p99:.2f} max={max:.2f} ms'.format(**st)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv')
    ap.add_argument('--bucket', type=float, default=30.0)
    ap.add_argument('--json')
    a = ap.parse_args()
    path = Path(a.csv)
    chains = defaultdict(list)
    headers = []
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        if not line or line.startswith('chain,'):
            continue
        if line.startswith('#'):
            headers.append(line)
            continue
        f = line.split(',')
        if len(f) < 8:
            continue
        chains[f[0]].append({'path': f[1], 'present_id': int(f[2]), 'enter_us': int(f[3]), 'copy_us': int(f[4]),
                             'blit_us': int(f[5]), 'dwm_us': int(f[6]), 'result': int(f[7])})
    launch = None
    lp = path.parent / 'launch.json'
    if lp.exists():
        launch = json.loads(lp.read_text(encoding='utf-8-sig'))

    def to_utc(us):
        if not launch or 'qpc_start_ms' not in launch:
            return None
        base = datetime.fromisoformat(launch['utc'].replace('Z', '+00:00'))
        return (base + timedelta(milliseconds=us / 1000.0 - float(launch['qpc_start_ms']))).astimezone(timezone.utc)

    summary = {'csv': str(path), 'headers': headers, 'chains': {}, 'buckets': []}
    for h in headers:
        print(h)
    for chain, rows in chains.items():
        rows.sort(key=lambda r: r['enter_us'])
        intervals = [(b['enter_us'] - a_['enter_us']) / 1000.0 for a_, b in zip(rows, rows[1:])]
        span = (rows[-1]['enter_us'] - rows[0]['enter_us']) / 1e6 if len(rows) > 1 else 0.0
        d = {
            'presents': len(rows), 'span_s': span, 'avg_fps': (len(rows) - 1) / span if span > 0 else None,
            'paths': sorted(set(r['path'] for r in rows)),
            'results': {str(k): sum(1 for r in rows if r['result'] == k) for k in set(r['result'] for r in rows)},
            'frame_interval_ms': stats(intervals),
            'copy_ms': stats([r['copy_us'] / 1000.0 for r in rows]),
            'blit_ms': stats([r['blit_us'] / 1000.0 for r in rows]),
            'dwmflush_ms': stats([r['dwm_us'] / 1000.0 for r in rows]),
            'present_call_ms': stats([(r['copy_us'] + r['blit_us'] + r['dwm_us']) / 1000.0 for r in rows]),
        }
        summary['chains'][chain] = d
        print(f'\nchain {chain} paths={d["paths"]} presents={len(rows)} span={span:.1f}s avg_fps={d["avg_fps"] or 0:.2f} results={d["results"]}')
        for k in ('frame_interval_ms', 'copy_ms', 'blit_ms', 'dwmflush_ms', 'present_call_ms'):
            print(f'  {k:<18}', fmt(d[k]))
    allrows = sorted((r for rows in chains.values() for r in rows), key=lambda r: r['enter_us'])
    if len(allrows) > 1:
        t0 = allrows[0]['enter_us']
        buckets = defaultdict(list)
        parts = defaultdict(lambda: defaultdict(list))
        prev = {}
        for r in allrows:
            b = int((r['enter_us'] - t0) / 1e6 // a.bucket)
            key = r['present_id'] and None
            if 'last' in prev:
                buckets[b].append((r['enter_us'] - prev['last']) / 1000.0)
            prev['last'] = r['enter_us']
            parts[b]['copy'].append(r['copy_us'] / 1000.0)
            parts[b]['blit'].append(r['blit_us'] / 1000.0)
            parts[b]['dwm'].append(r['dwm_us'] / 1000.0)
        print(f'\nper {a.bucket:.0f} s bucket (interval between presents, all chains):')
        for b in sorted(parts):
            st = stats(buckets.get(b, []))
            utc = to_utc(t0 + b * a.bucket * 1e6)
            row = {'bucket': b, 'start_s': b * a.bucket, 'utc': utc.isoformat(timespec='seconds') if utc else None,
                   'interval_ms': st, 'copy_ms': stats(parts[b]['copy']), 'blit_ms': stats(parts[b]['blit']),
                   'dwmflush_ms': stats(parts[b]['dwm'])}
            summary['buckets'].append(row)
            fps = 1000.0 / st['mean'] if st.get('n') and st['mean'] > 0 else 0.0
            print(f'  [{b * a.bucket:5.0f}s] {utc.strftime("%H:%M:%S") if utc else "        "} ' + fmt(st) +
                  f' ~{fps:.1f} fps | copy {row["copy_ms"].get("mean", 0):.2f} blit {row["blit_ms"].get("mean", 0):.2f} dwm {row["dwmflush_ms"].get("mean", 0):.2f} ms')
    if a.json:
        Path(a.json).write_text(json.dumps(summary, indent=1, default=str), encoding='utf-8')
        print('\nwritten', a.json)


if __name__ == '__main__':
    main()
