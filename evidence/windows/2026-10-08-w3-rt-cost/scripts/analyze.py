"""W3 RT effect-cost series: one row per session from the existing session outputs, all inside window B.

Usage: python analyze.py N[:label] [N[:label] ...] [--json out.json]
Sources (all written by run-arm.sh / run-m157.sh):
  scratch/m15/native-caps001/run-N.out     window B trigger and held lines (lab UTC), ETW per-process interval stats
  scratch/m15/native-caps001/attempts/native-capsN/collected.tar  game/game-log.txt (the game pid, radv_perftest,
                                           experiment), game/dx12user.settings.before (the RT keys the game read)
  scratch/dpm/dpm-game-N.txt               1 s KMD DPM sampler (lab local time = UTC+2): clock, mV, Tctl, GRBM busy, power
  scratch/m15/offgpu/gpuctr-N.txt          1 s Windows GPU engine counters (lab UTC): 3d %
  scratch/m15/native-caps001/plug-N.log    smart plug every ~14 s (development PC UTC; the lab clock runs ~11 s ahead)
"""
import json
import re
import sys
import tarfile
from datetime import datetime, timedelta
from pathlib import Path
from statistics import mean

ROOT = Path(r"P:\bc-250\scratch")
KIT = ROOT / "m15" / "native-caps001"
LOCAL = timedelta(hours=2)
LAB_AHEAD = timedelta(seconds=11)


def ts(text):
    return datetime.fromisoformat(text[:26])


def game_info(n):
    info = {"pid": None, "perftest": None, "experiment": None, "rt_keys": {}}
    tar = KIT / "attempts" / f"native-caps{n}" / "collected.tar"
    if not tar.exists():
        return info
    with tarfile.open(tar) as t:
        names = t.getnames()
        if "./game/game-log.txt" in names:
            log = t.extractfile("./game/game-log.txt").read().decode("utf-8", "replace")
            # Steam start: "launch 2s: witcher3 <pid>"; direct start: "pid <pid> session 1".
            m = re.findall(r"witcher3 (\d+)", log) or re.findall(r"Z pid (\d+) session", log)
            info["pid"] = int(m[0]) if m else None
            m = re.search(r"radv_perftest (\S+)", log)
            info["perftest"] = m.group(1) if m else None
            m = re.search(r"experiment (\S+)", log)
            info["experiment"] = m.group(1) if m else None
        # .after = the file as the game left it (it rewrites it at the menu): the settings it ran with.
        src = "./game/dx12user.settings.after" if "./game/dx12user.settings.after" in names else "./game/dx12user.settings.before"
        if src in names:
            s = t.extractfile(src).read().decode("utf-8", "replace")
            for k in ("EnableRT", "RTGIPreset", "EnableRtRadiance", "Shadows", "RTAOEnabled", "PTEnable", "Resolution",
                      "FullScreenMode", "AAMode", "LimitFPS", "VSync"):
                m = re.search(rf"(?m)^{k}=([^\r\n]*)", s)
                if m:
                    info["rt_keys"][k] = m.group(1)
    return info


def window(n, pid):
    text = (KIT / f"run-{n}.out").read_text(encoding="utf-8", errors="replace")
    start = held = None
    for line in text.splitlines():
        m = re.match(r"(\S+)Z .*window B trigger", line)
        if m:
            start = ts(m.group(1))
        m = re.match(r"(\S+)Z .*window B .*held ([\d.]+) s", line)
        if m:
            held = float(m.group(2))
    stats = {}
    for m in re.finditer(r'"[^"]*" \((\d+)\)\s+n=\s*(\d+) rate=\s*([\d.]+)/s interval median=\s*([\d.]+) ms '
                         r'p95=\s*([\d.]+) ms p99=\s*([\d.]+) ms max=\s*([\d.]+) ms', text):
        stats[int(m.group(1))] = dict(n=int(m.group(2)), rate=float(m.group(3)), median_ms=float(m.group(4)),
                                      p95_ms=float(m.group(5)), p99_ms=float(m.group(6)), max_ms=float(m.group(7)))
    game = stats.get(pid) if pid else None
    return start, held, game, stats


def dpm(n, start, seconds):
    p = ROOT / "dpm" / f"dpm-game-{n}.txt"
    out = []
    if not p.exists() or start is None:
        return out
    for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"(\d\d:\d\d:\d\d\.\d+) dpm\s+(\d+) MHz\s+(\d+) mV.*?([\d.]+) C busy\s+([\d.]+)%.*?power\s+([\d.]+) W", line)
        if not m:
            continue
        # The sampler prints lab local time; take the local date of the window (sessions cross midnight local).
        t = datetime.combine((start + LOCAL).date(), datetime.strptime(m.group(1), "%H:%M:%S.%f").time()) - LOCAL
        if start <= t <= start + timedelta(seconds=seconds):
            out.append(dict(mhz=int(m.group(2)), mv=int(m.group(3)), tctl=float(m.group(4)), busy=float(m.group(5)),
                            power=float(m.group(6))))
    return out


def gpuctr(n, start, seconds):
    p = ROOT / "m15" / "offgpu" / f"gpuctr-{n}.txt"
    out = []
    if not p.exists() or start is None:
        return out
    for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"(\d\d:\d\d:\d\d) 3d ([\d.]+)", line)
        if not m:
            continue
        t = datetime.combine(start.date(), datetime.strptime(m.group(1), "%H:%M:%S").time())
        if start <= t <= start + timedelta(seconds=seconds):
            out.append(float(m.group(2)))
    return out


def plug(n, start, seconds):
    p = KIT / f"plug-{n}.log"
    out = []
    if not p.exists() or start is None:
        return out
    for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.startswith("{"):
            continue
        j = json.loads(line)
        t = datetime.fromisoformat(j["queried_utc"][:26]) + LAB_AHEAD
        if start <= t <= start + timedelta(seconds=seconds) and j.get("power_w") is not None:
            out.append(j["power_w"])
    return out


def row(n, label):
    info = game_info(n)
    start, held, game, stats = window(n, info["pid"])
    secs = held or 90
    d, g, w = dpm(n, start, secs), gpuctr(n, start, secs), plug(n, start, secs)
    r = dict(trial=n, arm=label, pid=info["pid"], perftest=info["perftest"], experiment=info["experiment"],
             rt=info["rt_keys"], window_start=start.isoformat() + "Z" if start else None, held_s=held, game=game)
    if d:
        r["clock_mhz_mean"] = round(mean(x["mhz"] for x in d))
        r["clock_hist"] = {k: sum(1 for x in d if x["mhz"] == k) for k in sorted({x["mhz"] for x in d})}
        r["mv_mean"] = round(mean(x["mv"] for x in d))
        r["tctl_min"], r["tctl_max"] = min(x["tctl"] for x in d), max(x["tctl"] for x in d)
        r["busy_mean"] = round(mean(x["busy"] for x in d), 1)
        r["smu_power_w_mean"] = round(mean(x["power"] for x in d), 1)
        r["dpm_samples"] = len(d)
    if g:
        r["win_3d_pct_mean"] = round(mean(g), 1)
    if w:
        r["plug_w_mean"], r["plug_w_max"], r["plug_samples"] = round(mean(w), 1), max(w), len(w)
    if game and d:
        r["rate_per_ghz"] = round(game["rate"] / (r["clock_mhz_mean"] / 1000), 2)
    return r


def main():
    args = sys.argv[1:]
    out_json = None
    if "--json" in args:
        i = args.index("--json")
        out_json = args[i + 1]
        args = args[:i] + args[i + 2:]
    rows = []
    for a in args:
        n, _, label = a.partition(":")
        rows.append(row(int(n), label or n))
    for r in rows:
        g = r["game"] or {}
        print(f"{r['trial']} {r['arm']:<14} pid {r['pid']} perftest {r['perftest']} exp {r['experiment']} "
              f"rate {g.get('rate')}/s median {g.get('median_ms')} p95 {g.get('p95_ms')} p99 {g.get('p99_ms')} max {g.get('max_ms')} "
              f"| {r.get('clock_mhz_mean')} MHz {r.get('mv_mean')} mV busy {r.get('busy_mean')}% 3d {r.get('win_3d_pct_mean')}% "
              f"Tctl {r.get('tctl_min')}-{r.get('tctl_max')} C SMU {r.get('smu_power_w_mean')} W plug {r.get('plug_w_mean')}/{r.get('plug_w_max')} W "
              f"| per GHz {r.get('rate_per_ghz')} | RT {r['rt'].get('EnableRT')}/{r['rt'].get('EnableRtRadiance')}/"
              f"{r['rt'].get('Shadows')}/{r['rt'].get('RTAOEnabled')}")
    if out_json:
        Path(out_json).write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
