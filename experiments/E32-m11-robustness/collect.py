"""Collect a terminal E32 run and independent final readback. Never starts tests."""
import argparse
import base64
import io
import json
import re
import sys
import tarfile
from pathlib import Path, PureWindowsPath

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/win"))
from target import Target


def remote(target, code, timeout=30, binary=False):
    encoded = base64.b64encode(code.encode("utf-16-le")).decode()
    p = target.ssh("powershell -NoProfile -EncodedCommand " + encoded, timeout=timeout, binary=binary)
    if p.returncode:
        raise RuntimeError("Run still active; refusing terminal collection" if "Run still active" in p.stderr else "Remote collection check failed; no terminal state established")
    return p.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", help="Existing run identifier, e.g. soak-01")
    parser.add_argument("destination", type=Path, help="New local directory inside workspace")
    args = parser.parse_args()
    if not re.fullmatch("[a-z0-9-]+", args.run):
        parser.error("Invalid run identifier")
    destination = args.destination.resolve()
    if not destination.is_relative_to(ROOT.parent) or destination.exists():
        parser.error("Destination must be new and inside workspace")
    target = Target()
    out = "C:\\BC250\\m11\\" + args.run
    code = r"""
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
$out='OUT'
$launch=Get-Content "$out\launch.json" -Raw | ConvertFrom-Json
$tasks=@($launch.worker_task,$launch.monitor_task)
$active=@($tasks | ForEach-Object {Get-ScheduledTask $_} | Where-Object State -in @('Running','Queued'))
if($active.Count){throw 'Run still active; do not collect as terminal'}
@{readback_exists=(Test-Path "$out\final-readback.json")} | ConvertTo-Json -Compress
""".replace("OUT", out)
    state = json.loads(remote(target, code))
    if not state["readback_exists"]:
        result = target.run_script(str(Path(__file__).with_name("final-readback.ps1")),
                                   ["-Out", out], timeout=60)
        if result.returncode:
            raise RuntimeError("Final readback failed; no acceptance, no restart")
    destination.mkdir()
    # The completed run directory contains only our test artifacts, never memory dumps.
    p = target.ssh('cmd /c "tar -cf - -C ' + out + ' ."', timeout=180, binary=True)
    if p.returncode:
        raise RuntimeError("Terminal artifact transfer failed; preserve partial collection")
    with tarfile.open(fileobj=io.BytesIO(p.stdout)) as archive:
        for member in archive:
            path = (destination / member.name).resolve()
            if not path.is_relative_to(destination):
                raise ValueError("Archive path escaped destination")
            if member.isfile():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(archive.extractfile(member).read())
    inputs = json.loads((destination / "inputs.json").read_text(encoding="utf-8-sig"))
    for item in inputs:
        path = PureWindowsPath(item["Path"])
        if path.suffix.lower() != ".ps1":
            continue
        if path.parent != PureWindowsPath("C:/BC250/m11"):
            raise ValueError("Unexpected runner source location")
        target.pull(str(path), str(destination / path.name), timeout=20)
    print("Collected terminal run:", args.run)
    print("Audit artifacts with audit.py; this command does not accept M11.")


if __name__ == "__main__":
    main()
