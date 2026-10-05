# kd - kernel debugger as a background server

`kd.exe` held open in the background, with a second script that throws commands at it and prints the answer.
That shape exists for two reasons: a debugger started per command would miss every bugcheck in between, and a
non-interactive session cannot sit at a `kd>` prompt.

The debugger is the portable WinDbg package unpacked under `<BC250_ROOT>\toolchain\windbg\x64\amd64\` - nothing
is installed. Symbols, symbol cache and logs live under `<BC250_ROOT>\scratch\` (`BC250_ROOT` is the
workspace root, by default the parent directory of this repository); `_NT_SYMBOL_PATH`,
`DBGHELP_HOMEDIR` and `_NT_SYMCACHE_PATH` are set explicitly by these scripts, so nothing lands on `C:`.

| File | What it is |
|---|---|
| `kdenv.py` | Shared: paths, symbol path, environment, the KDNET key, process checks, output fencing |
| `kd_server.py` | `start` / `stop` / `restart` / `status` / `firewall` |
| `kd_cmd.py` | Send commands to the server, or run them one-shot against a dump file |
| `analyze-kernel-dump.ps1` | One-shot triage of a dump on the lab, through `target.py ps` |

## A dump file

Needs no server and no target:

```
python tools/win/kd/kd_cmd.py --dump $env:BC250_ROOT\scratch\dumps\092126-43343-01.dmp "!analyze -v"
python tools/win/kd/kd_cmd.py --dump $env:BC250_ROOT\scratch\dumps\092126-43343-01.dmp "vertarget" "lm m nwifi"
```

Or served, when the same dump is going to be asked many questions:

```
python tools/win/kd/kd_server.py start --dump $env:BC250_ROOT\scratch\dumps\092126-43343-01.dmp
python tools/win/kd/kd_cmd.py "kb" "!pool ffff9a83385db060"
python tools/win/kd/kd_server.py stop
```

## A dump file on the lab

`analyze-kernel-dump.ps1` does the same one-shot analysis inside the lab's own Windows. Run it with
`target.py ps`. The lab keeps its own copy of the debugger under `C:\BC250\tools\kd`.

```
python tools/win/target.py ps tools/win/kd/analyze-kernel-dump.ps1 -Dump C:\Windows\MEMORY.DMP
python tools/win/target.py ps tools/win/kd/analyze-kernel-dump.ps1 -Dump C:\BC250\tmp\kmd.dmp -Pdb C:\BC250\tools\kd\pdb -Out C:\BC250\tmp\kd-triage.txt -Extra "!pool;!vm 1"
```

The script prints a short summary of the bugcheck lines. It leaves the full debugger log on the lab at
`-Out`, and the exact commands of the run in a `.cmds` file next to that log. Copy the log here with
`python tools/win/target.py pull <remote log> <local file>`. Add `-NoSymbolServer` when the lab cannot
reach the public symbol server, because an unreachable server makes every `.reload` wait for a timeout.

`-Extra` takes the extra debugger commands as one string, and each `;` in it starts a new command.
`target.py ps` runs the script with `powershell -File`, and `-File` cannot bind an array parameter.

The script also checks itself offline. This check starts no debugger and reads no dump:

```
pwsh -NoProfile -File tools/win/kd/analyze-kernel-dump.ps1 -SelfTest
```

### Why this one runs on the lab

A debugger costs the development PC a lot of nonpaged kernel memory. Two measurements show it:

- One-shot `kd -z` on a dump of about 1 GB grew the nonpaged pool of this PC at 372 to 628 MB/s. The
  host kill switch stopped all four runs of 2026-09-27.
- The live KDNET server leaked nonpaged kernel memory at about 0.87 GB/s and hung this PC after about
  110 s on 2026-09-21. The KDNET section below gives more detail.

For this reason the dump analysis runs on the lab. A dump on the development PC is still possible, but it
needs the host kill switch and short sequential runs. The lab path also saves a copy of 1 GB or more over
the network, and it needs no KDNET key.

### The debugger files on the lab

This repository holds no Microsoft binaries. Push the portable WinDbg files from
`<BC250_ROOT>\toolchain\windbg\x64\amd64\` to `C:\BC250\tools\kd` one time. The analysis needs these
files:

```
kd.exe  dbgcore.dll  dbgeng.dll  dbghelp.dll  dbgmodel.dll  msdia140.dll  srcsrv.dll  symsrv.dll
winext\ext.dll  winxp\kdexts.dll  triage\pooltag.txt  triage\triage.ini
```

Put the PDB of the deployed kernel driver in `C:\BC250\tools\kd\pdb`. The script adds that directory to
the symbol path. The public symbol server does not hold our driver, so the debugger takes the PDB from
there. `-Module` names the driver for `.reload` and `lmvm`, and the script refuses a name that is not a
plain module name.

## A live target over KDNET

> **Disabled since 2026-09-21.** Starting the live server made the development PC leak nonpaged kernel memory
> at about 0.87 GB/s (hang after ~110 s; reproduced once under supervision: 57 GB in 70 s, released about 70 s
> after the killed process had gone). The cause
> inside the launch is not known yet; `kd_server.py start` refuses live mode unless
> `BC250_KD_LIVE_AT_MY_OWN_RISK=1`. Dump files are not affected. The text below describes the intended use.

The target must have been set up for network debugging and rebooted, and the key it was given must be in
`<BC250_ROOT>\secrets\kd\key.txt`, one line, four dot-separated groups as `kdnet.exe` prints them.

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
(`vertarget`, `lm m nwifi`, `!analyze -v` - symbols downloaded into `<BC250_ROOT>\scratch\symbols`, bucket
`0x1E_C000001D_nwifi!NwfReadMsg`), `status`, `stop`. The live KDNET path and `--break` / `--go` have not been
exercised against a target yet.

`analyze-kernel-dump.ps1` passes its offline self-test under PowerShell 7 and under Windows PowerShell
5.1, which is the shell of an SSH session on the lab. Its refusal paths also pass: a missing dump, a
missing `kd.exe` and a module name that carries a command each return exit code 2. A full run with a
stand-in for `kd.exe` in place of the debugger wrote the expected `.cmds` file, kept only the named lines
in the summary and returned the exit code of the child. No run against a real dump on the lab exists yet,
so this directory keeps no output of one.
