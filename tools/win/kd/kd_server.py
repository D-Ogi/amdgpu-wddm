#!/usr/bin/env python3
"""Run `kd.exe` as a long-lived debugging server for the BC-250, so that short non-interactive commands can be
sent to it with `kd_cmd.py` instead of holding an interactive debugger open.

    python kd_server.py start                 live target over KDNET (UDP port 50000, key from secrets)
    python kd_server.py start --dump FILE     serve a dump file instead of a live target
    python kd_server.py status                is the server up, is the target connected
    python kd_server.py stop
    python kd_server.py restart [--dump FILE]
    python kd_server.py firewall              what the local firewall does with inbound UDP (read-only)

The server keeps running across target reboots: KDNET reconnects by itself, and kd waits for it. That is the
reason for a server at all - a debugger started per command would miss every bugcheck between commands.

The key lives in `P:/BC-250/secrets/kd/key.txt` and is never printed; `status` shows the command line with the
key replaced. Symbols, symbol cache and logs are all under `P:/BC-250/scratch/kd` and `.../scratch/symbols`.
"""

import datetime
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kdenv  # noqa: E402

CREATE_NO_WINDOW = 0x08000000
CREATE_NEW_PROCESS_GROUP = 0x00000200


def transport(dump):
    if dump:
        return ["-z", os.path.abspath(dump)], None
    secret = kdenv.key()
    return ["-k", f"net:port={kdenv.PORT},key={secret}"], secret


def start(dump=None):
    state = kdenv.read_state()
    if state and kdenv.alive(state["pid"]):
        sys.exit(f"a server is already running as pid {state['pid']} ({state['mode']}); stop it first")
    kdenv.ensure_dirs()
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    log = os.path.join(kdenv.SCRATCH, f"kd-{stamp}.log")
    connection, secret = transport(dump)
    # -server has to come first: kd rejects the command line otherwise.
    argv = [kdenv.KD, "-server", f"npipe:pipe={kdenv.PIPE}"] + connection + [
        "-y", kdenv.SYMBOL_PATH,
        "-logo", log]
    # Its own hidden console, its own process group: nothing to look at, and Ctrl+Break can still be delivered
    # to it (that is how `kd_cmd.py --break` breaks into a running target). Whatever kd writes before the log
    # file is open (a rejected command line, for instance) goes next to the log.
    console = open(log + ".out", "wb")
    child = subprocess.Popen(argv, env=kdenv.environment(), cwd=kdenv.SCRATCH,
                             stdout=console, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                             creationflags=CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                             close_fds=True)
    console.close()
    kdenv.write_state({"pid": child.pid, "pipe": kdenv.PIPE, "log": log,
                       "mode": f"dump {os.path.abspath(dump)}" if dump else f"live KDNET udp/{kdenv.PORT}",
                       "started": datetime.datetime.now().isoformat(timespec="seconds"),
                       "argv": kdenv.redact(argv, secret)})
    print(f"kd server pid {child.pid}  pipe npipe:pipe={kdenv.PIPE}  log {log}")
    print("mode " + (f"dump {os.path.abspath(dump)}" if dump else
                     f"live KDNET udp/{kdenv.PORT}, waiting for the target to connect"))


def stop():
    state = kdenv.read_state()
    if not state:
        sys.exit("no server recorded")
    if kdenv.alive(state["pid"]):
        subprocess.run(["taskkill", "/PID", str(state["pid"]), "/T", "/F"], capture_output=True)
        print(f"stopped pid {state['pid']}")
    else:
        print(f"pid {state['pid']} was already gone")
    os.remove(kdenv.STATE)


def log_tail(log, lines=12):
    try:
        with open(log, "r", encoding="utf-8", errors="replace") as f:
            return [l.rstrip() for l in f.read().splitlines() if l.strip()][-lines:]
    except OSError:
        return []


def status():
    state = kdenv.read_state()
    if not state:
        print("no server recorded (start it with: python kd_server.py start)")
        return 1
    running = kdenv.alive(state["pid"])
    print(f"server     pid {state['pid']}  {'running' if running else 'NOT running'}  since {state['started']}")
    print(f"mode       {state['mode']}")
    print(f"pipe       npipe:pipe={state['pipe']}")
    print(f"log        {state['log']}")
    print("command    " + " ".join(state["argv"]))
    if not running:
        return 1
    tail = log_tail(state["log"])
    if state["mode"].startswith("dump"):
        print("target     a dump file, always 'connected'")
    else:
        # kd prints these as the KDNET link comes and goes; the last one wins.
        marks = [l for l in tail if "Connected to target" in l or "Waiting to reconnect" in l
                 or "Kernel Debugger connection established" in l]
        last = marks[-1] if marks else ""
        print("target     " + ("waiting to reconnect (target rebooting, or debugging not enabled on it)"
                               if "Waiting to reconnect" in last else
                               "connected" if last else
                               "no connection line in the log yet: nothing has ever connected"))
    for line in tail:
        print("  | " + line)
    return 0


def firewall():
    """Read-only: whether inbound connections are blocked by default. Adding a rule needs elevation; this
    script never tries. If KDNET never connects and this says inbound is blocked, that is the first suspect."""
    done = subprocess.run(["powershell", "-NoProfile", "-Command",
                           "Get-NetFirewallProfile | Select-Object Name,Enabled,DefaultInboundAction | Format-Table -AutoSize"],
                          capture_output=True, text=True, timeout=60)
    sys.stdout.write(done.stdout or done.stderr)
    print(f"KDNET needs inbound UDP {kdenv.PORT} on this PC ({kdenv.KD}).")
    print("Opening it needs an elevated shell; this script does not change firewall rules.")


def main(argv):
    if not argv:
        sys.exit(__doc__)
    cmd, args = argv[0], argv[1:]
    dump = args[args.index("--dump") + 1] if "--dump" in args else None
    if cmd == "start":
        start(dump)
    elif cmd == "stop":
        stop()
    elif cmd == "restart":
        if kdenv.read_state():
            stop()
        start(dump)
    elif cmd == "status":
        sys.exit(status())
    elif cmd == "firewall":
        firewall()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
