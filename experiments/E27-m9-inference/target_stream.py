#!/usr/bin/env python3
"""Stream an E27 remote script through Target, retaining output if the lab hangs.
Usage: python target_stream.py SCRIPT.ps1 LOCAL_LOG
This observes the launched job; a lost SSH connection does not cancel the remote script.
"""
import pathlib
import subprocess
import sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools" / "win"))
from target import Target

script, log = map(pathlib.Path, sys.argv[1:3])
target = Target()
remote = target.push([str(script)])[0]
command = f'powershell -NoProfile -ExecutionPolicy Bypass -File "{remote}"'
# Exclusive creation prevents an earlier observation from being overwritten.
with log.open("x", encoding="utf-8") as witness:
    process = subprocess.Popen(target.ssh_argv(command), stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, errors="replace")
    print(f"observer_pid={process.pid}", flush=True)
    for line in process.stdout:
        witness.write(line)
        witness.flush()
        print(line, end="", flush=True)
    code = process.wait()
    witness.write(f"\nssh_exit={code}\n")
sys.exit(code)
