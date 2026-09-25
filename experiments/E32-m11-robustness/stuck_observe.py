"""One-shot stuck-queue launch and independent 30-second observation.

Requires a collected, audited 24-hour soak and a separately prepared positive
fill control. Never retries the launch and never power-cycles the lab.
"""
import argparse
import base64
import json
import re
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

from audit import ROOT, audit
sys.path.insert(0, str(ROOT / "tools/win"))
from target import Target


def command(script):
    return "powershell -NoProfile -EncodedCommand " + base64.b64encode(script.encode("utf-16-le")).decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--soak", type=Path, required=True, help="Collected completed soak, with final readback")
    parser.add_argument("--run", required=True, help="Already prepared stuck run, e.g. stuck-01")
    parser.add_argument("--destination", type=Path, required=True, help="New workspace observation directory")
    args = parser.parse_args()
    if not re.fullmatch("stuck-[a-z0-9-]+", args.run):
        parser.error("Invalid stuck run name")
    destination = args.destination.resolve()
    if not destination.is_relative_to(ROOT.parent) or destination.exists():
        parser.error("Use a new directory inside the workspace; no observation retry")
    result = audit(args.soak, ROOT / "evidence/linux/2026-09-21-E14-vulkan-compute-reference")
    if result["status"] != "PASS":
        raise RuntimeError("Soak acceptance incomplete; injection refused")
    plug = subprocess.run([sys.executable, str(ROOT.parent / "scratch/smartplug/plug.py"), "status"],
                          capture_output=True, text=True, timeout=15)
    if plug.returncode or json.loads(plug.stdout).get("relay_on") is not True:
        raise RuntimeError("Recovery plug readback unavailable or OFF")
    target = Target()
    # Resolve the pinned endpoint before starting the independent deadline.
    target.address
    out = "C:\\BC250\\m11\\" + args.run
    task = "BC250-M11-" + args.run + "-Stuck"
    preflight = r"""
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
$out='OUT';$task='TASK'
if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP'}
if((Get-ScheduledTask $task).State -ne 'Ready'){throw 'Prepared task not ready'}
if(@(Get-ScheduledTask 'BC250-M11-*' | Where-Object State -in @('Running','Queued')).Count){throw 'Another M11 task is active'}
$cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
$clock=& $cli clock read | Out-String
if($LASTEXITCODE -ne 0 -or $clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000){throw 'Clock or thermal precondition'}

if((Get-Content "$out\positive\result.json" -Raw | ConvertFrom-Json).status -ne 'PASS'){throw 'Positive control missing'}
if((Test-Path "$out\stuck\started.json") -or (Test-Path "$out\armed.once")){throw 'Injection already armed or started'}
'READY'
""".replace("OUT", out).replace("TASK", task)
    p = target.ssh(command(preflight), timeout=10)
    if p.returncode or "READY" not in p.stdout:
        raise RuntimeError("Stuck preflight refused; no launch")
    destination.mkdir()
    (destination / "soak-audit.json").write_text(json.dumps(result, indent=2))
    (destination / "plug-readback.json").write_text(plug.stdout)
    def record(kind, **fields):
        row = {"utc": datetime.now(timezone.utc).isoformat(), "kind": kind, **fields}
        with (destination / "observation.jsonl").open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(row) + "\n")

    # Persist intent before the only launch attempt. A lost SSH acknowledgment is
    # ambiguous, never grounds to call Start-ScheduledTask again.
    record("armed", run=args.run, task=task, observation_seconds=30)
    start = time.monotonic()
    trigger = r"""
$ErrorActionPreference='Stop'
New-Item -ItemType File 'OUT\armed.once' | Out-Null
Start-ScheduledTask 'TASK'
'LAUNCH_REQUESTED'
""".replace("OUT", out).replace("TASK", task)
    try:
        p = target.ssh(command(trigger), timeout=5)
        record("launch_ack", returncode=p.returncode, acknowledged="LAUNCH_REQUESTED" in p.stdout)
    except subprocess.TimeoutExpired:
        record("launch_ack_unknown")
    probe = r"""
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
$out='OUT';$value=@{task=(Get-ScheduledTask 'TASK').State}
foreach($name in @('started','child','result')){
 if(Test-Path "$out\stuck\$name.json"){$value[$name]=Get-Content "$out\stuck\$name.json" -Raw | ConvertFrom-Json}
}
if($value.child){
 $p=Get-Process -Id $value.child.pid -ErrorAction SilentlyContinue
 if($p -and $p.Path -eq $value.child.exe){$value.process=@{pid=$p.Id;start=$p.StartTime.ToUniversalTime().ToString('o');cpu=$p.CPU}}
}
if(Test-Path "$out\stuck\native.err"){$value.stderr_tail=@(Get-Content "$out\stuck\native.err" -Tail 16)}
$value | ConvertTo-Json -Depth 6 -Compress
""".replace("OUT", out).replace("TASK", task)
    argv = target.ssh_argv(command(probe))
    while (remaining := 30 - (time.monotonic() - start)) > 0:
        try:
            p = subprocess.run(argv, capture_output=True, text=True, timeout=min(3, remaining))
            if p.returncode == 0:
                record("sample", elapsed=time.monotonic() - start, data=json.loads(p.stdout))
            else:
                record("ssh_failure", elapsed=time.monotonic() - start, returncode=p.returncode)
        except (subprocess.TimeoutExpired, ValueError) as error:
            record(type(error).__name__, elapsed=time.monotonic() - start)
        remaining = 30 - (time.monotonic() - start)
        if remaining > 0:
            time.sleep(min(0.5, remaining))
    record("deadline", elapsed=time.monotonic() - start,
           note="Observation ended; no assertion of GPU recovery. Inspect and recover before further GPU work.")
    print("One launch attempted; independent observation saved. Do not retry injection.")
    print("Inspect OS/event/queue state and recover the lab if necessary.")


if __name__ == "__main__":
    main()
