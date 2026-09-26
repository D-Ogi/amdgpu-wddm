#!/usr/bin/env python3
"""What `kd_server.py` and `kd_cmd.py` both need: where the debugger is, where its symbols and logs go, how
the KDNET key is read, and how a process is checked or asked to break.

Nothing here writes to drive C:. The symbol store, the symbol cache (`DBGHELP_HOMEDIR`) and the logs are all
under `<BC250_ROOT>/scratch`, and both scripts set `_NT_SYMBOL_PATH` explicitly instead of inheriting whatever
default points at `C:\\ProgramData`.
"""

import ctypes
import json
import os
import re
import subprocess
import sys
from pathlib import Path

# BC250_ROOT is the workspace root; by default the parent directory of this repository
# (this file is tools/win/kd/kdenv.py, so the repository root is three levels up from here).
ROOT = os.environ.get("BC250_ROOT", str(Path(__file__).resolve().parents[3].parent))


def path(*parts):
    return os.path.normpath(os.path.join(ROOT, *parts))


KD = path("toolchain", "windbg", "x64", "amd64", "kd.exe")
SCRATCH = path("scratch", "kd")
SYMBOLS = path("scratch", "symbols")
STATE = os.path.join(SCRATCH, "server.json")
KEY_FILE = path("secrets", "kd", "key.txt")
PIPE = os.environ.get("BC250_KD_PIPE", "bc250kd")
PORT = int(os.environ.get("BC250_KD_PORT", "50000"))
# Our own build output first, so a driver we just built is matched by the PDB next to it rather than by
# whatever the symbol server happens to hold.
SYMBOL_PATH = ";".join([path("scratch", "build", "bc250kmd"),
                        path("scratch", "build", "bc250rd"),
                        f"srv*{SYMBOLS}*https://msdl.microsoft.com/download/symbols"])
KEY_SHAPE = re.compile(r"[0-9a-z]+(\.[0-9a-z]+){3}\Z")


def environment():
    """The environment both the server and a one-shot run are started with."""
    env = dict(os.environ)
    env["_NT_SYMBOL_PATH"] = SYMBOL_PATH
    env["DBGHELP_HOMEDIR"] = SCRATCH
    env["_NT_SYMCACHE_PATH"] = os.path.join(SCRATCH, "symcache")
    env.pop("_NT_ALT_SYMBOL_PATH", None)
    return env


def key():
    """The KDNET key, from `secrets/kd/key.txt`. It is a secret: it goes into the kd command line and nowhere
    else. Never print the value this returns; `redact()` is what goes into logs and messages."""
    try:
        with open(KEY_FILE, "r", encoding="utf-8") as f:
            value = next((line.strip() for line in f if line.strip()), "")
    except FileNotFoundError:
        sys.exit(f"no KDNET key at {KEY_FILE}; the target has not been set up for debugging yet")
    if not KEY_SHAPE.match(value):
        sys.exit(f"the key in {KEY_FILE} is not four dot-separated groups as kdnet prints them")
    return value


def redact(argv, secret):
    """The command line as it may be shown or logged: the key replaced by a placeholder."""
    return [a.replace(secret, "<key>") if secret else a for a in argv]


def ensure_dirs():
    for directory in (SCRATCH, SYMBOLS, os.path.join(SCRATCH, "symcache")):
        os.makedirs(directory, exist_ok=True)


def read_state():
    try:
        with open(STATE, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def write_state(state):
    ensure_dirs()
    with open(STATE, "w", encoding="utf-8") as f:
        json.dump(state, f, indent=2)


def alive(pid):
    """True while that process id is still running."""
    SYNCHRONIZE, STILL_ACTIVE = 0x00100000, 259
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    handle = k32.OpenProcess(SYNCHRONIZE | 0x0400, False, int(pid))  # + QUERY_LIMITED_INFORMATION
    if not handle:
        return False
    code = ctypes.c_ulong()
    ok = k32.GetExitCodeProcess(handle, ctypes.byref(code))
    k32.CloseHandle(handle)
    return bool(ok) and code.value == STILL_ACTIVE


def ctrl_break(pid):
    """Ask the debugger to break into a running target, the way Ctrl+Break does at its console.

    The server runs in its own console (CREATE_NO_WINDOW) and its own process group, so the event has to be
    sent from a process attached to that console. This function detaches whatever console it has, which is why
    `kd_cmd.py` runs it in a short-lived child of its own instead of in the process that prints the output."""
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.FreeConsole()
    if not k32.AttachConsole(int(pid)):
        return False, f"cannot attach to the console of pid {pid} (error {ctypes.get_last_error()})"
    k32.SetConsoleCtrlHandler(None, True)
    ok = k32.GenerateConsoleCtrlEvent(1, int(pid))  # CTRL_BREAK_EVENT to that process group
    error = ctypes.get_last_error()
    k32.FreeConsole()
    return bool(ok), "" if ok else f"GenerateConsoleCtrlEvent failed (error {error})"


BEGIN, END = "===bc250-kd-begin===", "===bc250-kd-end==="
NOISE = re.compile(r"^(NatVis script (un)?loaded|Microsoft \(R\) Windows Debugger|Copyright \(c\) Microsoft|"
                   r"Opened log file|Command Line:|Symbol search path is:|Executable search path is:)")


def script(commands, trailer, name="cmd"):
    """Write the commands to a script file and return the `-cf` argument for it.

    The commands are fenced by two `.echo` markers so that the answer can be cut out of the banner, the
    extension-gallery chatter and the NatVis lines around it. It has to be a file rather than `-c "a;b;c"`:
    `.echo` (and `.printf`) take the whole rest of the line, semicolons and following commands included, so a
    marker in a `-c` string eats the commands after it. One command per line has no such ambiguity."""
    ensure_dirs()
    path_ = os.path.join(SCRATCH, f"{name}.kdscript")
    with open(path_, "w", encoding="ascii", errors="replace") as f:
        f.write("\n".join([f".echo {BEGIN}"] + list(commands) + [f".echo {END}"] + list(trailer)) + "\n")
    return path_


def clean(text):
    """What the commands printed, without what the debugger printed around them.

    The fence is matched on whole lines: kd also echoes each command after its prompt, so `7: kd> .echo
    ===bc250-kd-begin===` appears before the marker's own output line and must not be mistaken for it. The
    last opening marker is the one taken - a remote client is replayed the server's output history when it
    connects, so markers from earlier commands can appear before this command's own."""
    lines = text.splitlines()
    start = next((i for i in range(len(lines) - 1, -1, -1) if lines[i].strip() == BEGIN), None)
    if start is not None:
        stop = next((i for i in range(start + 1, len(lines)) if END in lines[i]), len(lines))
        lines = lines[start + 1:stop]
    return "\n".join(l for l in lines if not NOISE.match(l.strip())).strip()


def run_kd(argv, env=None, timeout=600, stdin=None):
    return subprocess.run([KD] + argv, env=env or environment(), capture_output=True, text=True,
                          timeout=timeout, input=stdin, errors="replace")
