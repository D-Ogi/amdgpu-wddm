"""Does the W3 RT frame rate follow the shader clock? For each measurement window of the in-game sweep (plan-474),
cuts the window into 5 s slices and prints the game's present rate and the mean KMD DPM clock of each slice, plus a
least-squares fit rate = a * GHz + b per window. A rate that follows the clock gives b near 0 (rate per GHz constant).

Usage: python clock-scaling.py N   (after analyze-sweep.py N; reads scratch/m15/etw/N/gpu-B-dump.txt, one pass)"""
import json
import sys
from datetime import datetime, timedelta
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import importlib.util  # noqa: E402

spec = importlib.util.spec_from_file_location("analyze", HERE / "analyze.py")
A = importlib.util.module_from_spec(spec)
spec.loader.exec_module(A)
spec = importlib.util.spec_from_file_location("split", A.ROOT / "m15" / "game-recon" / "etw" / "etw-present-windows.py")
S = importlib.util.module_from_spec(spec)
spec.loader.exec_module(S)

n = int(sys.argv[1])
rows = json.loads((HERE / f"sweep-{n}.json").read_text(encoding="utf-8"))
pid = A.game_info(n)["pid"]
etw = A.ROOT / "m15" / "etw" / str(n)
t0 = S.base_utc(str(etw / "etw-notes.txt"))
times = []
with open(etw / "gpu-B-dump.txt", encoding="utf-8", errors="replace") as f:
    for line in f:
        if line.startswith("Microsoft-Windows-DxgKrnl/Present/win:Info") and f"({pid})" in line:
            times.append(t0 + timedelta(microseconds=int(line.split(",")[1].strip())))
SLICE = 5
for r in rows:
    a = datetime.fromisoformat(r["start"][:26])
    b = datetime.fromisoformat(r["end"][:26])
    pts = []
    t = a
    while t + timedelta(seconds=SLICE) <= b:
        u = t + timedelta(seconds=SLICE)
        cnt = sum(1 for x in times if t <= x < u)
        d = A.dpm(n, t, SLICE)
        if d:
            ghz = sum(x["mhz"] for x in d) / len(d) / 1000
            pts.append((ghz, cnt / SLICE))
        t = u
    if len(pts) < 3:
        print(r["window"], "too few slices")
        continue
    mx = sum(p[0] for p in pts) / len(pts)
    my = sum(p[1] for p in pts) / len(pts)
    sxx = sum((p[0] - mx) ** 2 for p in pts)
    slope = sum((p[0] - mx) * (p[1] - my) for p in pts) / sxx if sxx > 1e-9 else float("nan")
    icpt = my - slope * mx if sxx > 1e-9 else float("nan")
    print(f"{r['window']:<8} GHz {min(p[0] for p in pts):.3f}-{max(p[0] for p in pts):.3f} rate {min(p[1] for p in pts):.1f}-"
          f"{max(p[1] for p in pts):.1f} fit rate = {slope:.2f} * GHz + {icpt:.2f} | slices " +
          " ".join(f"{g:.2f}:{v:.1f}" for g, v in pts))
