"""Cut the sparse case list into lab batches that each finish well inside the 3-minute trial bound.

    python plan_batches.py --cases lists/sparse-resources.txt --host host-ref/durations.tsv
                           --lab-history lists/lab-history-2026-09-25.tsv --out batches

Estimate of one case on unit A = scale * host duration (TestDuration of this PC's reference run).
Cases NotSupported on the host because it has one physical device (device_group_*) keep their host time:
unit A has one GPU too. Other cases without a host Pass (NotSupported on the host GPU for format/sample
reasons) get the median host duration of the passing cases of their group (first five name components,
then four, then all) - the lab may support what the host does not, so they are priced as if they run.
scale = max(--min-scale, --ratio-quantile of lab/host ratios over the cases that both the earlier unit A run
(2026-09-25) and this host passed), so the measured ratio can only raise the assumption.
A batch is closed before its estimate (startup + cases) would pass --budget-s or it would hold more than
--max-cases cases. Case order is the mustpass order; batches are contiguous slices of it.
"""

import argparse
import hashlib
import json
import os
import statistics


def read_list(path):
    with open(path, encoding="utf-8") as f:
        return [l.strip() for l in f if l.strip() and not l.startswith("#")]


def read_tsv(path):
    rows = {}
    with open(path, encoding="utf-8") as f:
        head = f.readline().rstrip("\n").split("\t")
        for line in f:
            v = line.rstrip("\n").split("\t")
            rows[v[0]] = dict(zip(head, v))
    return rows


def group(case, n):
    return ".".join(case.split(".")[:n])


def quantile(values, q):
    v = sorted(values)
    if not v:
        return None
    k = min(len(v) - 1, max(0, int(round(q * (len(v) - 1)))))
    return v[k]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cases", required=True)
    ap.add_argument("--host", required=True, help="TSV case/status/duration_us of the host reference")
    ap.add_argument("--lab-history", help="TSV case/status/duration_us of an earlier unit A run")
    ap.add_argument("--out", required=True)
    ap.add_argument("--min-scale", type=float, default=10.0)
    ap.add_argument("--ratio-quantile", type=float, default=0.9)
    ap.add_argument("--startup-s", type=float, default=10.0, help="lab process start + instance/device creation")
    ap.add_argument("--per-case-s", type=float, default=0.02, help="lab fixed cost per case (log, setup)")
    ap.add_argument("--budget-s", type=float, default=90.0)
    ap.add_argument("--max-cases", type=int, default=600)
    ap.add_argument("--prefix", default="sparse")
    a = ap.parse_args()

    cases = read_list(a.cases)
    host = read_tsv(a.host)
    passed = {c: float(r["duration_us"]) / 1e6 for c, r in host.items()
              if r.get("status") == "Pass" and r.get("duration_us")}
    # NotSupported for a reason that holds on unit A as well (one physical device): priced at what the host
    # measured, since the lab answers the same way.
    same_answer = {c: float(r["duration_us"]) / 1e6 for c, r in host.items()
                   if r.get("status") == "NotSupported" and r.get("duration_us")
                   and "1 physical device" in r.get("details", "")}

    ratios = []
    if a.lab_history:
        for c, r in read_tsv(a.lab_history).items():
            if r.get("status") == "Pass" and c in passed and passed[c] > 0:
                ratios.append(float(r["duration_us"]) / 1e6 / passed[c])
    measured = quantile(ratios, a.ratio_quantile) if ratios else None
    scale = max(a.min_scale, measured or 0.0)

    medians = {}
    for n in (5, 4):
        acc = {}
        for c, s in passed.items():
            acc.setdefault(group(c, n), []).append(s)
        medians[n] = {g: statistics.median(v) for g, v in acc.items()}
    global_median = statistics.median(passed.values()) if passed else 0.05

    def host_estimate(c):
        if c in passed:
            return passed[c], "host"
        if c in same_answer:
            return same_answer[c], "host-notsupported-1-device"
        for n in (5, 4):
            g = group(c, n)
            if g in medians[n]:
                return medians[n][g], f"group{n}"
        return global_median, "global"

    os.makedirs(a.out, exist_ok=True)
    batches, cur, cur_est = [], [], a.startup_s
    sources = {}
    for c in cases:
        h, src = host_estimate(c)
        sources[src] = sources.get(src, 0) + 1
        est = scale * h + a.per_case_s
        if cur and (cur_est + est > a.budget_s or len(cur) >= a.max_cases):
            batches.append((cur, cur_est))
            cur, cur_est = [], a.startup_s
        cur.append(c)
        cur_est += est
    if cur:
        batches.append((cur, cur_est))

    manifest = {
        # The path as given, not absolute: the manifest travels to the lab and into the repository.
        "cases_file": a.cases.replace("\\", "/"),
        "cases_sha256": hashlib.sha256(open(a.cases, "rb").read()).hexdigest().upper(),
        "case_count": len(cases),
        "scale": scale,
        "measured_ratio": {"quantile": a.ratio_quantile, "value": measured, "n": len(ratios),
                           "median": statistics.median(ratios) if ratios else None,
                           "max": max(ratios) if ratios else None},
        "startup_s": a.startup_s, "per_case_s": a.per_case_s, "budget_s": a.budget_s,
        "max_cases": a.max_cases, "estimate_sources": sources,
        "batches": [],
    }
    for i, (bc, est) in enumerate(batches, 1):
        name = f"{a.prefix}-{i:04d}.txt"
        data = ("\n".join(bc) + "\n").encode("utf-8")
        with open(os.path.join(a.out, name), "wb") as f:
            f.write(data)
        manifest["batches"].append({"index": i, "file": name, "count": len(bc),
                                    "sha256": hashlib.sha256(data).hexdigest().upper(),
                                    "estimate_lab_s": round(est, 1),
                                    "first": bc[0], "last": bc[-1]})
    total = sum(len(b) for b, _ in batches)
    assert total == len(cases)
    with open(os.path.join(a.out, "manifest.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=1)
        f.write("\n")
    ests = [e for _, e in batches]
    print(f"{len(batches)} batches, {total} cases, scale {scale:.2f} (measured q{a.ratio_quantile} "
          f"{measured}, n={len(ratios)}), estimate per batch max {max(ests):.1f} s, "
          f"sum {sum(ests)/60:.1f} min, sources {sources}")


if __name__ == "__main__":
    main()
