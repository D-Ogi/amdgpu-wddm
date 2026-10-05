"""Merge the per-batch results of run-batch.ps1 and compare them with a reference run.

    python summarize.py RESULTS_RUN_DIR --cases lists/sparse-resources.txt [--reference REF_RUN_DIR]
                        [--out OUT_DIR]

RESULTS_RUN_DIR is <Results>\\<RunId> as pulled from the lab (batch-NNNN\\cases.tsv, summary.json, ...).
REF_RUN_DIR is the same layout from another machine (this PC's reference run). Batch boundaries may differ;
the comparison is per case. Writes OUT_DIR (default RESULTS_RUN_DIR\\merged): merged.tsv (one row per case
of the list, status NotRunYet for cases without a result), summary.json and summary.md.
Exit 0 when every case has a result and all are Pass/NotSupported (warnings listed), 1 otherwise.
"""

import argparse
import glob
import json
import os
import sys
from collections import Counter

CLEAN = {"Pass", "NotSupported"}
WARN = {"QualityWarning", "CompatibilityWarning"}


def load_run(run_dir):
    res, batches = {}, []
    for d in sorted(glob.glob(os.path.join(run_dir, "batch-*")) + glob.glob(os.path.join(run_dir, "list-*"))):
        tsv = os.path.join(d, "cases.tsv")
        if os.path.exists(tsv):
            with open(tsv, encoding="utf-8-sig") as f:
                head = f.readline().rstrip("\n").split("\t")
                for line in f:
                    v = line.rstrip("\n").split("\t")
                    if len(v) >= 2:
                        res[v[0]] = dict(zip(head, v))
        sj = os.path.join(d, "summary.json")
        if os.path.exists(sj):
            with open(sj, encoding="utf-8-sig") as f:
                s = json.load(f)
            batches.append({"batch": s.get("batch"), "complete": s.get("complete"), "exit": s.get("exit"),
                            "done": s.get("done"), "total": s.get("total"), "attempts": s.get("attempts"),
                            "stop_reason": s.get("stop_reason"), "icd_sha256": s.get("icd_sha256"),
                            "route": s.get("route")})
    return res, batches


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("--cases", required=True)
    ap.add_argument("--reference")
    ap.add_argument("--out")
    a = ap.parse_args()
    out = a.out or os.path.join(a.run_dir, "merged")
    os.makedirs(out, exist_ok=True)

    with open(a.cases, encoding="utf-8") as f:
        cases = [l.strip() for l in f if l.strip()]
    res, batches = load_run(a.run_dir)
    ref, _ = load_run(a.reference) if a.reference else ({}, [])

    counts = Counter()
    failing, warnings = [], []
    total_us = 0.0
    with open(os.path.join(out, "merged.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("case\tstatus\tduration_us\treference_status\treference_duration_us\tdetails\n")
        for c in cases:
            r = res.get(c)
            s = r["status"] if r else "NotRunYet"
            counts[s] += 1
            us = r.get("duration_us", "") if r else ""
            if us:
                total_us += float(us)
            if s in WARN:
                warnings.append((s, c))
            elif s not in CLEAN and s != "NotRunYet":
                failing.append((s, c, r.get("details", "") if r else ""))
            rr = ref.get(c)
            f.write(f"{c}\t{s}\t{us}\t{rr['status'] if rr else ''}\t{rr.get('duration_us', '') if rr else ''}\t"
                    f"{r.get('details', '') if r else ''}\n")
    extra = sorted(set(res) - set(cases))

    cmp = Counter()
    diffs = {"pass_ref_not_pass": [], "notsupported_here_pass_ref": [], "pass_here_notsupported_ref": []}
    if ref:
        for c in cases:
            s = res[c]["status"] if c in res else "NotRunYet"
            t = ref[c]["status"] if c in ref else "NotRunYet"
            cmp[(s, t)] += 1
            if s == "NotRunYet" or t == "NotRunYet":
                continue
            if t == "Pass" and s == "NotSupported":
                diffs["notsupported_here_pass_ref"].append(c)
            elif t == "NotSupported" and s == "Pass":
                diffs["pass_here_notsupported_ref"].append(c)
            elif t == "Pass" and s != "Pass":
                diffs["pass_ref_not_pass"].append(f"{s} {c}")

    summary = {
        "run_dir": os.path.abspath(a.run_dir), "cases": len(cases), "with_result": len(cases) - counts["NotRunYet"],
        "counts": dict(counts), "failing": [f"{s} {c}" for s, c, _ in failing],
        "warnings": [f"{s} {c}" for s, c in warnings], "extra_cases_not_in_list": extra,
        "test_duration_sum_s": round(total_us / 1e6, 1), "batches": batches,
        "reference": os.path.abspath(a.reference) if a.reference else None,
        "comparison": {f"{s} | ref {t}": n for (s, t), n in sorted(cmp.items())},
        "differences": {k: v for k, v in diffs.items()},
    }
    with open(os.path.join(out, "summary.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(summary, f, indent=1)
        f.write("\n")

    lines = [f"# Sparse CTS results: {os.path.basename(os.path.abspath(a.run_dir))}", "",
             f"Cases in list: {len(cases)}, with a result: {summary['with_result']}, "
             f"sum of TestDuration: {summary['test_duration_sum_s']} s", "",
             "| status | cases |", "|---|---|"]
    lines += [f"| {k} | {v} |" for k, v in counts.most_common()]
    stopped = [b for b in batches if b.get("stop_reason")]
    incomplete = [b for b in batches if not b.get("complete")]
    lines += ["", f"Batches seen: {len(batches)}, incomplete: {len(incomplete)}, stopped: {len(stopped)}"]
    for b in stopped:
        lines.append(f"- {b['batch']}: STOP {b['stop_reason']}")
    if failing:
        lines += ["", "## Not Pass/NotSupported", ""] + [f"- {s} `{c}` {d}" for s, c, d in failing]
    if warnings:
        lines += ["", "## Warnings", ""] + [f"- {s} `{c}`" for s, c in warnings]
    if ref:
        lines += ["", "## Against the reference", "", "| this run | reference | cases |", "|---|---|---|"]
        lines += [f"| {s} | {t} | {n} |" for (s, t), n in sorted(cmp.items())]
        for k, v in diffs.items():
            lines += ["", f"{k}: {len(v)}"] + [f"- {x}" for x in v[:200]]
            if len(v) > 200:
                lines.append(f"- ... {len(v) - 200} more in summary.json")
    with open(os.path.join(out, "summary.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines[:12 + len(counts)]))
    ok = counts["NotRunYet"] == 0 and not failing
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
