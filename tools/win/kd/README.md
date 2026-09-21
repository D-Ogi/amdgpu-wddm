# kd - kernel debugger as a background server

`kd.exe` held open in the background, with a second script that throws commands at it and prints the answer.
That shape exists for two reasons: a debugger started per command would miss every bugcheck in between, and a
non-interactive session cannot sit at a `kd>` prompt.

The debugger is the portable WinDbg package unpacked under `P:\BC-250\toolchain\windbg\x64\amd64\` - nothing
is installed. Symbols, symbol cache and logs live under `P:\BC-250\scratch\`; `_NT_SYMBOL_PATH`,
`DBGHELP_HOMEDIR` and `_NT_SYMCACHE_PATH` are set explicitly by these scripts, so nothing lands on `C:`.

| File | What it is |
|---|---|
| `kdenv.py` | Shared: paths, symbol path, environment, the KDNET key, process checks, output fencing |
| `kd_server.py` | `start` / `stop` / `restart` / `status` / `firewall` |
| `kd_cmd.py` | Send commands to the server, or run them one-shot against a dump file |

## A dump file

Needs no server and no target:

```
python tools/win/kd/kd_cmd.py --dump P:\BC-250\scratch\dumps\092126-43343-01.dmp "!analyze -v"
python tools/win/kd/kd_cmd.py --dump P:\BC-250\scratch\dumps\092126-43343-01.dmp "vertarget" "lm m nwifi"
```

Or served, when the same dump is going to be asked many questions:

```
python tools/win/kd/kd_server.py start --dump P:\BC-250\scratch\dumps\092126-43343-01.dmp
python tools/win/kd/kd_cmd.py "kb" "!pool ffff9a83385db060"
python tools/win/kd/kd_server.py stop
```

## A live target over KDNET

The target must have been set up for network debugging and rebooted, and the key it was given must be in
`P:\BC-250\secrets\kd\key.txt`, one line, four dot-separated groups as `kdnet.exe` prints them.

```
python tools/win/kd/kd_server.py start          # waits for the target to connect; returns at once
python tools/win/kd/kd_server.py status         # server alive, and what the log says about the connection
python tools/win/kd/kd_cmd.py --break "lm m bc250*" "!analyze -v"
python tools/win/kd/kd_cmd.py --go
python tools/win/kd/kd_server.py stop
```

A running target has no debugger prompt, so commands sent to it would wait for the next bugcheck: pass
`--break` first unless the target is already broken in, and `--go` to let it run again. After a bugcheck the
target is already broken in and `--break` is unnecessary.

**The server is meant to outlive target reboots.** KDNET reconnects by itself and `kd` waits for it, which is
the whole point of leaving it running: a bugcheck that happens between two commands is caught. `status` reads
the connection lines out of the log and says `connected` or `waiting to reconnect`.

## Details worth knowing

- **The key is a secret.** It is read from `secrets/kd/key.txt` and put on `kd`'s command line, nowhere else;
  the state file and `status` show it as `<key>`. Note that a command line is not a hiding place: anyone who
  can list processes on this PC (`Get-CimInstance Win32_Process`) can read the key off the running `kd.exe`.
- **Firewall.** KDNET is inbound UDP on port 50000 to this PC. `kd_server.py firewall` reports the profiles
  read-only; opening the port needs an elevated shell and these scripts never change a rule.
- **One port, one server.** Only one debugger can hold UDP 50000. `start` refuses if its own server is already
  running, but it cannot see a `kd.exe` somebody else started by hand - if the target never connects, look for
  another `kd.exe` first.
- **How commands are delivered.** They go in a script file passed with `-cf`, one per line, not in `-c "a;b"`:
  `.echo` and `.printf` swallow the rest of a `-c` line, semicolons and following commands included. The
  commands are fenced by two `.echo` markers so the answer can be cut out of the banner and the NatVis noise.
  The client asks for one line of output history (`-clines 1`), and the fence takes the *last* opening marker,
  because a connecting client is replayed part of the server's earlier output.
- **`-server` has to be the first argument** on kd's command line; it rejects it anywhere else.
- **The server runs in its own hidden console and its own process group.** That is what makes `--break` work:
  it is a `CTRL_BREAK_EVENT` sent to that group, exactly what Ctrl+Break at a `kd` console does. `kd_cmd.py`
  does it in a short-lived child of its own, because sending the event means detaching from one's own console.
- **Overrides**: `BC250_KD_PORT` (50000), `BC250_KD_PIPE` (`bc250kd`), `BC250_ROOT`.

## What has been run

Verified against the dump `092126-43343-01.dmp`: one-shot `--dump`, and the server plus `kd_cmd` round trip
(`vertarget`, `lm m nwifi`, `!analyze -v` - symbols downloaded into `P:\BC-250\scratch\symbols`, bucket
`0x1E_C000001D_nwifi!NwfReadMsg`), `status`, `stop`. The live KDNET path and `--break` / `--go` have not been
exercised against a target yet.
