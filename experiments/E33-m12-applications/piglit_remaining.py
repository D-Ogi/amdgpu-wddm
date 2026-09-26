"""Prepare an exact remainder after a reviewed terminal piglit stop.

Earlier outcomes remain required inputs to final comparison; this is not a waiver.
Timeouts, crashes and incomplete cases cannot be continued with this tool.
"""
import argparse
import hashlib
import json
from pathlib import Path
from compare_piglit import read_results, inventory


def remaining(names, results, reviewed):
    if not names or len(names) != len(set(names)):
        raise ValueError("Empty or duplicate inventory")
    if set(results) - set(names):
        raise ValueError("Unexpected previous case")
    stops = {name for name, row in results.items() if row["result"] not in {"pass", "skip"}}
    if stops != set(reviewed):
        raise ValueError("Every non-pass stop must be reviewed exactly")
    if any(results[name]["result"] not in {"warn", "fail"} for name in stops):
        raise ValueError("Cannot continue crash, timeout or unfinished case")
    return [name for name in names if name not in results]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cases", type=Path)
    parser.add_argument("previous", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--reviewed-stop", action="append", default=[])
    args = parser.parse_args()
    inventory(args.cases)
    names = [s.strip() for s in args.cases.read_text(encoding="utf-8-sig").splitlines() if s.strip() and not s.lstrip().startswith("#")]
    results = read_results(args.previous)
    todo = remaining(names, results, args.reviewed_stop)
    args.out.mkdir()
    (args.out / "remaining.txt").write_text("".join(name + "\n" for name in todo), encoding="utf-8", newline="\n")
    report = {"total": len(names), "previous": len(results), "remaining": len(todo),
              "reviewed_outcomes_retained": {name: results[name] for name in args.reviewed_stop},
              "previous_sha256": hashlib.sha256(args.previous.read_bytes()).hexdigest(),
              "inventory_sha256": hashlib.sha256(args.cases.read_bytes()).hexdigest(),
              "complete": not todo, "earlier_results_required": True}
    (args.out / "partition.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: report[k] for k in ["total", "previous", "remaining"]}))


if __name__ == "__main__":
    main()
