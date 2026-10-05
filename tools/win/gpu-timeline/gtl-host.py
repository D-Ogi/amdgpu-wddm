#!/usr/bin/env python3
"""gtl-host.py - the development-PC side of a gpu-timeline lab run, through tools/win/target.py (for the operator).

    python tools/win/gpu-timeline/gtl-host.py push           exe + lab script to C:\\BC250\\gpu-timeline
    python gtl-host.py run TAG [--seconds N] [--hz H] [--set full|lite] [--probe-only] [--detach]
    python gtl-host.py stop TAG                              end a running sample early (it still writes its file)
    python gtl-host.py pull TAG                              receipt, texts and run.gtl to runs\\TAG, hash checked

The exe and the runs stay outside this repository: BC250_GTL_BUILD and BC250_GTL_RUNS name them, by default
<BC250_ROOT>/scratch/build/gpu-timeline and <BC250_ROOT>/scratch/build/gpu-timeline/runs.

`run` passes the local exe's SHA256 as -ExpectExeSha256, so the lab refuses a stale copy. Without --detach the
SSH session is held for the run (at most 60 s plus the probe); with --detach a one-shot SYSTEM task runs it and the
call returns at once. Then: python analyze.py runs\\TAG\\run.gtl [--fps F] [--json runs\\TAG\\result.json]
"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
# bc250-win/tools/win/gpu-timeline/gtl-host.py: target.py is one level up, the workspace root four.
sys.path.insert(0, str(HERE.parent))
import target  # noqa: E402

ROOT = Path(os.environ.get("BC250_ROOT") or HERE.parents[3].parent)
REMOTE_DIR = "C:\\BC250\\gpu-timeline"
REMOTE_OUT = "C:\\BC250\\tmp\\gtl"
BUILD = Path(os.environ.get("BC250_GTL_BUILD") or ROOT / "scratch/build/gpu-timeline")
RUNS = Path(os.environ.get("BC250_GTL_RUNS") or BUILD / "runs")
EXE = BUILD / "gpu-timeline.exe"
SCRIPT = HERE / "lab" / "gtl-run.ps1"


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest().upper()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("push")
    r = sub.add_parser("run")
    r.add_argument("tag")
    r.add_argument("--seconds", type=int, default=60)
    r.add_argument("--hz", type=int, default=1009)
    r.add_argument("--set", default="full", choices=["full", "lite"])
    r.add_argument("--probe-only", action="store_true")
    r.add_argument("--detach", action="store_true")
    for name in ("stop", "pull"):
        sub.add_parser(name).add_argument("tag")
    args = ap.parse_args(argv)
    if getattr(args, "tag", None) and not all(c.isalnum() or c in "_-" for c in args.tag):
        sys.exit("tag: letters, digits, '_' and '-' only")
    t = target.Target()

    if args.cmd == "push":
        print("\n".join(t.push([str(EXE), str(SCRIPT)], REMOTE_DIR)))
        print(f"gpu-timeline.exe SHA256 {sha256(EXE)}")
        return 0
    if args.cmd == "run":
        if not 1 <= args.seconds <= 60:
            sys.exit("--seconds must be 1..60")
        a = ["-Tag", args.tag, "-Seconds", str(args.seconds), "-Hz", str(args.hz), "-Set", args.set,
             "-Exe", REMOTE_DIR + "\\gpu-timeline.exe", "-ExpectExeSha256", sha256(EXE)]
        if args.probe_only:
            a.append("-ProbeOnly")
        if args.detach:
            a.append("-Detach")
        done = t.run_script(str(SCRIPT), a, timeout=args.seconds + 120, remote_dir=REMOTE_DIR)
        sys.stdout.write(done.stdout)
        sys.stderr.write(done.stderr)
        return done.returncode
    if args.cmd == "stop":
        done = t.run(f'New-Item -ItemType File -Force -Path "{REMOTE_OUT}\\{args.tag}\\stop" | Out-Null; "stop file set"')
        print(done.strip() if isinstance(done, str) else done)
        return 0
    if args.cmd == "pull":
        local = RUNS / args.tag
        local.mkdir(parents=True, exist_ok=True)
        for name in ("meta.json", "probe.txt", "probe-err.txt", "sample.txt", "sample-err.txt", "run.gtl"):
            try:
                t.pull(f"{REMOTE_OUT}\\{args.tag}\\{name}", str(local / name))
                print(f"pulled {name}")
            except target.TargetError as e:
                print(f"{name}: {e}")
        meta = json.loads((local / "meta.json").read_text(encoding="ascii"))
        if (local / "run.gtl").exists():
            ok = meta.get("run_sha256") == sha256(local / "run.gtl")
            print(f"run.gtl SHA256 {'matches' if ok else 'DOES NOT MATCH'} the lab receipt")
            return 0 if ok else 1
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
