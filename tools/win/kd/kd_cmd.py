#!/usr/bin/env python3
"""Send debugger commands to the kd server started by `kd_server.py` and print what it answers.

    python kd_cmd.py "lm m bc250*" "!analyze -v"      run commands, leave the target broken in
    python kd_cmd.py --break                          break into a running target
    python kd_cmd.py --go                             let it run again
    python kd_cmd.py --break "kb" "!irp" --go         break, look, resume
    python kd_cmd.py --dump FILE "!analyze -v"        one-shot on a dump file, no server needed

A remote client attaches to the server, runs the commands and leaves with `.remote_exit`, so the debugging
session and the server survive. A running target has no debugger prompt, so commands would wait for the next
bugcheck: pass `--break` first unless the target is already broken in.
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kdenv  # noqa: E402

CREATE_NO_WINDOW = 0x08000000


def send(commands, timeout=600):
    state = kdenv.read_state()
    if not state or not kdenv.alive(state["pid"]):
        sys.exit("no kd server is running (python kd_server.py start)")
    # -clines 1: do not drag the server's whole output history back with every command.
    return kdenv.run_kd(["-remote", f"npipe:pipe={state['pipe']},server=localhost", "-clines", "1",
                         "-cf", kdenv.script(commands, [".remote_exit"], "remote")], timeout=timeout)


def one_shot(dump, commands, timeout=900):
    """A dump does not need a server: open it, run the commands, quit."""
    kdenv.ensure_dirs()
    log = os.path.join(kdenv.SCRATCH, "kd-dump.log")
    return kdenv.run_kd(["-z", os.path.abspath(dump), "-y", kdenv.SYMBOL_PATH, "-logo", log,
                         "-cf", kdenv.script(commands, ["q"], "dump")], timeout=timeout)


def break_in():
    """Ctrl+Break to the server, from a child of our own: the call detaches the caller's console."""
    state = kdenv.read_state()
    if not state or not kdenv.alive(state["pid"]):
        sys.exit("no kd server is running (python kd_server.py start)")
    done = subprocess.run([sys.executable, os.path.abspath(__file__), "--ctrl-break-now", str(state["pid"])],
                          capture_output=True, text=True, timeout=60, creationflags=CREATE_NO_WINDOW)
    sys.stderr.write(done.stderr)
    return done.returncode


def main(argv):
    if argv[:1] == ["--ctrl-break-now"]:  # internal: runs in its own console, see kdenv.ctrl_break
        ok, why = kdenv.ctrl_break(int(argv[1]))
        return 0 if ok else sys.exit(why)
    if not argv:
        sys.exit(__doc__)

    dump = None
    if "--dump" in argv:
        at = argv.index("--dump")
        dump, argv = argv[at + 1], argv[:at] + argv[at + 2:]
    commands = [a for a in argv if a not in ("--break", "--go")]

    if "--break" in argv:
        if dump:
            sys.exit("--break makes no sense for a dump file")
        if break_in():
            return 1
    if "--go" in argv:
        commands.append("g")
    if not commands:
        if "--break" in argv:
            print("break requested")
            return 0
        sys.exit("nothing to do: give at least one debugger command")

    done = one_shot(dump, commands) if dump else send(commands)
    sys.stdout.write(kdenv.clean(done.stdout) + "\n")
    if done.stderr.strip():
        sys.stderr.write(done.stderr)
    return done.returncode


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except subprocess.TimeoutExpired:
        sys.exit("the debugger did not answer in time: the target is probably running (try --break) or gone")
