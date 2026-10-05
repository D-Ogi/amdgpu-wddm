# hostwatch - the black box and the kill switch of the development PC

A kernel debugger on this PC is dangerous to this PC. Two scripts make it survivable: one records what the
kernel pool does, and one kills the debugger before the pool takes the machine down.

```
pwsh -NoProfile -File tools\win\hostwatch\selftest.ps1              offline gate, kills nothing but its own children
powershell.exe -File tools\win\hostwatch\hostwatch.ps1              one line every 10 s, forever
powershell.exe -File tools\win\hostwatch\killswitch.ps1             watch the pool, kill kd.exe when it runs away
powershell.exe -File tools\win\hostwatch\killswitch-dryrun.ps1      live validation of the kill path, by hand
```

Both watchers run until you stop them. `killswitch.ps1` also stops when its stop file appears, which is how a
script ends it:

```
New-Item -ItemType File <BC250_ROOT>\scratch\hostwatch\killswitch.stop
```

## Why this is mandatory

The project's rules do not ask for this watch. They require it. The reasons are measured, on this PC:

- A live `kd` server grew this PC's nonpaged pool at about **0.87 GB/s** and hung the machine within two
  minutes (2026-09-21). After the kill, the memory came back only about two minutes later. The cause is still
  unknown.
- One-shot dump analysis is not safe either. `kd -z` on a dump of about 1 GB spiked the nonpaged pool at
  **372 to 628 MB/s** (2026-09-27, four runs, all ended by this kill switch).

So: the kill switch runs **before** `kd_server.py start` or any `kd -z`, and it keeps running while the
debugger lives. A live `kd` on this PC also needs the owner's consent for that run. Dump analysis is allowed
without it, but not without this watch.

After an exit code 255 or an empty debugger result, read `killswitch.log` before you blame the debugger. The
kill switch is usually the one that ended the run, and the log says why.

## hostwatch.ps1

One line every `-Every` seconds (10 by default), appended with one open and one close, so a hard reset leaves
the record behind:

```
2026-09-21T07:26:03 npp=4824MB pp=1190MB avail=9371MB commit=22184MB handles=142033 top=sqlservr:11874 kd=1:3271
```

`npp` is the nonpaged pool, the figure that killed the machine. `kd=<count>:<handles>` says whether a debugger
was alive at that moment. The log and the stop files stay under `<BC250_ROOT>\scratch\hostwatch`, never on
drive C:. `-Log` sends them elsewhere.

## killswitch.ps1

Every second it reads the nonpaged pool and kills every `kd.exe` when either rule trips:

| Parameter | Default | Meaning |
|---|---|---|
| `-GrowthLimitMB` | 3072 | Hard cap, counted from the pool at start. The host idled near 5 GB on 2026-09-27, so a fixed ceiling would be wrong |
| `-RateMBps` | 300 | One-second growth that means the leak is running. The measured leak was about 870 MB/s |
| `-ProcessName` | `kd` | What to kill |
| `-DryRun` | off | Log the trigger, kill nothing |
| `-StateDir` | `<BC250_ROOT>\scratch\hostwatch` | Where `killswitch.log` and `killswitch.stop` live |

Every decision goes to the log with its reason, and the watch continues after a kill, because the first kill
does not always end the leak.

## The two self-tests

`selftest.ps1` is the offline gate, and `tools/quality/quick.ps1` runs it as the check `hostwatch`. Four
checks: all scripts parse with the Windows PowerShell 5.1 parser, the trigger fires against an impossible
limit, the dry run kills nothing, a stale stop file is cleared at start instead of ending the run, the run
ends when the stop file appears again, and `hostwatch.ps1` writes real pool samples at the interval it was
given. It kills only its own two children.

`killswitch-dryrun.ps1` is the live validation, and it is run by hand. It starts a hidden `ping -n 40
127.0.0.1`, lets the kill switch trigger against it in a dry pass and then in a real pass, and exits 0 only
when the dry pass left the process alive and the real pass killed it. **Hazard:** the kill switch selects
victims by process name, so the real pass kills every `ping.exe` on this PC, not only the dummy one. That is
why it stays out of `quick.ps1`. First validation: 2026-09-27 19:10, both passes as expected.

## What is not here

The run captures stay in the workspace: `hostwatch.log`, `killswitch.log`, the stop files and the pool
snapshots of the 2026-09-21 hang are samples of this PC's kernel memory, not instruments, so they are not
in this repository.

## The operator copy

The copy at `<BC250_ROOT>\scratch\hostwatch` is the one an operator starts beside a debugger session, and the
workspace rules name it by that path. This directory holds the source of record. Change the file here first,
then copy it over after a deliberate check. The operator copy has no `selftest.ps1` and keeps the workspace
root in its defaults.
