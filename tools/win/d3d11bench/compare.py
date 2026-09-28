"""Hold d3d11bench results of the system D3D path against the per-application DXVK path.

The M14 bound (D004): on the same workload, settings, clocks and builds, the system path (Microsoft runtime plus
the project's UMD) may be at most 5 % slower than per-application DXVK. d3d11bench measures one path per run;
this script compares the two sides.

    python compare.py --base per-app-1.json per-app-2.json per-app-3.json \
                      --candidate system-1.json system-2.json system-3.json [--bound 0.05]

Per scene it takes the median of the per-run medians of the gated metric (frame_ms for draws and fill, total_ms
for shaders) on each side, and the run-to-run spread of each side ((max - min) / median of the per-run values).
The result has to clear the bound by the larger spread:
  PASS          candidate / base - 1 <= bound - spread
  FAIL          candidate / base - 1 >  bound + spread, or the output checksums of the scene differ
  INCONCLUSIVE  anything in between: the runs cannot tell
Other metrics (GPU time, recording and present cost, shader creation) are printed for diagnosis, not gated.

Exit: 0 every scene PASS, 1 a scene FAIL, 2 unusable input, 3 INCONCLUSIVE and no FAIL.
"""

import argparse
import json
import statistics
import sys

GATED = {"draws": "frame_ms", "fill": "frame_ms", "shaders": "total_ms"}
INFO = {
    "draws": ("gpu_ms", "record_ms", "present_ms"),
    "fill": ("gpu_ms", "record_ms", "present_ms"),
    "shaders": ("create_ms", "draw_ms"),
}
RUN_SETTINGS = ("mode", "width", "height", "feature_level")
SCENE_SETTINGS = ("frames", "warmup", "draws", "layers", "shaders")


class InputError(Exception):
    pass


def metric(scene, name):
    """A scene metric: the median of a statistics object, or a plain number."""
    value = scene.get(name)
    if isinstance(value, dict):
        value = value.get("median")
    if not isinstance(value, (int, float)):
        raise InputError(f"scene {scene.get('name')}: no {name}")
    return float(value)


def settings(run):
    adapter = run.get("adapter", {})
    return {
        "run": {key: run.get(key) for key in RUN_SETTINGS},
        "adapter": (adapter.get("vendor"), adapter.get("device")),
        "scenes": {
            scene["name"]: {key: scene[key] for key in SCENE_SETTINGS if key in scene}
            for scene in run.get("scenes", [])
        },
    }


def configuration(run):
    return {"environment": run.get("environment", {}), "dxvk_conf": run.get("dxvk_conf", {})}


def drivers(run):
    """SHA-256 of every loaded Vulkan driver. Paths differ between the sides (the loader's registered ICD against
    the UMD's own copy), so only the file hashes say whether both ran the same driver."""
    return sorted(icd.get("sha256") or "unknown" for icd in run.get("icds", []))


def check_side(runs, label, path):
    if not runs:
        raise InputError(f"{label}: no runs")
    for run in runs:
        name = run.get("_file", label)
        if run.get("tool") != "d3d11bench" or run.get("format") != 1:
            raise InputError(f"{name}: not a d3d11bench format 1 result")
        if run.get("result") != "measured":
            raise InputError(f"{name}: the run did not complete (exit {run.get('exit')})")
        if path and run.get("d3d11") != path:
            raise InputError(f"{name}: d3d11.dll is {run.get('d3d11')}, expected {path} for the {label} side")


def spread(values):
    middle = statistics.median(values)
    return (max(values) - min(values)) / middle if middle > 0 else float("inf")


def compare(base, candidate, bound=0.05, check_paths=True, ignore_configuration=False):
    """Returns (rows, exit code). Raises InputError when the runs cannot be compared."""
    check_side(base, "base", "app-local" if check_paths else None)
    check_side(candidate, "candidate", "system" if check_paths else None)

    reference = settings(base[0])
    for run in base + candidate:
        if settings(run) != reference:
            raise InputError(f"{run.get('_file', 'a run')}: settings differ from {base[0].get('_file', 'the first run')}")
    if not ignore_configuration:
        for run in base + candidate:
            if configuration(run) != configuration(base[0]):
                raise InputError(f"{run.get('_file', 'a run')}: environment (DXVK, Vulkan loader, Mesa) or dxvk.conf differs "
                                 f"(same configuration on both paths, or --ignore-configuration)")
            if drivers(run) != drivers(base[0]):
                raise InputError(f"{run.get('_file', 'a run')}: loaded Vulkan drivers {drivers(run)} differ from "
                                 f"{drivers(base[0])} (same ICD file on both paths, or --ignore-configuration)")

    rows = []
    for name in reference["scenes"]:
        gated = GATED.get(name)
        if gated is None:
            raise InputError(f"unknown scene {name}")
        scenes = {
            side: [next(s for s in run["scenes"] if s["name"] == name) for run in runs]
            for side, runs in (("base", base), ("candidate", candidate))
        }
        values = {side: [metric(s, gated) for s in scenes[side]] for side in scenes}
        base_value = statistics.median(values["base"])
        candidate_value = statistics.median(values["candidate"])
        if base_value <= 0:
            raise InputError(f"scene {name}: base {gated} is {base_value}")
        change = candidate_value / base_value - 1.0
        margin = max(spread(values["base"]), spread(values["candidate"]))
        checksums = {s.get("checksum") for side in scenes for s in scenes[side]}

        if len(checksums) != 1:
            verdict = "FAIL"
            reason = "output differs"
        elif change > bound + margin:
            verdict, reason = "FAIL", ""
        elif change <= bound - margin:
            verdict, reason = "PASS", ""
        else:
            verdict, reason = "INCONCLUSIVE", "spread too large for the bound"

        info = {}
        for extra in INFO[name]:
            try:
                b = statistics.median(metric(s, extra) for s in scenes["base"])
                c = statistics.median(metric(s, extra) for s in scenes["candidate"])
            except InputError:
                continue
            info[extra] = (b, c)

        rows.append({
            "scene": name, "metric": gated, "base": base_value, "candidate": candidate_value, "change": change,
            "margin": margin, "verdict": verdict, "reason": reason, "info": info,
            "runs": (len(base), len(candidate)),
        })

    verdicts = {row["verdict"] for row in rows}
    code = 1 if "FAIL" in verdicts else 3 if "INCONCLUSIVE" in verdicts else 0
    return rows, code


def load(paths):
    runs = []
    for path in paths:
        try:
            with open(path, encoding="utf-8") as f:
                run = json.load(f)
        except (OSError, ValueError) as e:
            raise InputError(f"{path}: {e}") from e
        run["_file"] = path
        runs.append(run)
    return runs


def main(argv=None):
    parser = argparse.ArgumentParser(description="Hold d3d11bench results against the M14 5 % bound.")
    parser.add_argument("--base", nargs="+", required=True, help="per-application DXVK runs")
    parser.add_argument("--candidate", nargs="+", required=True, help="system path runs")
    parser.add_argument("--bound", type=float, default=0.05, help="allowed slowdown (default 0.05)")
    parser.add_argument("--any-path", action="store_true",
                        help="do not require app-local d3d11.dll for the base and the system one for the candidate")
    parser.add_argument("--ignore-configuration", action="store_true",
                        help="compare although DXVK_*, VK_*, MESA_*, RADV_*, ACO_*, BC250_* variables, dxvk.conf or "
                             "the loaded Vulkan drivers differ")
    args = parser.parse_args(argv)

    try:
        rows, code = compare(load(args.base), load(args.candidate), args.bound, not args.any_path,
                             args.ignore_configuration)
    except InputError as e:
        print(f"unusable input: {e}")
        return 2

    print(f"bound {args.bound:.1%}; runs base/candidate {rows[0]['runs'][0]}/{rows[0]['runs'][1]}" if rows else "no scenes")
    for row in rows:
        print(f"{row['scene']:8} {row['metric']:9} base {row['base']:9.4f} ms  candidate {row['candidate']:9.4f} ms  "
              f"{row['change']:+7.2%}  spread {row['margin']:6.2%}  {row['verdict']}"
              + (f" ({row['reason']})" if row["reason"] else ""))
        for name, (b, c) in row["info"].items():
            change = f"{c / b - 1.0:+7.2%}" if b > 0 else "      -"
            print(f"         {name:9} base {b:9.4f} ms  candidate {c:9.4f} ms  {change}")
    return code


if __name__ == "__main__":
    sys.exit(main())
