"""Run a /MD test-shared-log.cpp build against its actual shared-log.h.

Usage: python test-shared-log.py EXE NEW_OUTPUT_DIRECTORY
The executable takes no UI/GPU actions. Each child terminates itself without
CRT teardown. Full buffering is the required visibility-negative control.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

exe, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=False)
results = []
for mode in ("legacy", "default", "closed", "buffered"):
    log = out / (mode + ".txt")
    run = subprocess.run([str(exe), mode, str(log)], capture_output=True, text=True, timeout=30)
    data = log.read_bytes() if log.exists() else b""
    rows = data.splitlines()
    results.append(dict(mode=mode, exit=run.returncode, stdout=run.stdout,
                        stderr=run.stderr, bytes=len(data), lines=len(rows),
                        sha256=hashlib.sha256(data).hexdigest()))
    (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    assert run.returncode == (10 if mode == "buffered" else 0), results[-1]
    if mode != "buffered":
        assert len(rows) == 1000
        assert all(("seq=%d " % i).encode() in row for i, row in enumerate(rows))
    else:
        assert not data, "Full buffering unexpectedly survived forced termination"
assert len({r["sha256"] for r in results[:3]}) == 1
print(json.dumps(results, indent=2))
