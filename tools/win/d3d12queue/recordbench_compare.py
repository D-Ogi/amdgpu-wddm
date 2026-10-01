# SPDX-License-Identifier: MIT
"""A/B table of two record bench sessions (build.ps1 -RecordBench): the deferred-replay experiment off and on.

Reads each session directory's trace.jsonl (the run's start, phase and final lines) and recordbench-frames.csv (one row
per counted frame), and prints per phase the recording thread's API cycles and microseconds per frame, the frame's
wall time and the CPU time from GetThreadTimes and GetProcessTimes, with the change from the first session to the
second. A session that did not pass (final line not S_OK, a mismatch) is named, and its numbers are still shown.
"""
import argparse
import csv
import json
import re
import statistics
import sys
from pathlib import Path

PHASES = ("burst", "interleaved", "threaded")
START = re.compile(r"Record bench seed [0-9a-f]{16}, .*cycles per us (?P<rate>[0-9.]+), AMDGPU_WDDM_D3D12_EXPERIMENT (?P<experiment>.+)")
WALL = re.compile(r"Record bench (?P<phase>\w+) wall us: .*; cpu ms per frame: main thread (?P<thread>[0-9.]+), "
                  r"process (?P<process>[0-9.]+), over (?P<span>[0-9.]+) ms")
FINAL = re.compile(r"Record bench: phases measured (?P<phases>\d+) of 3, frames (?P<frames>\d+), words (?P<words>\d+), "
                   r"mismatches (?P<mismatches>\d+), .*cycles per us at the end (?P<end_rate>[0-9.]+), AMDGPU_WDDM_D3D12_EXPERIMENT .+")


def session(directory):
    root = Path(directory)
    run = dict(name=root.name, rate=None, experiment=None, final=None, hr=None, wall={}, frames={phase: [] for phase in PHASES})
    with open(root / "trace.jsonl", encoding="utf-8", errors="replace") as trace:
        for line in trace:
            try:
                record = json.loads(line)
            except ValueError:
                continue
            api = record.get("api") if isinstance(record, dict) else None
            if not isinstance(api, str) or not api.startswith("Record bench"):
                continue
            if match := START.fullmatch(api):
                run["rate"], run["experiment"] = float(match["rate"]), match["experiment"]
            elif match := WALL.fullmatch(api):
                run["wall"][match["phase"]] = dict(thread_ms=float(match["thread"]), process_ms=float(match["process"]))
            elif match := FINAL.fullmatch(api):
                run["final"] = {key: float(value) if "." in value else int(value) for key, value in match.groupdict().items()}
                run["hr"] = record.get("hr")
    with open(root / "recordbench-frames.csv", newline="", encoding="utf-8") as rows:
        for row in csv.DictReader(rows):
            if row["phase"] in run["frames"]:
                run["frames"][row["phase"]].append(row)
    if run["rate"] is None:
        raise ValueError(f"{root}: no record bench start line in trace.jsonl")
    return run


def phase_figures(run, phase):
    rows = run["frames"][phase]
    if not rows:
        return None
    api = [int(row["api_cycles_max"]) for row in rows]
    frame = [float(row["frame_us"]) for row in rows]
    record = [float(row["record_us"]) for row in rows]
    figures = dict(frames=len(rows), api_mcycles=statistics.fmean(api) / 1e6, api_us=statistics.fmean(api) / run["rate"],
                   api_p95_us=sorted(api)[min(len(api) - 1, round(0.95 * (len(api) - 1)))] / run["rate"],
                   record_us=statistics.fmean(record), frame_us=statistics.fmean(frame),
                   frame_p50_us=statistics.median(frame))
    figures.update(run["wall"].get(phase, {}))
    return figures


def passed(run):
    final = run["final"]
    return bool(final) and run["hr"] == "00000000" and final["phases"] == 3 and final["mismatches"] == 0


def table(first, second):
    lines = [f"A: {first['name']} (experiment {first['experiment']}, {first['rate']:.1f} cycles/us)",
             f"B: {second['name']} (experiment {second['experiment']}, {second['rate']:.1f} cycles/us)"]
    for run, label in ((first, "A"), (second, "B")):
        if not passed(run):
            lines.append(f"{label} did not pass: final line {run['final']}, hr {run['hr']}")
    keys = (("api_us", "api cpu us/frame", 1), ("api_p95_us", "api cpu us p95", 1), ("api_mcycles", "api Mcycles/frame", 3),
            ("record_us", "record wall us", 1), ("frame_us", "frame wall us", 1), ("frame_p50_us", "frame wall us p50", 1),
            ("thread_ms", "main thread cpu ms/frame", 3), ("process_ms", "process cpu ms/frame", 3))
    for phase in PHASES:
        a, b = phase_figures(first, phase), phase_figures(second, phase)
        lines.append(f"{phase}: frames {a['frames'] if a else 0} / {b['frames'] if b else 0}")
        if not a or not b:
            continue
        for key, title, digits in keys:
            if key not in a or key not in b:
                continue
            change = f"{(b[key] - a[key]) / a[key] * 100:+.1f} %" if a[key] else "n/a"
            lines.append(f"  {title:26} {a[key]:12.{digits}f} {b[key]:12.{digits}f}  {change}")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("first", type=Path, help="session directory A (usually the experiment off)")
    parser.add_argument("second", type=Path, help="session directory B (usually deferred-replay on)")
    args = parser.parse_args()
    try:
        first, second = session(args.first), session(args.second)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(2, f"{error}\n")
    print(table(first, second))
    return 0 if passed(first) and passed(second) else 1


if __name__ == "__main__":
    sys.exit(main())
