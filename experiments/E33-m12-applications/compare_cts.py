"""Compare normalized CTS JSONL against an exact, flat case list.

Input rows use the existing worker format: {"case": "dEQP-VK...", "status": "Pass"}.
This compares results, not hardware identity, conformance eligibility or explanations.
The caller must independently verify same board, CTS and Mesa revisions.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import sqlite3
import tempfile

STATUSES = {"Pass", "Fail", "QualityWarning", "CompatibilityWarning", "Pending",
            "NotSupported", "ResourceError", "InternalError", "Crash", "Timeout", "DeviceLost", "CapabilityWarning", "Waiver"}


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def compare(cases, windows, linux, output):
    output.mkdir(parents=True, exist_ok=False)
    # Disk-backed joins keep full mustpass comparisons independent of RAM size.
    with tempfile.TemporaryDirectory(prefix="cts-compare-", dir=output) as temporary:
        db = sqlite3.connect(str(Path(temporary) / "cases.db"))
        try:
            db.executescript("CREATE TABLE expected(name TEXT PRIMARY KEY);"
                             "CREATE TABLE result(side TEXT, name TEXT, status TEXT, PRIMARY KEY(side,name));")
            with cases.open(encoding="utf-8-sig") as stream:
                for line in stream:
                    name = line.strip()
                    if not name or name.startswith("#"):
                        continue
                    if not name.startswith("dEQP-VK.") or any(c.isspace() for c in name):
                        raise ValueError("Expected a flat Vulkan case list")
                    db.execute("INSERT INTO expected VALUES (?)", (name,))
            expected = db.execute("SELECT count(*) FROM expected").fetchone()[0]
            if not expected:
                raise ValueError("Empty case list")
            counts = {}
            for side, path in (("windows", windows), ("linux", linux)):
                counts[side] = Counter()
                with path.open(encoding="utf-8-sig") as stream:
                    for number, line in enumerate(stream, 1):
                        if not line.strip():
                            continue
                        row = json.loads(line)
                        name, status = row["case"], row["status"]
                        if not isinstance(name, str) or not name.startswith("dEQP-VK.") or status not in STATUSES:
                            raise ValueError(f"Invalid result in {side} line {number}")
                        db.execute("INSERT INTO result VALUES (?,?,?)", (side, name, status))
                        counts[side][status] += 1
            db.commit()
            issues = Counter()
            with (output / "differences.jsonl").open("x", encoding="utf-8", newline="\n") as report:
                query = """SELECT e.name,w.status,l.status FROM expected e
                    LEFT JOIN result w ON w.side='windows' AND w.name=e.name
                    LEFT JOIN result l ON l.side='linux' AND l.name=e.name ORDER BY e.name"""
                for name, win, lin in db.execute(query):
                    category = None
                    if win is None or lin is None:
                        category = "missing_result"
                    elif win != lin:
                        category = "status_difference"
                    elif win not in ("Pass", "NotSupported"):
                        category = "matching_nonpassing_result"
                    if category:
                        issues[category] += 1
                        report.write(json.dumps({"case": name, "windows": win, "linux": lin,
                                                 "category": category}) + "\n")
                for side, name, status in db.execute("""SELECT side,name,status FROM result
                        WHERE name NOT IN (SELECT name FROM expected) ORDER BY side,name"""):
                    issues["unexpected_result"] += 1
                    report.write(json.dumps({"case": name, "side": side, "status": status,
                                             "category": "unexpected_result"}) + "\n")
            result = {
                "status": "RESULTS_MATCH" if not issues else "REVIEW_REQUIRED",
                "expected_cases": expected, "counts": counts, "issues": issues,
                "inputs_sha256": {"cases": digest(cases), "windows": digest(windows), "linux": digest(linux)},
                "scope": "Result comparison only; identities, capability skips and difference explanations require review. Not certification.",
            }
            (output / "summary.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
            return result
        finally:
            db.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cases", "windows", "linux", "output"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    result = compare(args.cases, args.windows, args.linux, args.output)
    print(json.dumps(result))
    raise SystemExit(0 if result["status"] == "RESULTS_MATCH" else 1)
