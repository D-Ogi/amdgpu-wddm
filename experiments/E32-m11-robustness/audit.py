"""Audit a collected E32 soak. Read-only: never changes lab state or inputs."""
import argparse
import hashlib
import json
import re
from datetime import datetime
from pathlib import Path, PureWindowsPath

TAGS = ("25Kd", "B2Cg", "B2Gm", "B2Gx", "B2Ih", "B2Pw", "B2Sp", "B2Vs", "B2Ww")
ICD_WITNESS = re.compile(r"C:\\BC250\\m10\\wsi-final\\(?:\.\\)?vulkan_radeon\.dll", re.I)
ROOT = Path(__file__).resolve().parents[2]


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def read_rows(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8-sig").splitlines() if line.strip()]


def utc(value):
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


def pool_values(row):
    values = {(tag, kind): (0, 0) for tag in TAGS for kind in ("Paged", "Nonp")}
    seen = set()
    for item in row["rows"]:
        key = item["tag"], item["type"]
        if key not in values or key in seen:
            raise ValueError("Unknown or duplicate pool row")
        seen.add(key)
        if item["allocs"] - item["frees"] != item["count"]:
            raise ValueError("Pool allocation/free/count mismatch")
        values[key] = item["count"], item["bytes"]
    return values


def audit(folder, reference):
    checks = []
    def check(name, passed, details=None):
        checks.append({"check": name, "pass": bool(passed), "details": details})

    worker = read_json(folder / "worker-result.json")
    monitor = read_json(folder / "monitor-result.json")
    start = read_json(folder / "worker-start.json")
    cycles = read_rows(folder / "cycles.jsonl")
    inputs = read_json(folder / "inputs.json")
    check("worker and monitor terminal PASS", worker.get("status") == monitor.get("status") == "PASS")
    check("24 monotonic hours", worker.get("elapsed_seconds", 0) >= 86400,
          worker.get("elapsed_seconds"))
    check("unlimited 24-hour launch", start.get("duration") == 86400 and start.get("max_cycles") == 0)
    check("UTC elapsed corroborates monotonic duration",
          (utc(worker["utc"]) - utc(start["utc"])).total_seconds() >= 86400)
    count = worker.get("cycles", 0)
    check("complete ordered cycle ledger",
          count > 0 and len(cycles) == count and [r["cycle"] for r in cycles] == list(range(1, count + 1))
          and all(r["result"] == "PASS" for r in cycles))
    check("one monitor checkpoint per cycle", monitor.get("cycles") == count)

    ref_text = (reference / "compute/vkcompute.txt").read_text()
    hashes = dict(re.findall(r"^(\w+)\s+n=\d+\s+hash=0x([a-f0-9]+)", ref_text, re.M))
    check("eight canonical Linux hashes", len(hashes) == 8)
    spv = [item for item in inputs if item["Path"].lower().endswith(".spv")]
    check("nine identical Linux SPIR-V inputs", len(spv) == 9 and all(
        hashlib.sha256((reference / "compute/spv" / item["Path"].split("\\")[-1]).read_bytes()).hexdigest().upper() == item["Hash"]
        for item in spv))
    bad_compute, bad_models, bad_loader, bad_ledger = [], [], [], []
    for cycle in cycles:
        number = cycle["cycle"]
        path = folder / f"cycle-{number:06d}"
        compute = (path / "compute.out").read_text()
        if "8 test(s) run, 0 mismatch(es)" not in compute or any(
            not re.search(r"^" + re.escape(name) + r"\s+n=\d+\s+hash=0x" + value +
                          r"\s+cpu_hash=0x" + value + r"\s+match=yes", compute, re.M)
            for name, value in hashes.items()
        ):
            bad_compute.append(number)
        for model in ("stories15M", "tinyllama"):
            actual = (path / (model + ".out")).read_bytes().replace(b"\r", b"")
            expected = (reference / ("llama/" + model + "-ngl99.out")).read_bytes().replace(b"\r", b"")
            log = (path / (model + ".err")).read_text(errors="replace")
            match = re.search(r"offloaded (\d+)/(\d+) layers to GPU", log)
            if actual != expected or not match or match[1] != match[2]:
                bad_models.append([number, model])
        for name in ("compute", "stories15M", "tinyllama", "cube"):
            if not ICD_WITNESS.search((path / (name + ".err")).read_text(errors="replace")):
                bad_loader.append([number, name])
        for name, expected in cycle["stdout_file_sha256"].items():
            if hashlib.sha256((path / (name + ".out")).read_bytes()).hexdigest().upper() != expected:
                bad_ledger.append([number, name])
    check("every compute result equals CPU and Linux", not bad_compute, bad_compute)
    check("every model text equals Linux with full GPU offload", not bad_models, bad_models)
    check("actual candidate ICD witness in every workload", not bad_loader, bad_loader)
    check("raw output hashes equal cycle ledger", not bad_ledger, bad_ledger)

    samples = read_rows(folder / "pool.jsonl")
    initial = pool_values(samples[0])
    checkpoints = [x for x in samples if x["label"].isdigit()]
    finals = [x for x in samples if x["label"].startswith("final-")]
    check("pool covers every cycle and five final samples",
          samples[0]["label"] == "initial" and
          [int(x["label"]) for x in checkpoints] == list(range(1, count + 1)) and len(finals) == 5)
    growth = []
    baseline = pool_values(checkpoints[0]) if checkpoints else initial
    for row in checkpoints:
        values = pool_values(row)
        for key in values:
            if any(a > b for a, b in zip(values[key], baseline[key])):
                growth.append([row["label"], "/".join(key), values[key], baseline[key]])
    check("no growth beyond first active-worker checkpoint", not growth, growth[:20])
    retained = []
    for row in finals:
        values = pool_values(row)
        for key in values:
            if values[key] != initial[key]:
                retained.append([row["label"], "/".join(key), values[key], initial[key]])
    check("all final pool counts and bytes equal initial values", len(finals) == 5 and not retained, retained[:20])

    thermal = read_rows(folder / "telemetry.jsonl")
    times = [utc(x["utc"]) for x in thermal]
    gaps = [(b-a).total_seconds() for a, b in zip(times, times[1:])]
    temperatures = [x["temperature_mc"] for x in thermal]
    check("temperature samples below 85C",
          bool(temperatures) and min(temperatures) > 0 and max(temperatures) < 85000,
          {"samples": len(thermal), "min_mc": min(temperatures, default=None),
           "max_mc": max(temperatures, default=None), "max_gap_seconds": max(gaps, default=None)})
    check("telemetry has no unexplained gap over 60 seconds",
          bool(gaps) and all(0 < gap <= 60 for gap in gaps) and
          (utc(start["utc"]) - times[0]).total_seconds() >= -10 and
          (utc(worker["utc"]) - times[-1]).total_seconds() <= 60)
    check("one GPU generation and epoch throughout",
          len({(x["generation"], x["epoch"]) for x in thermal}) == 1)
    check("no recorded event/dump failure artifact",
          not any((folder / name).exists() for name in
                  ("failure-events.json", "failure-dumps.txt", "stop.txt")))
    runner_bad = []
    runner_inputs = [x for x in inputs if PureWindowsPath(x["Path"]).suffix.lower() == ".ps1"]
    if len(runner_inputs) != 2:
        runner_bad.append("Expected worker and monitor source inputs")
    for item in runner_inputs:
        name = PureWindowsPath(item["Path"]).name
        path = folder / name
        if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest().upper() != item["Hash"]:
            runner_bad.append(name)
    check("frozen runner sources match input manifest", not runner_bad, runner_bad)
    final = read_json(folder / "final-readback.json") if (folder / "final-readback.json").exists() else {}
    expected_inputs = {x["Path"]: x["Hash"] for x in inputs}
    actual_inputs = {x["path"]: x["actual"] for x in final.get("inputs", [])}
    logs = final.get("event_logs", [])
    check("independent final event/dump/boot/integrity audit",
          final.get("same_boot") is True and final.get("same_dump_inventory") is True and
          final.get("events") == [] and actual_inputs == expected_inputs and
          len(logs) == 2 and {x["name"] for x in logs} == {"System", "Application"} and
          all(x.get("enabled") and x.get("readable") for x in logs),
          "Final readback missing" if not final else None)
    expected_umd = next((x["Hash"] for x in inputs if PureWindowsPath(x["Path"]).name.lower() == "bc250d3d.dll"), None)
    check("expected desktop UMD remains loaded",
          bool(final.get("dwm")) and all(x["hash"] == expected_umd for x in final.get("dwm", [])))
    return {"status": "PASS" if all(x["pass"] for x in checks) else "FAIL",
            "scope": "soak artifacts only; deliberate stuck queue and manual completion audit still required",
            "checks": checks}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("--reference", type=Path, default=ROOT / "evidence/linux/2026-09-21-E14-vulkan-compute-reference")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = audit(args.folder, args.reference)
    text = json.dumps(result, indent=2)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
