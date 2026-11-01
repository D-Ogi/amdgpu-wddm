"""Stream a long-running command from unit A's Linux to a local file, each line stamped with the development PC's
UTC clock (so a hang shows where the stream stopped). Usage: stream-linux.py "<remote command>" <local file> [max_s]
Run it in the background before an R2 step; it ends at max_s (default 900) or when the connection drops."""
import datetime
import os
import subprocess
import sys
import time

ROOT = os.environ.get("BC250_ROOT", r"P:\BC-250")
sys.path.insert(0, os.path.join(ROOT, "bc250-win", "tools", "win"))
import target  # noqa: E402

cmd, path = sys.argv[1], sys.argv[2]
max_s = float(sys.argv[3]) if len(sys.argv) > 3 else 900.0
t = target.Target(config=target.LINUX_CONFIG)
argv = t.ssh_argv(cmd, options=("-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=3"))
start = time.time()
with open(path, "a", encoding="utf-8", errors="replace") as out:
    out.write(f"# stream start {datetime.datetime.now(datetime.timezone.utc):%Y-%m-%dT%H:%M:%S.%fZ} cmd={cmd}\n")
    p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        for raw in p.stdout:
            now = datetime.datetime.now(datetime.timezone.utc)
            out.write(f"{now:%H:%M:%S.%f} {raw.decode(errors='replace').rstrip()}\n")
            out.flush()
            if time.time() - start > max_s:
                break
    finally:
        p.kill()
        out.write(f"# stream end {datetime.datetime.now(datetime.timezone.utc):%Y-%m-%dT%H:%M:%S.%fZ} rc={p.poll()}\n")
