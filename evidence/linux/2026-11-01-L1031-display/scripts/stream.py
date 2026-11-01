"""Development PC: run a script on unit A's Linux session and stream its output into a local file as it arrives.

linux-xfer.py run returns the output only when the script ends, so a step that may hang the machine (or a trace
reader that must outlive one) runs through this instead: whatever was printed before a hang is on this PC.
Same ssh as linux-xfer.py (tools/win/target.py, the Linux session configuration), script sent as `sh -s`.

  python stream.py <local log> <bound s> <local script> [args...]
  python stream.py <local log> <bound s> --cmd "<remote command>"

Exit code: the remote exit code; 124 when the bound ran out (the local ssh is killed, which also ends the remote
`sh -s` once its output pipe closes).
"""
import os
import subprocess
import sys
import threading
import time

ROOT = os.environ.get("BC250_ROOT", r"P:\BC-250")
sys.path.insert(0, os.path.join(ROOT, "bc250-win", "tools", "win"))
import target  # noqa: E402


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    log, bound = argv[0], float(argv[1])
    if argv[2] == "--cmd":
        remote, script = argv[3], None
    else:
        script = open(argv[2], "rb").read().replace(b"\r\n", b"\n")
        remote = "sh -s " + " ".join(argv[3:])
    t = target.Target(config=target.LINUX_CONFIG)
    p = subprocess.Popen(t.ssh_argv(remote), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    out = open(log, "ab", buffering=0)
    out.write(("\n# stream.py %s bound=%ss remote=%s\n" % (time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                                                           bound, remote)).encode())

    def pump():
        fd = p.stdout.fileno()
        while True:
            chunk = os.read(fd, 4096)
            if not chunk:
                break
            out.write(chunk)
            sys.stdout.buffer.write(chunk)
            sys.stdout.flush()

    th = threading.Thread(target=pump, daemon=True)
    th.start()
    if script is not None:
        try:
            p.stdin.write(script)
        except OSError:
            pass
    p.stdin.close()
    try:
        rc = p.wait(timeout=bound)
    except subprocess.TimeoutExpired:
        p.kill()
        rc = 124
    th.join(timeout=5)
    out.write(("\n# stream.py end rc=%d %s\n" % (rc, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))).encode())
    out.close()
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
