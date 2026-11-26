"""Host helper for the Linux session of unit A: the ssh of tools/win/target.py with the Linux session config.

    python -I lin.py run "<sh command>" [timeout_s]
    python -I lin.py push <remote_dir> <local files...>
    python -I lin.py stream "<sh command>" <local log> [timeout_s]

stream writes the remote output to the log as it arrives (flushed per chunk), so a hang of the unit leaves the
last line in the log. No address and no key is printed.
"""
import io
import os
import subprocess
import sys
import tarfile
import time

sys.path.insert(0, r"P:\BC-250\bc250-win\tools\win")
import target  # noqa: E402


def main(argv):
    t = target.Target(target.LINUX_CONFIG)
    cmd = argv[0]
    if cmd == "run":
        done = t.ssh(argv[1], timeout=int(argv[2]) if len(argv) > 2 else 120)
        sys.stdout.write(done.stdout)
        sys.stderr.write(done.stderr)
        return done.returncode
    if cmd == "push":
        remote_dir, files = argv[1], argv[2:]
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w") as tar:
            for p in files:
                tar.add(p, arcname=os.path.basename(p))
        done = t.ssh(f"mkdir -p {remote_dir} && tar -xf - -C {remote_dir} && ls -l {remote_dir}",
                     timeout=120, binary=True, stdin=buf.getvalue())
        sys.stdout.write(done.stdout.decode("utf-8", "replace"))
        sys.stderr.write(done.stderr.decode("utf-8", "replace"))
        return done.returncode
    if cmd == "stream":
        command, log = argv[1], argv[2]
        bound = int(argv[3]) if len(argv) > 3 else 170
        start = time.monotonic()
        with open(log, "ab", buffering=0) as out:
            p = subprocess.Popen(t.ssh_argv(command), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            os.set_blocking(p.stdout.fileno(), True)
            while True:
                chunk = p.stdout.read1(4096) if hasattr(p.stdout, "read1") else p.stdout.read(1)
                if chunk:
                    out.write(chunk)
                elif p.poll() is not None:
                    break
                if time.monotonic() - start > bound:
                    p.kill()
                    out.write(f"\n# HOST: stream stopped at the bound of {bound} s\n".encode())
                    return 124
        return p.wait()
    sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
