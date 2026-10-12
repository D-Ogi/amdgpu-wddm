"""Window B of one W3 session of the EnableRingVmFlush game arm: present rate, frame intervals, GPU clock and the
rate per GHz, for the whole window and for four equal sub-windows (the spread inside one session).

    python analyze.py N [N ...]

Inputs (all written by run-m157.sh): scratch/m15/etw/N/gpu-B-dump.txt and etw-notes.txt (the DxgKrnl capture of
window B), scratch/dpm/dpm-game-N.txt (the 1 s KMD DPM sampler; its clock is the lab's local time, UTC+2 on this
date, which is checked against the ETW window by the busy column). The present statistics come from
scratch/m15/game-recon/etw/etw-present-windows.py, the same tool the train validations use."""
import datetime as dt
import re
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path('P:/BC-250')
PW = ROOT / 'scratch/m15/game-recon/etw/etw-present-windows.py'
LOCAL = dt.timedelta(hours=2)  # CEST on 2026-10-11


def notes_window(notes):
    text = notes.read_text(encoding='utf-8', errors='replace')
    zero = re.search(r'^(\S+)Z .*window B gpu session \S+ start exited', text, re.M)
    held = re.search(r'window B gpu-only held ([0-9.]+) s', text)
    trig = re.search(r'window B trigger (.*)$', text, re.M)
    t0 = dt.datetime.fromisoformat(zero.group(1)[:26])
    return t0, float(held.group(1)) if held else None, trig.group(1).strip() if trig else '?'


def run_windows(dump, notes, spans):
    args = [sys.executable, str(PW), str(dump), str(notes)]
    args += [f'{name}={a:%H:%M:%S}-{b:%H:%M:%S}' for name, a, b in spans]
    return subprocess.run(args, capture_output=True, text=True).stdout


def parse(out):
    rows, cur = {}, None
    for line in out.splitlines():
        m = re.match(r'== (\S+) \(([0-9.]+)\.\.([0-9.]+) s, (\d+) s\): GPU busy \(DMA union\) ([0-9.]+) %', line)
        if m:
            cur = m.group(1)
            rows[cur] = {'from_s': float(m.group(2)), 'to_s': float(m.group(3)), 'busy_dma': float(m.group(5))}
            continue
        m = re.search(r'n=\s*(\d+) rate=\s*([0-9.]+)/s median=\s*([0-9.]+) ms p95=\s*([0-9.]+) p99=\s*([0-9.]+) '
                      r'max=\s*([0-9.]+)', line)
        if m and cur and 'n' not in rows[cur] and int(m.group(1)) > 50:
            rows[cur].update(n=int(m.group(1)), rate=float(m.group(2)), p50=float(m.group(3)),
                             p95=float(m.group(4)), p99=float(m.group(5)), max=float(m.group(6)))
    return rows


def dpm_samples(path, day):
    out = []
    pat = re.compile(r'^(\d\d:\d\d:\d\d\.\d+) dpm\s+(\d+) MHz\s+(\d+) mV .*?\)\s+([0-9.]+) C busy\s+([0-9.]+)%'
                     r'.*?power\s+([0-9.]+) W')
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        m = pat.match(line)
        if m:
            t = dt.datetime.combine(day, dt.time.fromisoformat(m.group(1)[:12])) - LOCAL
            out.append((t, int(m.group(2)), int(m.group(3)), float(m.group(4)), float(m.group(5)), float(m.group(6))))
    return out


def main():
    for n in sys.argv[1:]:
        etw = ROOT / 'scratch/m15/etw' / n
        dump, notes = etw / 'gpu-B-dump.txt', etw / 'etw-notes.txt'
        t0, held, trig = notes_window(notes)
        span = held or 120.0
        start, end = t0, t0 + dt.timedelta(seconds=span)
        whole = run_windows(dump, notes, [('all', start, end)])
        first = re.search(r'first present at ([0-9.]+) s', whole)
        lead = float(first.group(1)) if first else 0.0
        # The tool takes whole-second clock times and refuses a span that starts before its own trace zero, which
        # has sub-second precision (559: notes 20:33:42, trace 20:33:42.62): start on the next whole second.
        tz = re.search(r'trace zero (\S+), span ([0-9.]+) s', whole)
        if tz:
            t0 = dt.datetime.fromisoformat(tz.group(1).rstrip('Z')[:26])
            end = t0 + dt.timedelta(seconds=min(span, float(tz.group(2))))
        a = t0 + dt.timedelta(seconds=lead)
        a = a.replace(microsecond=0) + (dt.timedelta(seconds=1) if a.microsecond else dt.timedelta())
        end = end.replace(microsecond=0)
        q = (end - a) / 4
        spans = [('all', a, end)] + [(f'q{i + 1}', a + i * q, a + (i + 1) * q) for i in range(4)]
        out = run_windows(dump, notes, spans)
        rows = parse(out)
        samples = dpm_samples(ROOT / f'scratch/dpm/dpm-game-{n}.txt', t0.date())
        print(f'trial {n}: window B trigger "{trig}", trace zero {t0:%H:%M:%S}Z, held {span:.1f} s, '
              f'first present at {lead:.1f} s')
        print(f'  {"span":<5} {"UTC":<19} {"n":>5} {"rate/s":>7} {"p50 ms":>7} {"p95":>7} {"p99":>7} {"max":>7} '
              f'{"MHz":>6} {"per GHz":>8} {"Tctl":>5} {"busy%":>6} {"W":>5} {"dpm n":>5}')
        for name, s, e in spans:
            r = rows.get(name, {})
            # The sub-window boundaries the tool used (it clips to whole seconds of the trace).
            if 'from_s' in r:
                s, e = t0 + dt.timedelta(seconds=r['from_s']), t0 + dt.timedelta(seconds=r['to_s'])
            win = [x for x in samples if s <= x[0] <= e]
            mhz = statistics.mean(x[1] for x in win) if win else float('nan')
            tc = statistics.mean(x[3] for x in win) if win else float('nan')
            busy = statistics.mean(x[4] for x in win) if win else float('nan')
            pw = statistics.mean(x[5] for x in win) if win else float('nan')
            rate = r.get('rate', float('nan'))
            print(f'  {name:<5} {s:%H:%M:%S}-{e:%H:%M:%S} {r.get("n", 0):>5} {rate:>7.2f} {r.get("p50", 0):>7.2f} '
                  f'{r.get("p95", 0):>7.2f} {r.get("p99", 0):>7.2f} {r.get("max", 0):>7.2f} {mhz:>6.0f} '
                  f'{rate / (mhz / 1000):>8.2f} {tc:>5.1f} {busy:>6.1f} {pw:>5.1f} {len(win):>5}')
        allw = [x for x in samples if spans[0][1] <= x[0] <= spans[0][2]]
        if allw:
            print(f'  clock min {min(x[1] for x in allw)} max {max(x[1] for x in allw)} MHz, Tctl max '
                  f'{max(x[3] for x in allw):.1f} C, mV {min(x[2] for x in allw)}-{max(x[2] for x in allw)}')
        print()
        print(out)


if __name__ == '__main__':
    main()
