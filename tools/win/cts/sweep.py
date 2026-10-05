"""Drive run-batch.ps1 over a range of batch indices, one bounded call at a time.

    python sweep.py --run-id icd85077e29 --first 1 --last 147 [--max-calls 20] [-- extra runner args]
    python sweep.py --local --run-id X --first 1 --last 3 -- -Root <package dir> -Route registered ...
        (runs run-batch.ps1 on this PC instead, for testing; README "Offline checks")

Per call: exit 0/1 -> next index; 2 -> the same index again (resume); 3 -> stop the sweep and print the
runner's last lines. --max-calls bounds the number of runner calls of this invocation (each call is one
<= 170 s lab trial). Every call is logged to sweep-<run-id>.log in the work directory, which is
<BC250_ROOT>/scratch/cts by default (BC250_CTS_WORK overrides it, --work-dir wins over both). A file
sweep.stop in that directory stops the sweep before the next batch, as the overlay STOP flag does.
The lab side needs the package at C:\\BC250\\cts (README "Lab install").
"""

import argparse
import datetime
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# BC250_ROOT is the workspace root; by default the parent directory of this repository
# (this file is tools/win/cts/sweep.py, so the repository root is three levels up from here).
ROOT = os.environ.get("BC250_ROOT", os.path.normpath(os.path.join(HERE, "..", "..", "..", "..")))
TARGET = os.path.normpath(os.path.join(HERE, "..", "target.py"))
RUNNER = os.path.join(HERE, "run-batch.ps1")
PS51 = r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run-id", required=True)
    ap.add_argument("--first", type=int, required=True)
    ap.add_argument("--last", type=int, required=True)
    ap.add_argument("--max-calls", type=int, default=0)
    ap.add_argument("--max-resumes", type=int, default=5, help="calls per index before giving up")
    ap.add_argument("--work-dir", help="logs and the sweep.stop file; never inside the repository")
    ap.add_argument("--local", action="store_true")
    ap.add_argument("extra", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    extra = [x for x in a.extra if x != "--"]
    work = a.work_dir or os.environ.get("BC250_CTS_WORK") or os.path.join(ROOT, "scratch", "cts")
    os.makedirs(work, exist_ok=True)
    log = os.path.join(work, f"sweep-{a.run_id}.log")
    calls, index, resumes = 0, a.first, 0
    while index <= a.last:
        if a.max_calls and calls >= a.max_calls:
            print(f"max calls reached; next index {index}")
            return 0
        # Lab calls honour the owner's STOP flag on the overlay (and the operator's stop file) before each batch.
        if not a.local:
            mon = os.path.join(os.path.dirname(TARGET), "bc250mon", "mon.py")
            stop = subprocess.run([sys.executable, mon, "stop?"], capture_output=True, text=True, errors="replace")
            if "no stop request" not in stop.stdout or os.path.exists(os.path.join(work, "sweep.stop")):
                print(f"stop requested before batch {index}; rerun with --first {index}")
                return 4
        args = ["-Index", str(index), "-RunId", a.run_id] + extra
        if a.local:
            cmd = [PS51, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", RUNNER] + args
        else:
            cmd = [sys.executable, TARGET, "ps", RUNNER] + args
        start = datetime.datetime.now(datetime.timezone.utc)
        env = dict(os.environ)
        if a.local:  # this PC: keep every temp file off C:
            tmp = os.path.join(work, "tmp")
            os.makedirs(tmp, exist_ok=True)
            env["TEMP"] = env["TMP"] = tmp
        p = subprocess.run(cmd, capture_output=True, text=True, errors="replace", env=env)
        calls += 1
        out = (p.stdout + p.stderr).strip()
        with open(log, "a", encoding="utf-8") as f:
            f.write(f"{start:%Y-%m-%dT%H:%M:%SZ} index {index} exit {p.returncode}\n{out}\n")
        print(f"{start:%H:%M:%SZ} batch {index}: exit {p.returncode} | {out.splitlines()[0] if out else ''}")
        # target.py does not pass the runner's exit code through (an incomplete batch, 2, came back as 1 and the
        # sweep moved on, batch 30 of icd85077e29): the runner's own summary line decides when it is there.
        m = re.search(r"\bexit=(\d+) complete=(True|False)\b", out)
        code = int(m.group(1)) if m else p.returncode
        if code in (0, 1):
            index, resumes = index + 1, 0
        elif code == 2:
            resumes += 1
            if resumes >= a.max_resumes:
                print(f"batch {index} still incomplete after {resumes} calls; stopping")
                return 2
        else:
            print(out)
            print(f"stopped at batch {index} (exit {code}); inspect, then rerun with --first {index}")
            return 3
    print(f"done: batches {a.first}..{a.last}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
