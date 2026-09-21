#!/usr/bin/env python3
"""One place that knows how to reach the BC-250 lab target from the development PC.

Unit A answers on two addresses: the Ethernet port and a USB Wi-Fi dongle. Both are the same machine, so one
pinned host key serves both (`HostKeyAlias`). This module picks the wired address when it answers on port 22
within a couple of seconds and falls back to Wi-Fi otherwise, then hands out the ssh argv the tools need, plus
three helpers: run a PowerShell script file on the target, push files, pull a file.

The addresses and key paths are not in this repository. They come from a JSON file outside it, by default
`P:/BC-250/secrets/client/target.json` (`BC250_TARGET_CONFIG` overrides the path); `tools/win/README.md`
documents its shape and the lab's values. Importing this module never touches that file - only connecting does.

    python target.py addr                      the address that answers right now
    python target.py info                      address, configuration in use, remote hostname
    python target.py wait [seconds]            block until it answers again after a reboot
    python target.py run '<powershell>'         one inline command (watch the quoting, prefer a script file)
    python target.py ps <file.ps1> [args...]   copy a script over and run it with -File
    python target.py push <local...> [--to C:\\BC250\\dir]
    python target.py pull <remote> <local>

Inline PowerShell through Git Bash gets mangled ($_, quotes): put anything with punctuation in a .ps1 file
and use `ps`.
"""

import io
import json
import os
import socket
import subprocess
import sys
import tarfile
import time

ROOT = os.environ.get("BC250_ROOT", "P:/BC-250")
CONFIG = os.environ.get("BC250_TARGET_CONFIG", os.path.join(ROOT, "secrets", "client", "target.json"))

# Only used when the configuration file leaves a field out. Addresses have no default on purpose: they are the
# one thing that must come from the file, so that the repository carries no lab address.
DEFAULTS = {
    "user": "bc250",
    "port": 22,
    "probe_timeout": 3.0,
    "connect_timeout": 15,
    "connection_attempts": 3,
    "work_dir": "C:\\BC250",
    "tmp_dir": "C:\\BC250\\tmp",
}


class TargetError(RuntimeError):
    pass


def load(path=None):
    """The lab configuration. Relative paths inside it resolve against the file's own directory."""
    path = path or CONFIG
    try:
        with open(path, "r", encoding="utf-8") as f:
            cfg = dict(DEFAULTS, **json.load(f))
    except FileNotFoundError:
        raise TargetError(f"no target configuration at {path}; see tools/win/README.md for its shape")
    except ValueError as e:
        raise TargetError(f"{path} is not valid JSON: {e}")
    if not cfg.get("addresses"):
        raise TargetError(f"{path} lists no addresses")
    here = os.path.dirname(os.path.abspath(path))
    for field in ("identity", "known_hosts"):
        if cfg.get(field) and not os.path.isabs(cfg[field]):
            cfg[field] = os.path.join(here, cfg[field])
    cfg.setdefault("host_key_alias", cfg["addresses"][0])
    return cfg


def answers(address, port, timeout):
    """True if something accepts TCP there. Cheaper and more honest than waiting for an ssh timeout."""
    try:
        with socket.create_connection((address, int(port)), timeout=float(timeout)):
            return True
    except OSError:
        return False


class Target:
    def __init__(self, config=None):
        self.cfg = config if isinstance(config, dict) else load(config)
        self._address = None
        self.answered = None  # set by `address`: True probed, False nothing answered, None not probed

    @property
    def address(self):
        """The first configured address that answers, wired one first; remembered for this process.

        `BC250_TARGET_ADDR` forces an address and skips the probe, for the case where the probe would be wrong
        (a port forward, a target that is up but slow to answer during boot)."""
        if self._address:
            return self._address
        forced = os.environ.get("BC250_TARGET_ADDR")
        if forced:
            self._address, self.answered = forced, None
            return forced
        for address in self.cfg["addresses"]:
            if answers(address, self.cfg["port"], self.cfg["probe_timeout"]):
                self._address, self.answered = address, True
                return address
        # Nothing answered. The target reboots often; let ssh itself report the failure against the first
        # address rather than deciding here that the machine is gone.
        self._address, self.answered = self.cfg["addresses"][0], False
        return self._address

    def wait(self, seconds=300, every=10):
        """Block until an address answers on port 22, for use around the reboots this target does. True if it
        came back, False on timeout. Forgets a previously chosen address, so the wired one is preferred again."""
        deadline = time.monotonic() + seconds
        while True:
            self._address = None
            if self.address and self.answered:
                return True
            if time.monotonic() >= deadline:
                return False
            time.sleep(every)

    def ssh_argv(self, remote_command=None, options=()):
        cfg = self.cfg
        argv = ["ssh",
                "-o", "BatchMode=yes",
                "-o", "IdentitiesOnly=yes",
                "-o", f"ConnectTimeout={cfg['connect_timeout']}",
                "-o", f"ConnectionAttempts={cfg['connection_attempts']}",
                "-o", f"HostKeyAlias={cfg['host_key_alias']}"]
        if cfg.get("identity"):
            argv += ["-i", cfg["identity"]]
        if cfg.get("known_hosts"):
            argv += ["-o", f"UserKnownHostsFile={cfg['known_hosts']}"]
        if int(cfg["port"]) != 22:
            argv += ["-p", str(cfg["port"])]
        argv += list(options)
        argv.append(f"{cfg['user']}@{self.address}")
        if remote_command is not None:
            argv.append(remote_command)
        return argv

    def ssh(self, remote_command, timeout=120, binary=False, stdin=None, options=()):
        return subprocess.run(self.ssh_argv(remote_command, options),
                              input=stdin, capture_output=True, text=not binary, timeout=timeout)

    def run(self, powershell, timeout=120):
        """One PowerShell command, encoded so that no shell on the way can reinterpret it. Returns stdout."""
        import base64
        encoded = base64.b64encode(powershell.encode("utf-16-le")).decode()
        return self.ssh("powershell -NoProfile -EncodedCommand " + encoded, timeout=timeout).stdout

    def run_script(self, local_path, args=(), timeout=600, remote_dir=None):
        """Copy a .ps1 over and run it with -File. Returns the CompletedProcess, so the caller sees stderr
        and the exit code; inline PowerShell through ssh mangles quotes, a file does not."""
        remote_dir = remote_dir or self.cfg["tmp_dir"]
        self.push([local_path], remote_dir)
        remote = remote_dir.rstrip("\\") + "\\" + os.path.basename(local_path)
        quoted = " ".join(f'"{a}"' if " " in str(a) else str(a) for a in args)
        return self.ssh(f'powershell -NoProfile -ExecutionPolicy Bypass -File "{remote}" {quoted}'.rstrip(),
                        timeout=timeout)

    def push(self, local_paths, remote_dir=None, timeout=300):
        """Copy files to the target. There is no scp on it, so a tar built here is piped into its tar."""
        remote_dir = remote_dir or self.cfg["tmp_dir"]
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w") as tar:
            for path in local_paths:
                tar.add(path, arcname=os.path.basename(path))
        self.run(f'New-Item -ItemType Directory -Force -Path "{remote_dir}" | Out-Null')
        done = self.ssh(f'cmd /c "tar -xf - -C {remote_dir}"', timeout=timeout, binary=True, stdin=buf.getvalue())
        if done.returncode:
            raise TargetError(f"push failed: {done.stderr.decode('utf-8', 'replace').strip()}")
        return [remote_dir.rstrip("\\") + "\\" + os.path.basename(p) for p in local_paths]

    def pull(self, remote_path, local_path, timeout=300):
        """Copy one file back, the same way round."""
        directory, name = remote_path.rsplit("\\", 1)
        done = self.ssh(f'cmd /c "tar -cf - -C {directory} {name}"', timeout=timeout, binary=True)
        if done.returncode or not done.stdout:
            raise TargetError(f"pull failed: {done.stderr.decode('utf-8', 'replace').strip()}")
        with tarfile.open(fileobj=io.BytesIO(done.stdout), mode="r") as tar:
            member = tar.getmember(name)
            source = tar.extractfile(member)
            os.makedirs(os.path.dirname(os.path.abspath(local_path)), exist_ok=True)
            with open(local_path, "wb") as f:
                f.write(source.read())
        return os.path.abspath(local_path)


def main(argv):
    if not argv:
        sys.exit(__doc__)
    cmd, args = argv[0], argv[1:]
    try:
        target = Target()
        if cmd == "addr":
            address = target.address
            print(address if target.answered is not False else f"{address} (nothing answered on port 22, target down?)")
            sys.exit(0 if target.answered is not False else 1)
        elif cmd == "info":
            address = target.address
            print(f"config     {CONFIG}")
            print("addresses  " + ", ".join(target.cfg["addresses"]) + f"  (host key pinned as {target.cfg['host_key_alias']})")
            print(f"using      {target.cfg['user']}@{address}" +
                  ("" if target.answered else "  [not answering on port 22]" if target.answered is False else "  [forced]"))
            print("remote     " + (target.run("hostname; (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')")
                                   .strip().replace("\n", "  boot ").replace("\r", "") or "no answer"))
        elif cmd == "wait":
            seconds = int(args[0]) if args else 300
            back = target.wait(seconds)
            print(f"{target.address} answers" if back else f"still nothing after {seconds}s")
            sys.exit(0 if back else 1)
        elif cmd == "run":
            done = target.ssh(args[0] if args else sys.exit("run needs a command"))
            sys.stdout.write(done.stdout)
            sys.stderr.write(done.stderr)
            sys.exit(done.returncode)
        elif cmd == "ps":
            if not args:
                sys.exit("ps needs a .ps1 file")
            done = target.run_script(args[0], args[1:])
            sys.stdout.write(done.stdout)
            sys.stderr.write(done.stderr)
            sys.exit(done.returncode)
        elif cmd == "push":
            to = target.cfg["tmp_dir"]
            if "--to" in args:
                at = args.index("--to")
                to, args = args[at + 1], args[:at]
            for remote in target.push(args, to):
                print(remote)
        elif cmd == "pull":
            print(target.pull(args[0], args[1]))
        else:
            sys.exit(__doc__)
    except TargetError as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main(sys.argv[1:])
