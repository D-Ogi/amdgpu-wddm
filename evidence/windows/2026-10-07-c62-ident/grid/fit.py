"""C62 sweep analysis: per cell, the series (t, GPU C, Tctl, GPU MHz, SMU W, fan rpm), a summary (mean clock over the
trial and over its last 30 s, time to the first clock step below 1500 MHz, peak temperatures, mean SMU and wall W),
and a first-order thermal fit per cell: dT/dt = (k*P - (T - T0)) / tau over the samples before the first clock step
(the open-loop part), least squares on (k, tau). Writes fit.md and fit.json next to the runs."""
import json, pathlib, re, statistics as st

HERE = pathlib.Path(__file__).resolve().parent
RUNS = HERE / 'runs'


def parse(path):
    rows, cur = [], None
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        m = re.match(r't_ms=(\d+)', line)
        if m:
            cur = {'t': int(m.group(1)) / 1000.0}
            rows.append(cur)
            continue
        if cur is None:
            continue
        if line.startswith('dpm version'):
            for k, f in (('gpu_c', 'temperature_c'), ('mhz', 'gfx_mhz'), ('w', 'power_w')):
                mm = re.search(f + r'=([\d.]+)', line)
                if mm:
                    cur[k] = float(mm.group(1))
        elif line.startswith('fan fan='):
            for k, f in (('rpm', 'rpm'), ('tctl', 'tsi_c')):
                mm = re.search(r'\b' + f + r'=([\d.]+)', line)
                if mm:
                    cur[k] = float(mm.group(1))
    return [r for r in rows if 'gpu_c' in r and 'mhz' in r]


def plug(path):
    out = []
    if path.exists():
        for line in path.read_text(encoding='utf-8').splitlines():
            try:
                o = json.loads(json.loads(line).get('out') or '{}')
                if o.get('power_w') is not None:
                    out.append(o['power_w'])
            except Exception:
                pass
    return out


def fit(rows):
    """Two-node model. The die follows the sink within one sample: T_die = T_sink + R1 * P, R1 being the slope of the
    temperature step against the power step between adjacent samples where the power changed by 8 W or more (the
    DPM clock steps). The sink is first order: dT_sink/dt = a * P - b * T_sink + c, so tau = 1 / b and R2 = a / b
    (C/W, sink to air, fan included)."""
    steps = [((q['gpu_c'] - p['gpu_c']), (q['w'] - p['w'])) for p, q in zip(rows, rows[1:]) if abs(q['w'] - p['w']) >= 8]
    if len(steps) < 2:
        return {'note': 'fewer than two power steps of 8 W, R1 unknown'}
    r1 = sum(dt * dp for dt, dp in steps) / sum(dp * dp for _, dp in steps)
    out = {'r1_c_per_w': round(r1, 4), 'steps': len(steps)}
    X, y = [], []
    for p, q in zip(rows, rows[1:]):
        if q['t'] > p['t']:
            sp, sq = p['gpu_c'] - r1 * p['w'], q['gpu_c'] - r1 * q['w']
            X.append((p['w'], sp, 1.0))
            y.append((sq - sp) / (q['t'] - p['t']))
    n = len(X)
    if n < 6:
        return out
    # Normal equations, 3x3, solved by Cramer's rule (no numpy on purpose).
    A = [[sum(X[i][r] * X[i][c] for i in range(n)) for c in range(3)] for r in range(3)]
    B = [sum(X[i][r] * y[i] for i in range(n)) for r in range(3)]

    def det(M):
        return (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
                + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]))
    D = det(A)
    if abs(D) < 1e-9:
        return out
    sol = []
    for c in range(3):
        M = [row[:] for row in A]
        for r in range(3):
            M[r][c] = B[r]
        sol.append(det(M) / D)
    a, mb, c = sol
    b = -mb
    out.update({'n': n, 'a': a, 'b': b, 'c': c})
    if a > 0 and b > 0:
        out.update({'tau_s': 1 / b, 'r2_c_per_w': a / b, 't_air_c': c / b})
    return out


def main():
    res = {}
    for p in sorted(RUNS.glob('*.txt')):
        tag = p.stem
        rows = parse(p)
        if not rows:
            continue
        stop = next((l for l in p.read_text(encoding='utf-8', errors='replace').splitlines() if l.startswith('stop=')), '')
        t_end = rows[-1]['t']
        last = [r for r in rows if r['t'] >= t_end - 30]
        step = next((r['t'] for r in rows if r['mhz'] < 1500), None)
        wall = plug(RUNS / f'{tag}.plug.jsonl')
        res[tag] = {
            'samples': len(rows), 'stop': stop,
            'mhz_mean': round(st.mean(r['mhz'] for r in rows)), 'mhz_last30': round(st.mean(r['mhz'] for r in last)),
            'first_step_s': step, 'gpu_c_max': max(r['gpu_c'] for r in rows),
            'tctl_max': max((r.get('tctl', 0) for r in rows)), 'smu_w_mean': round(st.mean(r['w'] for r in rows), 1),
            'smu_w_last30': round(st.mean(r['w'] for r in last), 1),
            'rpm_mean': round(st.mean(r.get('rpm', 0) for r in rows)),
            'wall_w_mean': round(st.mean(wall), 1) if wall else None, 'wall_w_max': max(wall) if wall else None,
            'fit': fit(rows),
        }
    (HERE / 'fit.json').write_text(json.dumps(res, indent=1), encoding='utf-8')
    lines = ['| cell | MHz mean | MHz last 30 s | first step s | GPU C max | Tctl max | SMU W | SMU W last 30 s | rpm | wall W | R1 C/W | R2 C/W | tau s | stop |',
             '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    for tag, r in res.items():
        f = r['fit'] or {}
        lines.append(f"| {tag} | {r['mhz_mean']} | {r['mhz_last30']} | {r['first_step_s']} | {r['gpu_c_max']} | {r['tctl_max']} | "
                     f"{r['smu_w_mean']} | {r['smu_w_last30']} | {r['rpm_mean']} | {r['wall_w_mean']} | "
                     f"{f.get('r1_c_per_w', '-')} | {round(f['r2_c_per_w'], 3) if 'r2_c_per_w' in f else '-'} | "
                     f"{round(f['tau_s']) if 'tau_s' in f else '-'} | {r['stop']} |")
    (HERE / 'fit.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print('\n'.join(lines))


if __name__ == '__main__':
    main()
