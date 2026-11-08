#!/usr/bin/env python3
"""Summary of one `bc250kmd_cli dpm trace` file (BD-329).

Reads the CSV that `bc250kmd_cli dpm trace SECONDS [MS]` prints and answers the questions BD-329 asks of a run:
when the load started, how fast the clock rose, what the busy share was while the clock stayed low, which rule held
it, and where the busy share came from. With --reach MHZ --within S it also prints one verdict line:

    BD329-RAMP PASS reached 1700 MHz 6.2 s after the load started (bound 10.0 s)
    BD329-RAMP FAIL highest 1300 MHz in 10.4 s of load (bound 1700 MHz within 10.0 s)

The load starts at the first row whose busy share is at least --load-permille and in which at least half of the next
ten rows are too; it ends after the last such row. Rows before the load (the desktop, the game's loading screen) are
counted apart. Host only: it reads a file, nothing else.

    python tools/win/bc250kmd_cli/dpm_trace_summary.py trace.csv --reach 1700 --within 10
"""
import argparse
import csv
import sys

COLUMNS = ("t_ms", "ticks", "mhz", "target_mhz", "want_mhz", "cap_mhz", "busy_pm", "avg_pm", "src", "temp_mc",
           "throttle", "idle", "compute", "raises", "lowers")


def read_rows(lines):
    """The data rows as dicts with integer fields; comment lines (#) are skipped."""
    body = [line for line in lines if line.strip() and not line.startswith("#")]
    if not body:
        return []
    reader = csv.DictReader(body)
    missing = [c for c in COLUMNS if c not in (reader.fieldnames or [])]
    if missing:
        raise ValueError("not a dpm trace: missing columns %s" % ", ".join(missing))
    rows = []
    for raw in reader:
        row = dict(raw)
        for key in ("t_ms", "ticks", "uptime_ms", "mhz", "target_mhz", "want_mhz", "cap_mhz", "max_mhz", "smu_mhz",
                    "busy_pm", "avg_pm", "submit_pm", "sdma_pm", "temp_mc", "temp_ok", "idle", "compute", "stable",
                    "paused", "raises", "lowers", "thermal", "errors"):
            if key in row and row[key] not in (None, ""):
                row[key] = int(row[key])
        rows.append(row)
    return rows


def load_window(rows, load_permille):
    """(first, last) indices of the load, or None."""
    first = None
    for i, row in enumerate(rows):
        if row["busy_pm"] < load_permille:
            continue
        ahead = rows[i + 1:i + 11]
        if ahead and sum(1 for r in ahead if r["busy_pm"] >= load_permille) * 2 >= len(ahead):
            first = i
            break
    if first is None:
        return None
    last = first
    for i in range(first, len(rows)):
        if rows[i]["busy_pm"] >= load_permille:
            last = i
    return first, last


def band(busy):
    if busy >= 900:
        return ">=900"
    if busy >= 800:
        return "800-899"
    if busy >= 650:
        return "650-799"
    return "<650"


def summarize(rows, load_permille=500, low_mhz=1300, reach=None, within=None):
    """The summary as a list of lines, and the verdict (True, False, or None without --reach)."""
    out = []
    if not rows:
        return ["dpm trace: no rows"], (False if reach else None)
    span = (rows[-1]["t_ms"] - rows[0]["t_ms"]) / 1000.0
    ticks = rows[-1]["ticks"] - rows[0]["ticks"]
    out.append("rows %d over %.1f s, governor ticks %d (%.1f ms a tick), modes %s" % (
        len(rows), span, ticks, (span * 1000.0 / ticks) if ticks else 0.0,
        ",".join(sorted({r.get("mode", "?") for r in rows}))))
    window = load_window(rows, load_permille)
    if window is None:
        out.append("load: none (no row at %d permille with half of the next ten at it)" % load_permille)
        return out, (False if reach else None)
    first, last = window
    load = rows[first:last + 1]
    t0 = rows[first]["t_ms"]
    out.append("load: %.2f s to %.2f s (%.1f s), %d rows; start %d MHz %.1f C avg %d permille" % (
        t0 / 1000.0, rows[last]["t_ms"] / 1000.0, (rows[last]["t_ms"] - t0) / 1000.0, len(load), rows[first]["mhz"],
        rows[first]["temp_mc"] / 1000.0, rows[first]["avg_pm"]))
    top = max(r["mhz"] for r in load)
    out.append("clock in the load: highest %d MHz, mean %.0f MHz, at the end %d MHz; raises %d lowers %d" % (
        top, sum(r["mhz"] for r in load) / len(load), load[-1]["mhz"], load[-1]["raises"] - rows[first]["raises"],
        load[-1]["lowers"] - rows[first]["lowers"]))
    for mark in (1200, 1500, 1700, 1800, 2000):
        hit = next((r for r in load if r["mhz"] >= mark), None)
        out.append("  reached %4d MHz: %s" % (mark, "%.2f s after the start" % ((hit["t_ms"] - t0) / 1000.0)
                                                 if hit else "never"))
    out.append("temperature in the load: %.1f C at the start, %.1f C highest, %.1f C at the end" % (
        rows[first]["temp_mc"] / 1000.0, max(r["temp_mc"] for r in load) / 1000.0, load[-1]["temp_mc"] / 1000.0))
    src = {}
    for r in load:
        src[r["src"]] = src.get(r["src"], 0) + 1
    out.append("busy source in the load: %s" % ", ".join("%s %d" % (k, v) for k, v in sorted(src.items())))
    low = [r for r in load if r["mhz"] <= low_mhz]
    if low:
        bands = {}
        for r in low:
            bands[band(r["busy_pm"])] = bands.get(band(r["busy_pm"]), 0) + 1
        out.append("rows at or under %d MHz: %d, busy mean %.0f permille, avg mean %.0f; tick share %s" % (
            low_mhz, len(low), sum(r["busy_pm"] for r in low) / len(low), sum(r["avg_pm"] for r in low) / len(low),
            ", ".join("%s %d" % (k, bands.get(k, 0)) for k in (">=900", "800-899", "650-799", "<650"))))
    else:
        out.append("rows at or under %d MHz: none" % low_mhz)
    reasons = {}
    for r in load:
        reasons[r["throttle"]] = reasons.get(r["throttle"], 0) + 1
    out.append("throttle in the load: %s" % ", ".join("%s %d" % (k, v) for k, v in sorted(reasons.items())))
    out.append("compute set rows %d, idle rows %d, want above the clock in %d rows" % (
        sum(r["compute"] for r in load), sum(r["idle"] for r in load), sum(1 for r in load if r["want_mhz"] > r["mhz"])))
    verdict = None
    if reach is not None:
        hit = next((r for r in load if r["mhz"] >= reach), None)
        if hit is not None and (within is None or (hit["t_ms"] - t0) / 1000.0 <= within):
            verdict = True
            out.append("BD329-RAMP PASS reached %d MHz %.1f s after the load started (bound %s)" % (
                reach, (hit["t_ms"] - t0) / 1000.0, "%.1f s" % within if within is not None else "none"))
        else:
            verdict = False
            out.append("BD329-RAMP FAIL highest %d MHz in %.1f s of load (bound %d MHz within %s)" % (
                top, (rows[last]["t_ms"] - t0) / 1000.0, reach, "%.1f s" % within if within is not None else "the load"))
    return out, verdict


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("trace")
    ap.add_argument("--load-permille", type=int, default=500)
    ap.add_argument("--low-mhz", type=int, default=1300)
    ap.add_argument("--reach", type=int)
    ap.add_argument("--within", type=float)
    args = ap.parse_args(argv)
    with open(args.trace, encoding="utf-8", errors="replace") as f:
        rows = read_rows(f.read().splitlines())
    lines, verdict = summarize(rows, args.load_permille, args.low_mhz, args.reach, args.within)
    print("\n".join(lines))
    return 1 if verdict is False else 0


if __name__ == "__main__":
    sys.exit(main())
