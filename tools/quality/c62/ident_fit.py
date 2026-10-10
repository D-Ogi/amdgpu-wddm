"""C62 open-loop identification fit (ident/fan*.txt from c62-ident.ps1): the reproducer.

The copy in evidence/windows/2026-10-07-c62-ident/ident/ident-fit.py is the program of the round, and it stays
as the round published it, hashes and all (CLAUDE.md rule 6, and the package's own sha256.txt covers it). That
copy looks for its input in its own folder plus 'ident' again, which the published layout does not have, so it
runs to an empty result and writes its output back into evidence/ (audit finding F2, 2026-10-10). This copy is
the same arithmetic with the input and the output as arguments, and tools/quality/test_c62_fit.py runs it
against the staged evidence tree and compares the result with the recorded one, number for number.

One node per sensor: dT/dt = a * P_gpu + c * cpu_on - b * (T - T_idle), least squares over the 2 s samples.
P_gpu is the KMD power_w (socket GPU rail). cpu_on is 1 while the 4 CPU threads run (the CPU power is not read
per sample; the plug delta gives its size). Also prints the heating rate per phase and the plug watts.

Nothing is written unless --out names a file, so a run cannot touch the evidence tree.

    python tools/quality/c62/ident_fit.py --in evidence/windows/2026-10-07-c62-ident/ident
    python tools/quality/c62/ident_fit.py --in <folder> --out <file.json>
"""
import argparse, json, re
from pathlib import Path

import numpy as np

ap = argparse.ArgumentParser(description='C62 open-loop identification fit')
ap.add_argument('--in', dest='indir',
                default=str(Path(__file__).resolve().parents[3] / 'evidence' / 'windows' /
                            '2026-10-07-c62-ident' / 'ident'),
                help='folder with fan*.txt and their .plug.jsonl (default: the staged C62 package)')
ap.add_argument('--out', dest='outfile', default=None, help='write the JSON here as well as to stdout')
args = ap.parse_args()

here = Path(args.indir)
out = {}
for f in sorted(here.glob('fan*.txt')):
    rows, t, ph = [], None, None
    for line in f.read_text(errors='replace').splitlines():
        m = re.match(r't_ms=(\d+) phase=(\S+)', line)
        if m:
            t, ph = int(m[1]) / 1000, m[2]; cur = {'t': t, 'phase': ph}; rows.append(cur); continue
        if t is None:
            continue
        for key, pat in (('gpu_c', r'^dpm .*temperature_c=([\d.]+)'), ('mhz', r'^dpm .*gfx_mhz=(\d+)'),
                         ('p', r'^dpm .*power_w=([\d.]+)'), ('tctl', r'^fan .*tsi_c=([\d.]+)'),
                         ('rpm', r'^fan .*rpm=(\d+)')):
            mm = re.search(pat, line)
            if mm:
                rows[-1][key] = float(mm[1])
    rows = [r for r in rows if all(k in r for k in ('gpu_c', 'p', 'tctl'))]
    tt = np.array([r['t'] for r in rows]); P = np.array([r['p'] for r in rows])
    cpu = np.array([1.0 if r['phase'] in ('gpu+cpu', 'cpu') else 0.0 for r in rows])
    res = {'samples': len(rows), 'end_t': float(tt[-1]), 'phases': {}}
    for ph in dict.fromkeys(r['phase'] for r in rows):
        sel = [r for r in rows if r['phase'] == ph]
        if len(sel) < 2:
            continue
        dt = sel[-1]['t'] - sel[0]['t']
        res['phases'][ph] = {
            'n': len(sel), 'span_s': round(dt, 1), 'p_w_mean': round(float(np.mean([r['p'] for r in sel])), 1),
            'mhz': sorted({int(r['mhz']) for r in sel}),
            'gpu_c': [sel[0]['gpu_c'], sel[-1]['gpu_c']], 'tctl': [sel[0]['tctl'], sel[-1]['tctl']],
            'gpu_c_per_s': round((sel[-1]['gpu_c'] - sel[0]['gpu_c']) / dt, 3) if dt else None,
            'tctl_per_s': round((sel[-1]['tctl'] - sel[0]['tctl']) / dt, 3) if dt else None,
            'rpm': sorted({int(r.get('rpm', 0)) for r in sel})}
    for key in ('gpu_c', 'tctl'):
        T = np.array([r[key] for r in rows])
        T0 = float(np.mean(T[:5]))
        dT = np.gradient(T, tt)
        A = np.column_stack([P - P[:5].mean(), cpu, -(T - T0)])
        coef, *_ = np.linalg.lstsq(A, dT, rcond=None)
        pred = A @ coef
        r2 = 1 - np.sum((dT - pred) ** 2) / np.sum((dT - dT.mean()) ** 2)
        a, c, b = coef
        res[key] = {'T_idle': round(T0, 1), 'a_C_per_s_per_W': round(float(a), 4), 'cpu_C_per_s': round(float(c), 3),
                    'b_per_s': round(float(b), 4), 'tau_s': round(1 / b, 1) if b > 0 else None,
                    'R_C_per_W': round(float(a / b), 3) if b > 0 else None,
                    'cpu_equiv_W': round(float(c / a), 1) if a else None, 'R2_dTdt': round(float(r2), 3)}
    plug = Path(str(f).replace('.txt', '.plug.jsonl'))
    if plug.exists():
        ws = []
        for line in plug.read_text().splitlines():
            m = re.search(r"'19': (\d+)", line) or re.search(r'"19": (\d+)', line)
            if m:
                ws.append(int(m[1]) / 10)
        res['plug_w'] = ws
    out[f.stem] = res
print(json.dumps(out, indent=1))
if args.outfile:
    Path(args.outfile).write_text(json.dumps(out, indent=1))
