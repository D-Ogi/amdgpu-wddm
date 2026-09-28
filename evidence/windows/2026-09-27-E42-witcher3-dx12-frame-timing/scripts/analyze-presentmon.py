"""Summarise a PresentMon 2.x CSV (console build, default metrics) for one run.

Usage: python analyze-presentmon.py <run dir> [--bucket 30] [--json out.json]

Prints per swap chain: presents, span, frame time (MsBetweenPresents) mean/median/p1/p5/p95/p99/max, the
GPU columns when present, PresentMode/SyncInterval/PresentRuntime distributions, dropped frames; then the
same frame-time statistics per time bucket so that menu, loading and scene phases can be told apart. With
launch.json (qpc_start_ms, utc) next to the CSV, bucket starts are also given in UTC, so the input log can
be lined up. Statistics only; no claim about what the phases were - that comes from the screenshots.
"""
import argparse, csv, json, math, statistics, sys
from collections import Counter, defaultdict
from datetime import datetime, timedelta, timezone
from pathlib import Path


def pct(sorted_vals, p):
    if not sorted_vals:
        return math.nan
    k = (len(sorted_vals) - 1) * p / 100.0
    f = math.floor(k)
    c = min(f + 1, len(sorted_vals) - 1)
    return sorted_vals[f] + (sorted_vals[c] - sorted_vals[f]) * (k - f)


def stats(vals):
    vals = [v for v in vals if v is not None and not math.isnan(v)]
    if not vals:
        return {'n': 0}
    s = sorted(vals)
    return {'n': len(s), 'mean': statistics.fmean(s), 'median': pct(s, 50), 'p1': pct(s, 1), 'p5': pct(s, 5),
            'p95': pct(s, 95), 'p99': pct(s, 99), 'max': s[-1], 'min': s[0]}


def fnum(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def fmt(st, unit='ms'):
    if st.get('n', 0) == 0:
        return 'n=0'
    return ('n={n} mean={mean:.2f} med={median:.2f} p1={p1:.2f} p5={p5:.2f} p95={p95:.2f} p99={p99:.2f} '
            'max={max:.2f} {u}').format(u=unit, **st)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run_dir')
    ap.add_argument('--bucket', type=float, default=30.0, help='bucket length in seconds')
    ap.add_argument('--json', help='write the summary as JSON here')
    a = ap.parse_args()
    run = Path(a.run_dir)
    csv_path = run / 'presentmon.csv'
    rows = list(csv.DictReader(csv_path.open(encoding='utf-8-sig', newline='')))
    if not rows:
        print('empty CSV'); sys.exit(1)
    cols = list(rows[0].keys())
    print('columns:', ', '.join(cols))
    tcol = next((c for c in ('CPUStartQPCTime', 'CPUStartTime', 'CPUStartQPC', 'TimeInSeconds') if c in cols), None)
    ftcol = next((c for c in ('MsBetweenPresents', 'FrameTime', 'MsCPUBusy') if c in cols), None)
    print('time column:', tcol, ' frame-time column:', ftcol)

    launch = None
    lp = run / 'launch.json'
    if lp.exists():
        launch = json.loads(lp.read_text(encoding='utf-8-sig'))

    def to_utc(t_ms):
        if not launch or tcol != 'CPUStartQPCTime' or 'qpc_start_ms' not in launch:
            return None
        base = datetime.fromisoformat(launch['utc'].replace('Z', '+00:00'))
        return (base + timedelta(milliseconds=t_ms - float(launch['qpc_start_ms']))).astimezone(timezone.utc)

    summary = {'csv': str(csv_path), 'rows': len(rows), 'columns': cols, 'time_column': tcol, 'frame_time_column': ftcol,
               'swapchains': {}, 'buckets': []}
    by_sc = defaultdict(list)
    for r in rows:
        by_sc[(r.get('ProcessID'), r.get('SwapChainAddress'))].append(r)
    for (pid, sc), rs in by_sc.items():
        ft = [fnum(r.get(ftcol)) for r in rs] if ftcol else []
        t = [fnum(r.get(tcol)) for r in rs] if tcol else []
        t = [x for x in t if x is not None]
        span_s = (max(t) - min(t)) / (1000.0 if tcol != 'TimeInSeconds' else 1.0) if len(t) > 1 else 0.0
        d = {
            'pid': pid, 'swapchain': sc, 'presents': len(rs), 'span_s': span_s,
            'avg_fps_over_span': (len(rs) - 1) / span_s if span_s > 0 else None,
            'frame_time_ms': stats(ft),
            'present_mode': dict(Counter(r.get('PresentMode') for r in rs)),
            'sync_interval': dict(Counter(r.get('SyncInterval') for r in rs)),
            'runtime': dict(Counter(r.get('PresentRuntime') for r in rs)),
            'allows_tearing': dict(Counter(r.get('AllowsTearing') for r in rs)),
            'dropped_not_displayed': sum(1 for r in rs if r.get('DisplayedTime') in ('NA', '', None)) if 'DisplayedTime' in cols else None,
        }
        for c in ('MsGPUTime', 'MsGPUBusy', 'MsGPUWait', 'MsGPULatency', 'MsCPUBusy', 'MsCPUWait', 'MsInPresentAPI',
                  'MsBetweenDisplayChange', 'MsUntilDisplayed', 'MsRenderPresentLatency', 'DisplayLatency', 'DisplayedTime'):
            if c in cols:
                d[c] = stats([fnum(r.get(c)) for r in rs])
        summary['swapchains'][f'{pid}/{sc}'] = d
        print(f'\nswapchain pid={pid} {sc}: presents={len(rs)} span={span_s:.1f}s avg_fps={d["avg_fps_over_span"] or 0:.2f}')
        print('  frame time     ', fmt(d['frame_time_ms']))
        for c in ('MsGPUTime', 'MsGPUBusy', 'MsCPUBusy', 'MsInPresentAPI', 'MsBetweenDisplayChange', 'DisplayLatency'):
            if c in d:
                print(f'  {c:<15}', fmt(d[c]))
        print('  present mode   ', d['present_mode'])
        print('  sync interval  ', d['sync_interval'], ' runtime', d['runtime'], ' tearing', d['allows_tearing'])
        if d['dropped_not_displayed'] is not None:
            print('  not displayed  ', d['dropped_not_displayed'])

    if tcol and ftcol:
        t0 = min(fnum(r.get(tcol)) for r in rows if fnum(r.get(tcol)) is not None)
        scale = 1.0 if tcol == 'TimeInSeconds' else 1000.0
        buckets = defaultdict(list)
        gpu = defaultdict(list)
        for r in rows:
            tv = fnum(r.get(tcol))
            if tv is None:
                continue
            b = int((tv - t0) / scale // a.bucket)
            buckets[b].append(fnum(r.get(ftcol)))
            if 'MsGPUTime' in cols:
                gpu[b].append(fnum(r.get('MsGPUTime')))
        print(f'\nper {a.bucket:.0f} s bucket (from first present):')
        for b in sorted(buckets):
            st = stats(buckets[b])
            start_ms = t0 + b * a.bucket * scale
            utc = to_utc(start_ms) if scale == 1000.0 else None
            g = stats(gpu[b]) if gpu.get(b) else None
            row = {'bucket': b, 'start_s': b * a.bucket, 'utc': utc.isoformat(timespec='seconds') if utc else None,
                   'frame_time_ms': st, 'gpu_time_ms': g}
            summary['buckets'].append(row)
            fps = 1000.0 / st['mean'] if st.get('n') and st['mean'] > 0 else 0.0
            line = f'  [{b * a.bucket:5.0f}s] {utc.strftime("%H:%M:%S") if utc else "        "} ' + fmt(st) + f' ~{fps:.1f} fps'
            if g and g.get('n'):
                line += f' | gpu mean={g["mean"]:.2f} p95={g["p95"]:.2f}'
            print(line)
    if a.json:
        Path(a.json).write_text(json.dumps(summary, indent=1, default=str), encoding='utf-8')
        print('\nwritten', a.json)


if __name__ == '__main__':
    main()
