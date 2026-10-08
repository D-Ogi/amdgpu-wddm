"""Copies the raw files of the W3 RT effect-cost series into the evidence directory of the worktree.

Usage: python collect.py N:ARM [N:ARM ...] [--sweep N]
Per trial: plan, window B block of run-N.out, settings step output, game log and RT keys of the settings file the game
left, DPM sampler header plus the window B rows, Windows GPU counter rows of window B, plug log, fault scan of the
in-trial KMD stream, the last correctness screenshot (already scaled to 0.33 on the lab), the identity read after the
trial. Plus the scripts and presets, analyze.py's JSON and a sha256 list."""
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tarfile
from datetime import datetime, timedelta
from pathlib import Path

ROOT = Path(r"P:\bc-250\scratch")
KIT = ROOT / "m15" / "native-caps001"
W = ROOT / "w3-rt-cost"
OUT = W / "wt" / "evidence" / "windows" / "2026-10-08-w3-rt-cost"
LOCAL = timedelta(hours=2)
FAULT = re.compile(r"(?i)tdr|reset|fault|hang|removed|timeout|0x116|preempt.*fail")
# 474: shot of each measured window (0.5 scale) and of the Graphics page after each settings change.
SWEEP_SHOTS = {"shot-003.jpg": "A-ctl", "shot-008.jpg": "page-AO-off", "shot-010.jpg": "noAO", "shot-015.jpg": "page-RT-off",
               "shot-017.jpg": "rt-off", "shot-024.jpg": "page-GI-quality", "shot-026.jpg": "gi-q",
               "shot-036.jpg": "page-restored"}
LAB_ADDR = re.compile(r"\b(?:192\.168|10\.\d+|172\.(?:1[6-9]|2\d|3[01]))\.\d+\.\d+\b")


def window(n):
    text = (KIT / f"run-{n}.out").read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    keep, start, held = [], None, 91
    for i, line in enumerate(lines):
        if line.startswith("== window B"):
            keep += lines[i:i + 4]
        if "window B trigger" in line or "window B gpu-only" in line or "window A" in line:
            keep.append(line)
        m = re.match(r"(\S+)Z .*window B trigger", line)
        if m:
            start = datetime.fromisoformat(m.group(1)[:26])
        m = re.search(r"window B .*held ([\d.]+) s", line)
        if m:
            held = float(m.group(1)) + 1
    return keep, start, held


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "scripts").mkdir(exist_ok=True)
    trials, sweep, args = [], None, sys.argv[1:]
    if "--sweep" in args:
        i = args.index("--sweep")
        sweep = int(args[i + 1])
        args = args[:i] + args[i + 2:]
    for a in args:
        n, _, arm = a.partition(":")
        trials.append((int(n), arm))
    for n, arm in trials:
        shutil.copy2(KIT / "plans" / f"plan-{n}.md", OUT / f"plan-{n}.md")
        keep, start, held = window(n)
        (OUT / f"window-B-{n}.txt").write_text("\n".join(keep) + "\n", encoding="utf-8")
        s = ROOT / "dpm" / f"settings-{n}.txt"
        if s.exists():
            shutil.copy2(s, OUT / f"settings-step-{n}.txt")
        tar = KIT / "attempts" / f"native-caps{n}" / "collected.tar"
        with tarfile.open(tar) as t:
            (OUT / f"game-log-{n}.txt").write_bytes(t.extractfile("./game/game-log.txt").read())
            res = json.loads(t.extractfile("./game/game-result.json").read().decode("utf-8-sig"))
            after = t.extractfile("./game/dx12user.settings.after").read().decode("utf-8", "replace")
            keys = re.findall(r"(?m)^(Resolution|FullScreenMode|VSync|LimitFPS|EnableRT|RTGIPreset|EnableRtRadiance|Shadows|"
                              r"RTAOEnabled|PTEnable|AAMode|FSRFramegen|DLSSGMode|XessFrameGeneration)=([^\r\n]*)", after)
            summary = {k: res.get(k) for k in ("pid", "launch", "experiment", "radv_perftest", "stop_reason", "errors",
                                               "has_exited", "exit_code", "responding_last", "application_events",
                                               "elapsed_seconds", "exe_version")}
            (OUT / f"game-result-{n}.txt").write_text(
                json.dumps(summary, indent=2) + "\n\ndx12user.settings as the game left it (RT and output keys):\n"
                + "\n".join(f"{k}={v}" for k, v in keys) + "\n", encoding="utf-8")
            kern = t.extractfile("./game-kernel.log").read().decode("utf-8", "replace").splitlines()
        hits = [l for l in kern if FAULT.search(l) and "telemetry" not in l]
        modes = [l for l in kern if "display modes:" in l or "CommitVidPn" in l]
        (OUT / f"kmd-scan-{n}.txt").write_text(
            f"in-trial KMD stream (game-kernel.log): {len(kern)} lines\n"
            f"lines matching {FAULT.pattern}: {len(hits)}\n" + "\n".join(hits[:40]) +
            f"\n\nmode-set lines: {len(modes)}\n" + "\n".join(modes[:20]) + "\n", encoding="utf-8")
        d = ROOT / "dpm" / f"dpm-game-{n}.txt"
        rows = []
        for line in d.read_text(encoding="utf-8", errors="replace").splitlines():
            m = re.match(r"(\d\d:\d\d:\d\d\.\d+) dpm", line)
            if not m:
                rows.append(line) if len(rows) < 3 else None
                continue
            t = datetime.combine((start + LOCAL).date(), datetime.strptime(m.group(1), "%H:%M:%S.%f").time()) - LOCAL
            if start <= t <= start + timedelta(seconds=held):
                rows.append(line)
        (OUT / f"dpm-window-B-{n}.txt").write_text(
            "KMD DPM sampler, 1 s, lab local time (UTC+2); header and the rows inside window B\n" + "\n".join(rows) + "\n",
            encoding="utf-8")
        g = ROOT / "m15" / "offgpu" / f"gpuctr-{n}.txt"
        grows = [] if g.exists() else ["no GPU counter file: the session's counter sampler wrote none (472)"]
        for line in (g.read_text(encoding="utf-8", errors="replace") if g.exists() else "").splitlines():
            m = re.match(r"(\d\d:\d\d:\d\d) 3d", line)
            if not m:
                grows.append(line)
                continue
            t = datetime.combine(start.date(), datetime.strptime(m.group(1), "%H:%M:%S").time())
            if start <= t <= start + timedelta(seconds=held):
                grows.append(line)
        (OUT / f"gpu-counters-window-B-{n}.txt").write_text("\n".join(grows) + "\n", encoding="utf-8")
        p = KIT / f"plug-{n}.log"
        if p.exists():
            shutil.copy2(p, OUT / f"plug-{n}.log")
        shots = sorted((ROOT / "m15" / "control" / f"native-caps{n}").glob("shot-*.jpg"))
        if shots:
            shutil.copy2(shots[-1], OUT / f"shot-{n}.jpg")
        i = W / "lab" / f"identity-after{n}.txt"
        if i.exists():
            shutil.copy2(i, OUT / f"identity-after-{n}.txt")
    if sweep:
        # In-game sweep (plan-474): the operator log with every OCR read and key sequence, the per-window split of the
        # long window B, its rows as JSON, one shot per measured window and the Graphics page after each change.
        shutil.copy2(W / f"sweep-{sweep}.log", OUT / f"sweep-{sweep}.log")
        shutil.copy2(W / f"split-{sweep}.txt", OUT / f"split-{sweep}.txt")
        shutil.copy2(W / f"clock-scaling-{sweep}.txt", OUT / f"clock-scaling-{sweep}.txt")
        subprocess.run([sys.executable, str(W / "analyze-sweep.py"), str(sweep), "--json", str(OUT / f"sweep-{sweep}.json")],
                       check=True)
        ctl = ROOT / "m15" / "control" / f"native-caps{sweep}"
        for shot, name in SWEEP_SHOTS.items():
            shutil.copy2(ctl / shot, OUT / f"sweep-{sweep}-{name}.jpg")
    # 475: refused by the preflight before any GPU work (stale RADV_PERFTEST marker): plan with result, the runner's
    # trial log and the identity read afterwards. identity-final: the lab after the series and the settings restore.
    shutil.copy2(KIT / "plans" / "plan-475.md", OUT / "plan-475.md")
    shutil.copy2(KIT / "trial-475.log", OUT / "trial-475.log")
    shutil.copy2(W / "lab" / "identity-after475.txt", OUT / "identity-after-475.txt")
    shutil.copy2(W / "lab" / "identity-final.txt", OUT / "identity-final.txt")
    shutil.copy2(W / "lab" / "identity-pre.txt", OUT / "identity-pre.txt")
    for f in ("run-arm.sh", "series.sh", "analyze.py", "make-presets.py", "make-plan.py", "collect.py", "rt-sweep.sh",
              "analyze-sweep.py", "clock-scaling.py", "table.py",
              "handdev-kmd20-baseline.py"):
        shutil.copy2(W / f, OUT / "scripts" / f)
    for f in ("arm-knobs.ps1", "identity.ps1"):
        shutil.copy2(W / "lab" / f, OUT / "scripts" / f)
    for f in sorted((ROOT / "m15" / "game-recon" / "presets").glob("w3rt-*.txt")):
        shutil.copy2(f, OUT / "scripts" / f.name)
    args = [f"{n}:{arm}" for n, arm in trials]
    subprocess.run([sys.executable, str(W / "analyze.py"), *args, "--json", str(OUT / "results.json")], check=True)
    bad = []
    for f in OUT.rglob("*"):
        if f.is_file() and f.suffix in (".txt", ".md", ".json", ".log", ".sh", ".py", ".ps1"):
            if LAB_ADDR.search(f.read_text(encoding="utf-8", errors="replace")):
                bad.append(f.name)
    if bad:
        print("LAB ADDRESS FOUND IN:", bad)
    lines = []
    for f in sorted(OUT.rglob("*")):
        if f.is_file() and f.name != "sha256.txt":
            lines.append(f"{hashlib.sha256(f.read_bytes()).hexdigest()} *{f.relative_to(OUT).as_posix()}")
    (OUT / "sha256.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("collected", len(lines), "files into", OUT)


if __name__ == "__main__":
    main()
