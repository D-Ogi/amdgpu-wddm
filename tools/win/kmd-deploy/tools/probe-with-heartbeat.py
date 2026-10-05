"""LAB, read-only diagnostic: start the 60 s present heartbeat exactly as postflight.py does, then run
ops/task-probe-run.ps1 (postflight.ps1 line 52 sampled inside the bounded child), then stop the heartbeat.

    python tools/probe-with-heartbeat.py kmdRRR-deployNNN
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from kmdcommon import OPS, REMOTE_TMP, attempt_name, heartbeat, heartbeat_stop, target  # noqa: E402

name = attempt_name(sys.argv[1])
t = target()
t.push([OPS / 'task-probe.ps1'], REMOTE_TMP)
print(heartbeat(t, 60))
time.sleep(3)
try:
    r = t.run_script(OPS / 'task-probe-run.ps1', args=[name], timeout=90)
    print(r.stdout[-6000:], r.stderr[-1500:])
finally:
    print('heartbeat', heartbeat_stop(t))
