#!/usr/bin/env python3
"""One place that knows how to reach the BC-250 lab target from the development PC.

Unit A answers on two addresses: the Ethernet port and a USB Wi-Fi dongle. Both are the same machine, so one
pinned host key serves both (`HostKeyAlias`). This module picks the wired address when it answers on port 22
within a couple of seconds and falls back to Wi-Fi otherwise, then hands out the ssh argv the tools need, plus
three helpers: run a PowerShell script file on the target, push files, pull a file.

The addresses and key paths are not in this repository. They come from a JSON file outside it, by default
`<BC250_ROOT>/secrets/client/target.json` (`BC250_TARGET_CONFIG` overrides the path); `tools/win/README.md`
documents its shape and the lab's values. Importing this module never touches that file - only connecting does.

    python target.py addr                      the address that answers right now (or answered recently)
    python target.py info                      address, configuration in use, remote hostname
    python target.py wait [seconds]            block until it answers again after a reboot
    python target.py boot [--os windows|linux|any] [--bound S] [--first S] [--every S] [--after-now|--after UTC]
                                               wait for a boot of either OS: a line each round, one verdict
                                               line at the end (BOOT UP / BOOT WRONG-OS / BOOT TIMEOUT)
    python target.py forget                    drop the cached address (the next call probes again)
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
from pathlib import Path

# BC250_ROOT is the workspace root; by default the parent directory of this repository
# (this file is tools/win/target.py, so the repository root is two levels up from here).
ROOT = os.environ.get("BC250_ROOT", str(Path(__file__).resolve().parents[2].parent))
CONFIG = os.environ.get("BC250_TARGET_CONFIG", os.path.join(ROOT, "secrets", "client", "target.json"))
# The same unit booted from the diagnostic USB stick: its own user, key and pinned host key.
LINUX_CONFIG = os.environ.get("BC250_LINUX_TARGET_CONFIG",
                              os.path.join(ROOT, "secrets", "linux-session", "target.json"))

# Only used when the configuration file leaves a field out. Addresses have no default on purpose: they are the
# one thing that must come from the file, so that the repository carries no lab address.
DEFAULTS = {
    "user": "bc250",
    "port": 22,
    "probe_timeout": 3.0,
    "address_cache_seconds": 300,
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
    """True if something accepts TCP there. Cheaper and more honest than waiting for an ssh timeout.

    Not free, though: OpenSSH 9.8+ (PerSourcePenalties, on by default) counts a connection closed before
    authentication against the source address, 1 s each, and refuses every connection from that address once
    the sum passes 15 s. Twenty tool invocations a minute, each probing first, locked the operator PC out of
    the lab for five minutes twice on 2026-09-30 (OpenSSH/Operational: "drop connection ... penalty:
    connections without attempting authentication"). Hence the address cache below: probe rarely."""
    try:
        with socket.create_connection((address, int(port)), timeout=float(timeout)) as s:
            # An sshd sends its identification line at once. A stale DHCP lease can hand a listed address to
            # another device that accepts port 22 and closes at key exchange (2026-09-30, after an AC cycle);
            # accepting alone is not an answer.
            s.settimeout(float(timeout))
            return s.recv(64).startswith(b"SSH-")
    except OSError:
        return False


def cache_path(config_path=None):
    """The last answering address, next to the configuration (outside the repository, like the addresses)."""
    return os.path.splitext(config_path or CONFIG)[0] + "-last-address.json"


class Target:
    def __init__(self, config=None):
        self.cfg = config if isinstance(config, dict) else load(config)
        # Each configuration keeps its own address cache: a Linux session must not overwrite the Windows one.
        self._cache_file = cache_path(None if isinstance(config, dict) else config)
        self._address = None
        self.answered = None  # set by `address`: True probed, False nothing answered, None not probed
        self.cached_age = None  # seconds, when `address` came from the cache instead of a probe

    def _cached(self):
        """The address a recent probe found, if the cache file is younger than address_cache_seconds."""
        try:
            with open(self._cache_file, "r", encoding="utf-8") as f:
                entry = json.load(f)
            age = time.time() - float(entry["time"])
            if entry["address"] in self.cfg["addresses"] and 0 <= age < float(self.cfg["address_cache_seconds"]):
                return entry["address"], age
        except (OSError, ValueError, KeyError, TypeError):
            pass
        return None, None

    def _remember(self, address):
        try:
            tmp = self._cache_file + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump({"address": address, "time": time.time()}, f)
            os.replace(tmp, self._cache_file)
        except OSError:
            pass

    def forget(self):
        """Drop the cached address: the next `address` probes again."""
        try:
            os.remove(self._cache_file)
        except OSError:
            pass

    def _probe(self):
        """The first configured address that answers on port 22, wired one first; None if nothing does."""
        for address in self.cfg["addresses"]:
            if answers(address, self.cfg["port"], self.cfg["probe_timeout"]):
                self._remember(address)
                return address
        return None

    @property
    def address(self):
        """The address to use: forced, cached from a recent probe, or probed now; remembered for this process.

        `BC250_TARGET_ADDR` forces an address and skips the probe, for the case where the probe would be wrong
        (a port forward, a target that is up but slow to answer during boot). A probe result is reused for
        address_cache_seconds (see `answers`); an ssh connection failure on a cached address forgets it."""
        if self._address:
            return self._address
        forced = os.environ.get("BC250_TARGET_ADDR")
        if forced:
            self._address, self.answered = forced, None
            return forced
        cached, age = self._cached()
        if cached:
            self._address, self.answered, self.cached_age = cached, None, age
            return cached
        found = self._probe()
        if found:
            self._address, self.answered = found, True
            return found
        # Nothing answered. The target reboots often; let ssh itself report the failure against the first
        # address rather than deciding here that the machine is gone.
        self._address, self.answered = self.cfg["addresses"][0], False
        return self._address

    def wait(self, seconds=300, every=10):
        """Block until an address answers on port 22, for use around the reboots this target does. True if it
        came back, False on timeout. Ignores the cache and forgets a previously chosen address, so the wired
        one is preferred again."""
        deadline = time.monotonic() + seconds
        while True:
            found = self._probe()
            if found:
                self._address, self.answered, self.cached_age = found, True, None
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
        # Text output is decoded leniently: a game title with a curly apostrophe (byte 0x83 in the console
        # code page) once killed the reader thread and left stdout None (2026-09-30).
        text_options = {} if binary else {"encoding": "utf-8", "errors": "replace"}
        done = subprocess.run(self.ssh_argv(remote_command, options),
                              input=stdin, capture_output=True, text=not binary, timeout=timeout, **text_options)
        if done.returncode == 255 and self.cached_age is not None:
            # ssh's own failure (not the remote command's) on an address the cache vouched for: the next
            # invocation probes again. No automatic retry here: the command may not be repeatable.
            self.forget()
        return done

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


# --- Waiting for a boot -------------------------------------------------------------------------------------
#
# 2026-10-10: an agent waited for a Linux boot through a loop that grepped another background command's output.
# That command never printed, Linux had been up for eleven minutes, and nothing noticed. `boot` replaces such
# loops: it asks both operating systems itself, prints a line every round, and always ends with one verdict line.
# Run it in the background and act on its exit; never wait on another process's text instead.

WINDOWS_BOOT = "(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('s')"
LINUX_BOOT = "cut -d' ' -f1 /proc/uptime; date -u +%s"


def windows_boot_epoch(text):
    """Boot time in epoch seconds from WINDOWS_BOOT's output ('2026-10-10T22:40:05'), or None."""
    import calendar
    for line in text.splitlines():
        try:
            return calendar.timegm(time.strptime(line.strip(), "%Y-%m-%dT%H:%M:%S"))
        except ValueError:
            continue
    return None


def linux_boot_epoch(text):
    """Boot time in epoch seconds from LINUX_BOOT's output (uptime seconds, then the epoch now), or None."""
    try:
        uptime, now = text.split()[:2]
        return int(now) - int(float(uptime))
    except ValueError:
        return None


def boot_probe(name, config_path, connect_timeout=8, timeout=40):
    """A probe for `boot_wait`: one authenticating command on each configured address, wired one first.
    No bare port-22 probe (the lab sshd penalises connections closed before authentication). Returns a
    function giving (boot epoch or None, short reason)."""
    def probe():
        try:
            target = Target(config_path)
        except TargetError as e:
            return None, f"no configuration ({e})"
        if name == "windows":
            import base64
            command = "powershell -NoProfile -EncodedCommand " + \
                base64.b64encode(WINDOWS_BOOT.encode("utf-16-le")).decode()
            parse = windows_boot_epoch
        else:
            command, parse = LINUX_BOOT, linux_boot_epoch
        # ssh keeps the first value given for an option, so shorten the configuration, not the argv.
        target.cfg["connect_timeout"], target.cfg["connection_attempts"] = connect_timeout, 1
        reasons = []
        for address in target.cfg["addresses"]:
            target._address = address
            try:
                done = target.ssh(command, timeout=timeout)
            except subprocess.TimeoutExpired:
                reasons.append("timeout")
                continue
            epoch = parse(done.stdout) if done.returncode == 0 else None
            if epoch is not None:
                return epoch, "answers"
            reasons.append(ssh_failure(done))
        return None, "; ".join(reasons) or "no address"
    return probe


def ssh_failure(done):
    """A short reason without the address: these lines end up in logs, and lab addresses stay out of them."""
    text = (done.stderr or "").lower()
    for needle, reason in (("host key verification failed", "other host key"), ("timed out", "no answer"),
                           ("refused", "refused"), ("permission denied", "auth refused"),
                           ("banner exchange", "no ssh banner")):
        if needle in text:
            return reason
    return f"rc {done.returncode}"


def utc(epoch):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(epoch))


def boot_wait(probes, want, bound, first=0, every=30, after=None, wrong_rounds=2,
              clock=time.monotonic, sleep=time.sleep, out=print):
    """Ask every probe once a round until `want` ("windows", "linux" or "any") is up with a boot later than
    `after` (epoch seconds; None accepts any boot). Prints one line per round and one verdict line at the end.

    Returns (verdict, name): ("up", name), ("wrong-os", name) when only an OS other than `want` is up with
    a new boot for `wrong_rounds` rounds in a row, or ("timeout", None) after `bound` seconds. A probe that
    raises counts as down: the wait itself must never hang or die on one bad round."""
    start = clock()
    if first:
        sleep(first)
    wrong = 0
    while True:
        elapsed = clock() - start
        states, up = [], []
        for name, probe in probes:
            try:
                epoch, reason = probe()
            except Exception as e:  # noqa: BLE001 - any probe failure is "down", reported, never fatal
                epoch, reason = None, f"probe error {type(e).__name__}"
            if epoch is None:
                states.append(f"{name} down ({reason})")
            elif after is not None and epoch <= after:
                states.append(f"{name} up, old boot {utc(epoch)}")
            else:
                states.append(f"{name} up, boot {utc(epoch)}")
                up.append((name, epoch))
        out(f"boot-wait {elapsed:.0f}s: " + ", ".join(states), flush=True)
        for name, epoch in up:
            if want in ("any", name):
                out(f"BOOT UP {name} boot {utc(epoch)} after {elapsed:.0f}s", flush=True)
                return "up", name
        wrong = wrong + 1 if up else 0
        if up and wrong >= wrong_rounds:
            out(f"BOOT WRONG-OS {up[0][0]} is up (boot {utc(up[0][1])}), wanted {want}, after {elapsed:.0f}s",
                flush=True)
            return "wrong-os", up[0][0]
        if clock() - start + every > bound:
            out(f"BOOT TIMEOUT after {clock() - start:.0f}s, wanted {want}: " + ", ".join(states), flush=True)
            return "timeout", None
        sleep(every)


def boot_main(args):
    """`target.py boot` - exit 0 BOOT UP, 1 BOOT TIMEOUT, 3 BOOT WRONG-OS, 2 usage."""
    import argparse
    parser = argparse.ArgumentParser(prog="target.py boot")
    parser.add_argument("--os", choices=("windows", "linux", "any"), default="any")
    parser.add_argument("--bound", type=int, default=900, help="seconds before BOOT TIMEOUT")
    parser.add_argument("--first", type=int, default=0, help="seconds to wait before the first round")
    parser.add_argument("--every", type=int, default=30, help="seconds between rounds (at least 20)")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--after-now", action="store_true", help="accept only a boot that happens from now on")
    group.add_argument("--after", help="accept only a boot later than this UTC time, 2026-10-10T22:40:00")
    opts = parser.parse_args(args)
    if opts.every < 20:
        parser.error("--every under 20 s: the lab sshd penalises frequent connections")
    after = time.time() if opts.after_now else None
    if opts.after:
        after = windows_boot_epoch(opts.after.rstrip("Z"))
        if after is None:
            parser.error("--after needs YYYY-MM-DDTHH:MM:SS")
    probes = [("windows", boot_probe("windows", CONFIG))]
    if os.path.exists(LINUX_CONFIG):
        probes.append(("linux", boot_probe("linux", LINUX_CONFIG)))
    elif opts.os == "linux":
        parser.error(f"no Linux session configuration at {LINUX_CONFIG}")
    verdict, _ = boot_wait(probes, opts.os, opts.bound, opts.first, opts.every, after)
    sys.exit({"up": 0, "timeout": 1, "wrong-os": 3}[verdict])


def main(argv):
    # Remote text may carry characters the console code page cannot show; never die on them.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="replace")
    if not argv:
        sys.exit(__doc__)
    cmd, args = argv[0], argv[1:]
    if cmd == "boot":
        boot_main(args)
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
                  ("" if target.answered else "  [not answering on port 22]" if target.answered is False else
                   f"  [cached {target.cached_age:.0f} s ago]" if target.cached_age is not None else "  [forced]"))
            print("remote     " + (target.run("hostname; (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')")
                                   .strip().replace("\n", "  boot ").replace("\r", "") or "no answer"))
        elif cmd == "wait":
            seconds = int(args[0]) if args else 300
            back = target.wait(seconds)
            print(f"{target.address} answers" if back else f"still nothing after {seconds}s")
            sys.exit(0 if back else 1)
        elif cmd == "forget":
            target.forget()
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
