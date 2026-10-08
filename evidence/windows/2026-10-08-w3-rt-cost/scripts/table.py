"""Markdown tables for RESULT.md of the W3 RT effect-cost series from analyze.py's results.json (one row per launch)
and analyze-sweep.py's sweep-N.json (one row per in-game window).

Clock adjustment: inside the 474 windows the 5 s slices give rate = a * GHz + b with b > 0 (clock-scaling.py); the
elasticity a * GHz / rate is 0.74 to 0.77, so the rate does not follow the clock 1:1. Besides rate per GHz (which
assumes 1:1) the table gives the rate at 1.5 GHz as rate * (1.5 / GHz) ** 0.75 and its frame time.

Usage: python table.py EVIDENCE_DIR"""
import json
import sys
from pathlib import Path

E = 0.75
REF = 1.5
LABEL = {
    ("true", "false", "true", "1", "true"): "GI perf + reflections + shadows perf + AO (all four)",
    ("true", "false", "false", "0", "false"): "GI perf only",
    ("true", "false", "true", "0", "false"): "GI perf + reflections",
    ("true", "false", "false", "1", "false"): "GI perf + shadows perf",
    ("true", "false", "false", "0", "true"): "GI perf + AO",
}
SWEEP = {"A-ctl": "all four (in-session control)", "noAO": "GI perf + reflections + shadows perf (= high-rt preset)",
         "rt-off": "RT off", "gi-q": "all four, GI Quality"}

out = Path(sys.argv[1])
rows = []
sweeps = {int(f.stem.split("-")[1]) for f in out.glob("sweep-*.json")}
for r in json.loads((out / "results.json").read_text(encoding="utf-8")):
    if r["trial"] in sweeps:   # its window B spans every in-game window and the menu work between them
        continue
    rt = r["rt"]
    key = tuple(rt.get(k, "").lower() for k in ("EnableRT", "RTGIPreset", "EnableRtRadiance", "Shadows", "RTAOEnabled"))
    g = r.get("game") or {}
    rows.append(dict(id=str(r["trial"]), arm=r["arm"], what=LABEL.get(key, str(key)), knob=r.get("perftest") or "none",
                     rate=g.get("rate"), med=g.get("median_ms"), p95=g.get("p95_ms"), p99=g.get("p99_ms"),
                     mhz=r.get("clock_mhz_mean"), tctl=f"{r.get('tctl_min')}-{r.get('tctl_max')}", plug=r.get("plug_w_mean")))
for f in sorted(out.glob("sweep-*.json")):
    for r in json.loads(f.read_text(encoding="utf-8")):
        g = r.get("game") or {}
        rows.append(dict(id=f"{r['trial']} {r['window']}", arm="sweep", what=SWEEP.get(r["window"], r["window"]), knob="none",
                         rate=g.get("rate"), med=g.get("median_ms"), p95=g.get("p95_ms"), p99=g.get("p99_ms"),
                         mhz=r.get("clock_mhz_mean"), tctl=f"{r.get('tctl_min')}-{r.get('tctl_max')}", plug=r.get("plug_w_mean")))
print("| Trial | RT settings | RADV knob | Frames/s | Median ms | p95 ms | p99 ms | Clock MHz | Tctl C | Plug W | Frames/s per GHz | Frames/s at 1.5 GHz (e 0.75) | Frame ms at 1.5 GHz |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for r in rows:
    if not r["rate"] or not r["mhz"]:
        print(f"| {r['id']} | {r['what']} | {r['knob']} | no data | | | | | | | | | |")
        continue
    ghz = r["mhz"] / 1000
    adj = r["rate"] * (REF / ghz) ** E
    print(f"| {r['id']} | {r['what']} | {r['knob']} | {r['rate']:.1f} | {r['med']:.1f} | {r['p95']:.1f} | {r['p99']:.1f} | "
          f"{r['mhz']} | {r['tctl']} | {r['plug']} | {r['rate'] / ghz:.2f} | {adj:.1f} | {1000 / adj:.1f} |")
