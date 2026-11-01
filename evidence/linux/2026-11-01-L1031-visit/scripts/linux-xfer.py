"""Copy files to and from unit A's Linux session over target.py's ssh (tar through stdin/stdout).
  linux-xfer.py put <remote_dir> <local files...>
  linux-xfer.py get <remote_dir> <local_dir>
  linux-xfer.py run <local script> [args]      (sh -s, the script sent as bytes)"""
import io
import os
import sys
import tarfile

ROOT = os.environ.get("BC250_ROOT", r"P:\BC-250")
sys.path.insert(0, os.path.join(ROOT, "bc250-win", "tools", "win"))
import target  # noqa: E402

t = target.Target(config=target.LINUX_CONFIG)
cmd = sys.argv[1]
if cmd == "put":
    remote, files = sys.argv[2], sys.argv[3:]
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tf:
        for f in files:
            data = open(f, "rb").read().replace(b"\r\n", b"\n")
            info = tarfile.TarInfo(os.path.basename(f))
            info.size, info.mode = len(data), 0o755
            tf.addfile(info, io.BytesIO(data))
    done = t.ssh(f"mkdir -p {remote} && tar -xf - -C {remote} && ls -la {remote}", binary=True, stdin=buf.getvalue())
elif cmd == "get":
    remote, local = sys.argv[2].rstrip("/"), sys.argv[3]
    parent, name = os.path.dirname(remote) or "/", os.path.basename(remote)
    done = t.ssh(f"tar -cf - -C {parent} {name}", binary=True, timeout=600)
    if done.returncode == 0:
        os.makedirs(local, exist_ok=True)
        with tarfile.open(fileobj=io.BytesIO(done.stdout), mode="r") as tf:
            tf.extractall(local, filter="data")
        print(f"extracted {len(done.stdout)} bytes into {local}")
        sys.exit(0)
elif cmd == "run":
    script = open(sys.argv[2], "rb").read().replace(b"\r\n", b"\n")
    done = t.ssh("sh -s " + " ".join(sys.argv[3:]), binary=True, stdin=script, timeout=900)
else:
    sys.exit(__doc__)
sys.stdout.write((done.stdout or b"").decode(errors="replace"))
sys.stderr.write((done.stderr or b"").decode(errors="replace"))
sys.exit(done.returncode)
