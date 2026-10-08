"""W3 RT effect-cost series, in-game sweep (plan-474): one row per measurement window of one session.

Usage: python analyze-sweep.py N [--json out.json]
The windows are the session's `note:measure X start|end` marks (lab UTC, from the game log in collected.tar). Per window:
etw-present-windows.py over the long window B capture (scratch/m15/etw/N/gpu-B-dump.txt and etw-notes.txt) gives the
game's present rate, interval median/p95/p99/max and the DMA-union GPU busy; the 1 s KMD DPM sampler (lab local time =
UTC+2) gives clock, mV, Tctl, GRBM busy and SMU power; the plug log (development PC UTC, the lab clock runs ~11 s ahead)
gives wall watts. Rate per GHz = rate / mean clock."""
import json
import re
import subprocess
import sys
import tarfile
from datetime import datetime, timedelta
from pathlib import Path
from statistics import mean

ROOT = Path(r"P:\bc-250\scratch")
KIT = ROOT / "m15" / "native-caps001"
SPLIT = ROOT / "m15" / "game-recon" / "etw" / "etw-present-windows.py"
sys.path.insert(0, str(Path(__file__).resolve().parent))
import importlib.util  # noqa: E402

spec = importlib.util.spec_from_file_location("analyze", Path(__file__).resolve().parent / "analyze.py")
A = importlib.util.module_from_spec(spec)
spec.loader.exec_module(A)


def marks(n):
    with tarfile.open(KIT / "attempts" / f"native-caps{n}" / "collected.tar") as t:
        log = t.extractfile("./game/game-log.txt").read().decode("utf-8", "replace")
    found = {}
    # The runtime writes "mark measure X start|end" when it runs the note; the "control ..." summary line follows the
    # whole command.
    for m in re.finditer(r"(?m)^(\S+)Z mark measure (\S+) (start|end)", log):
        found.setdefault(m.group(2), {})[m.group(3)] = A.ts(m.group(1))
    return {k: v for k, v in found.items() if "start" in v and "end" in v}


def main():
    args = sys.argv[1:]
    out_json = None
    if "--json" in args:
        i = args.index("--json")
        out_json = args[i + 1]
        args = args[:i] + args[i + 2:]
    n = int(args[0])
    info = A.game_info(n)
    wins = marks(n)
    etw = ROOT / "m15" / "etw" / str(n)
    specs = [f"{k}={v['start']:%H:%M:%S}-{v['end']:%H:%M:%S}" for k, v in wins.items()]
    split = subprocess.run([sys.executable, str(SPLIT), str(etw / "gpu-B-dump.txt"), str(etw / "etw-notes.txt"), *specs],
                           capture_output=True, text=True, check=True).stdout
    (Path(__file__).resolve().parent / f"split-{n}.txt").write_text(split, encoding="utf-8")
    per, cur = {}, None
    for line in split.splitlines():
        m = re.match(r"== (\S+) \(([\d.]+)\.\.([\d.]+) s, (\d+) s\): GPU busy \(DMA union\) ([\d.]+) %", line)
        if m:
            cur = m.group(1)
            per[cur] = {"span_s": int(m.group(4)), "dma_busy_pct": float(m.group(5))}
            continue
        m = re.match(r"== (\S+): (.*)", line)
        if m:
            per[m.group(1)] = {"error": m.group(2)}
            cur = None
            continue
        m = re.match(r"\s+(.*?)\s+n=\s*(\d+) rate=\s*([\d.]+)/s median=\s*([\d.]+) ms p95=\s*([\d.]+) p99=\s*([\d.]+) "
                     r"max=\s*([\d.]+) >50ms=(\d+)", line)
        if m and cur and info["pid"] and f"({info['pid']})" in m.group(1):
            per[cur]["game"] = dict(n=int(m.group(2)), rate=float(m.group(3)), median_ms=float(m.group(4)),
                                    p95_ms=float(m.group(5)), p99_ms=float(m.group(6)), max_ms=float(m.group(7)),
                                    over50=int(m.group(8)))
    rows = []
    for k, v in wins.items():
        secs = (v["end"] - v["start"]).total_seconds()
        d, w = A.dpm(n, v["start"], secs), A.plug(n, v["start"], secs)
        r = dict(trial=n, window=k, start=v["start"].isoformat() + "Z", end=v["end"].isoformat() + "Z", **per.get(k, {}))
        if d:
            r.update(clock_mhz_mean=round(mean(x["mhz"] for x in d)), mv_mean=round(mean(x["mv"] for x in d)),
                     tctl_min=min(x["tctl"] for x in d), tctl_max=max(x["tctl"] for x in d),
                     busy_mean=round(mean(x["busy"] for x in d), 1), smu_power_w_mean=round(mean(x["power"] for x in d), 1),
                     dpm_samples=len(d))
        if w:
            r.update(plug_w_mean=round(mean(w), 1), plug_w_max=max(w), plug_samples=len(w))
        if r.get("game") and d:
            r["rate_per_ghz"] = round(r["game"]["rate"] / (r["clock_mhz_mean"] / 1000), 2)
        rows.append(r)
    for r in rows:
        g = r.get("game") or {}
        print(f"{r['trial']} {r['window']:<8} {r['start'][11:19]}-{r['end'][11:19]} rate {g.get('rate')}/s median "
              f"{g.get('median_ms')} p95 {g.get('p95_ms')} p99 {g.get('p99_ms')} max {g.get('max_ms')} >50ms {g.get('over50')} "
              f"| DMA busy {r.get('dma_busy_pct')}% | {r.get('clock_mhz_mean')} MHz {r.get('mv_mean')} mV busy {r.get('busy_mean')}% "
              f"Tctl {r.get('tctl_min')}-{r.get('tctl_max')} C SMU {r.get('smu_power_w_mean')} W plug {r.get('plug_w_mean')}/"
              f"{r.get('plug_w_max')} W | per GHz {r.get('rate_per_ghz')} {r.get('error', '')}")
    if out_json:
        Path(out_json).write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
