"""Frame time at matched GPU clock: window B of each session split by the 1 s KMD DPM sample that was in force.

    python -P clockmatch.py N [N ...]

The rate per GHz of analyze.py is not clock-neutral in this scene: inside one session it rises as the thermal policy
lowers the clock (559 q1 1457 MHz 10.71, q4 1243 MHz 11.02), so two sessions that sat at different clocks differ in
it without any change in the driver. Here every frame interval of the game (DxgKrnl Present of the process with the
most presents) is assigned to the DPM sample before its end, and only frames inside a stable stretch (the sample
before and the sample after show the same clock) count. Per clock level: frames, seconds covered, mean interval and
the rate it implies (1000 / mean), median, p99. Inputs as in analyze.py; both clocks are the lab's own (the ETW
trace zero is the lab's UTC from etw-notes.txt, the sampler prints the lab's local time, UTC+2 on this date)."""
import bisect
import collections
import datetime as dt
import re
import statistics
import sys
from pathlib import Path

ROOT = Path('P:/BC-250')
LOCAL = dt.timedelta(hours=2)


def trace_zero(notes):
    for line in notes.read_text(encoding='utf-8', errors='replace').splitlines():
        if 'window B gpu session' in line and 'start exited' in line:
            return dt.datetime.strptime(line.split()[0][:26], '%Y-%m-%dT%H:%M:%S.%f')
    raise SystemExit(f'no window B start line in {notes}')


def presents(dump):
    per = collections.defaultdict(list)
    with open(dump, encoding='utf-8', errors='replace') as f:
        header = True
        for line in f:
            if header:
                header = not line.startswith('EndHeader')
                continue
            if line.startswith('Microsoft-Windows-DxgKrnl/Present/win:Info'):
                p = [x.strip() for x in line.split(',')]
                per[p[2]].append(int(p[1]))
    return max(per.values(), key=len)


def samples(path, day):
    pat = re.compile(r'^(\d\d:\d\d:\d\d\.\d+) dpm\s+(\d+) MHz\s+(\d+) mV .*?\)\s+([0-9.]+) C busy\s+([0-9.]+)%')
    out = []
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        m = pat.match(line)
        if m:
            t = dt.datetime.combine(day, dt.time.fromisoformat(m.group(1)[:12])) - LOCAL
            out.append((t, int(m.group(2)), float(m.group(4))))
    return out


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))]


def main():
    for n in sys.argv[1:]:
        etw = ROOT / 'scratch/m15/etw' / n
        t0 = trace_zero(etw / 'etw-notes.txt')
        ts = sorted(presents(etw / 'gpu-B-dump.txt'))
        smp = samples(ROOT / f'scratch/dpm/dpm-game-{n}.txt', t0.date())
        times = [s[0] for s in smp]
        by = collections.defaultdict(list)
        secs = collections.defaultdict(set)
        per_sec = collections.defaultdict(list)
        tc = collections.defaultdict(list)
        for a, b in zip(ts, ts[1:]):
            end = t0 + dt.timedelta(microseconds=b)
            i = bisect.bisect_right(times, end) - 1
            if i < 1 or i + 1 >= len(smp):
                continue
            # A stable stretch: the samples on both sides of this one agree with it.
            if not (smp[i - 1][1] == smp[i][1] == smp[i + 1][1]):
                continue
            mhz = smp[i][1]
            by[mhz].append((b - a) / 1000.0)
            secs[mhz].add(i)
            per_sec[(mhz, i)].append((b - a) / 1000.0)
            tc[mhz].append(smp[i][2])
        print(f'trial {n}: {len(ts)} presents, trace zero {t0:%H:%M:%S.%f}Z, frames in stable clock stretches by level')
        print(f'  {"MHz":>5} {"frames":>6} {"s":>4} {"mean ms":>8} {"rate/s":>7} {"p50 ms":>7} {"p99 ms":>7} {"Tctl":>5} '
              f'{"sd(1s) ms":>9} {"se ms":>6}')
        for mhz in sorted(by):
            v = by[mhz]
            mean = statistics.mean(v)
            # The spread of the 1 s means at this level, and the standard error of their mean (seconds as samples).
            sec_means = [statistics.mean(x) for (m, _), x in per_sec.items() if m == mhz]
            sd = statistics.stdev(sec_means) if len(sec_means) > 1 else float('nan')
            se = sd / len(sec_means) ** 0.5
            print(f'  {mhz:>5} {len(v):>6} {len(secs[mhz]):>4} {mean:>8.2f} {1000 / mean:>7.2f} {pct(v, .5):>7.2f} '
                  f'{pct(v, .99):>7.2f} {statistics.mean(tc[mhz]):>5.1f} {sd:>9.2f} {se:>6.2f}')
        print()


if __name__ == '__main__':
    main()
