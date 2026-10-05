# lab-emerg - the emergency channel to unit A when ssh fails

Twice the lab's sshd stopped accepting connections while the machine itself was healthy (BD-051). The owner then
asked for a second way in that does not depend on sshd. This is it: a small HTTP listener on the unit, a signed
request from here, and a fixed list of actions.

**Use this channel before you ask the owner for help and before you cycle the mains.** That is the order the
workspace rules set: ssh, then this channel, then the smart plug, then the owner.

```
python tools\win\lab-emerg\lab-emerg.py status                  boot time, sshd, port 22, processes, boot steering
python tools\win\lab-emerg\lab-emerg.py restart-sshd            restart the service, then status again
python tools\win\lab-emerg\lab-emerg.py kill-game               end the game process
python tools\win\lab-emerg\lab-emerg.py ps FILE.ps1 [SECONDS]   run a local script there (job, 120 s, 600 max)
python tools\win\lab-emerg\lab-emerg.py ps -c "<text>" [SECONDS]
python tools\win\lab-emerg\lab-emerg.py get REMOTE LOCAL        fetch a file, SHA-256 checked
python tools\win\lab-emerg\lab-emerg.py put LOCAL REMOTE        write a file
python tools\win\lab-emerg\lab-emerg.py copy|move REMOTE_SRC REMOTE_DST
python tools\win\lab-emerg\lab-emerg.py delete|mkdir|list|acl|tail REMOTE [N]
python tools\win\lab-emerg\lab-emerg.py desktop-cpu             route the desktop to the CPU at the next boot
python tools\win\lab-emerg\lab-emerg.py kill-dwm accept-bd060   last resort for a hung compositor
python tools\win\lab-emerg\lab-emerg.py usb-boot status|windows|linux:N
python tools\win\lab-emerg\lab-emerg.py reboot reboot-now
```

Each command prints one JSON object and exits 0 only when the unit reported success. Files are limited to
16 MB in both directions. Nothing stays running on the development PC.

## How a request is protected

The listener is `listener.ps1`. It runs as SYSTEM from the scheduled task **"Lab emergency channel"** (at
startup, restarted on failure) and holds an `http.sys` listener on TCP 8722. The firewall rule "BC250 emergency
8722" admits the **local subnet only**.

A request is a `POST /lab/` with these headers:

| Header | Content |
|---|---|
| `X-Ts` | Unix seconds. More than 120 s away from the unit's clock is refused |
| `X-Nonce` | 32 hex characters. A reused nonce is refused |
| `X-Action` | The action name. Only the actions above exist |
| `X-Arg` | The argument, base64 of UTF-8 |
| `X-Sig` | `HMAC-SHA256(key, ts \n nonce \n action \n arg \n sha256(body))`, hex |

The signature therefore covers the action, the argument and the body, not only the body. The body is raw data:
a script for `ps`, the file bytes for `put`, empty otherwise.

**Nothing runs between requests.** The listener does not poll, does not spawn anything of its own and starts no
work that outlives a response. A `ps` job carries its own deadline and the listener waits for it.

## The file actions and their ACL rule

The owner set this rule on 2026-10-01, because `sshd_config` needs to keep its own security descriptor:

- Overwriting an existing file **keeps that file's ACL** and leaves `<file>.emerg-<utc>.bak` beside it.
- A new file inherits its directory's ACL and gets **Administrators as owner**, so the interactive user can
  read and change what SYSTEM wrote. New directories do the same.

## The key

The HMAC key is a credential. It lives in two places and in no other:

- `<BC250_ROOT>\secrets\lab-emergency\key.bin` on the development PC. `BC250_LAB_EMERG_KEY` points elsewhere.
- `C:\BC250\emergency\key.bin` on the unit, ACL SYSTEM and Administrators only.

The scripts in this directory name that path and never its content. The key is never printed, never logged and
never copied into this repository. `install.ps1` moves the key out of `C:\BC250\tmp` instead of leaving a copy
there.

`BC250_ROOT` is the workspace root, by default the parent directory of this repository.

## The addresses

`lab-emerg.py` takes the unit's addresses from `tools/win/target.py`, which reads them from
`<BC250_ROOT>/secrets/client/target.json`, outside this repository. It tries them in order and gives the first
one the full timeout, because the Wi-Fi dongle drops out. No address is written down here.

## Installing or removing the channel

`install.ps1` runs on the unit, elevated, through `target.py ps`, with `listener.ps1` and the key already pushed
to `C:\BC250\tmp`:

```
python tools\win\target.py push tools\win\lab-emerg\listener.ps1 --to C:\BC250\tmp
copy the key to C:\BC250\tmp under the name emergency-key.bin   (install.ps1 looks for that name and moves it)
python tools\win\target.py ps tools\win\lab-emerg\install.ps1
python tools\win\target.py ps tools\win\lab-emerg\install.ps1 -Uninstall
```

`-Uninstall` removes the task, the firewall rule and any running listener. It keeps the files in
`C:\BC250\emergency`, the key among them.

The task name matters. The trial kits refuse to run while a task matching `BC250|DWM|G0|WSI` exists, and the
channel is not a test task. Under its first name, "BC250 emergency channel", it made trial 222's Capture stage
fail. `install.ps1` therefore registers "Lab emergency channel" and removes the old name if it is still there.
`scripts/restart-listener-task.ps1` restarts the task without reinstalling anything.

A known wart: the comment at the top of `listener.ps1` still names the old task. The file is byte-identical to
the one installed on the unit, and it stays that way until the next deployment of the listener.

## scripts/

Helpers that the channel carries to the unit with `ps`. They are evidence tools from the sshd stall
investigation and the KMD work, not part of the protocol:

| Script | Runs on | What it does |
|---|---|---|
| `sshd-stall-evidence.ps1`, `sshd-stall-forensics.ps1`, `sshd-stacks.ps1`, `sshd-dump.ps1` | unit A | The BD-051 captures: service state, listening sockets, thread stacks, a process dump |
| `tcp22-owners.ps1`, `stall-snapshot.ps1` | unit A | Who holds port 22, and one snapshot of the stalled state |
| `early-log.ps1`, `restart-early-log.ps1` | unit A | The KMD log ring wraps in about 1.5 s. These capture the first seconds after a driver start |
| `boot-events.ps1`, `kmd-state.ps1` | unit A | Boot and shutdown events, and the driver's registry state |
| `restart-listener-task.ps1` | unit A | Print the listener hash, stop the task, start it again, report its state |
| `sshd-heal.sh` | this PC | When an authenticated ssh command does not answer: capture the forensics through the channel, restart sshd, ask again. Exit 0 ssh answers, 1 healed, 2 still down |
| `guard-http.sh` | this PC | Temperature guard for a game session with no held ssh session. It reads the KMD dpm line every 30 s and ends the game after two readings above 87 C. `RESET_FLOOR=1` also resets the DPM floor |

The two shell scripts find `lab-emerg.py` and `target.py` from their own location, so they work from either copy
of the kit. They poll every 30 s on purpose. Never go faster: the lab's sshd penalises fast probing, and a
per-second channel loop is one half of the BD-051 stall itself.

## selftest.sh touches the lab

`selftest.sh` is a round trip of every file action in `C:\BC250\tmp\emerg-test` and two short `ps` jobs. It
writes on unit A, so it is not a host test and `tools/quality/quick.ps1` does not run it. The offline gate is
`parse-check.ps1`, which parses every script of this kit with the Windows PowerShell 5.1 parser (the engine the
listener runs under) and compiles `lab-emerg.py`. `quick.ps1` runs it as the check `lab-emerg`.

```
powershell.exe -NoProfile -File tools\win\lab-emerg\parse-check.ps1
bash tools\win\lab-emerg\selftest.sh            TOUCHES THE LAB
```

## The operator copy

The copy at `<BC250_ROOT>\scratch\lab-emergency` is the one the workspace rules name, and operator scripts call
it by that path while the lab is unreachable. This directory holds the source of record. Change the file here
first, then copy it over after a deliberate check, because the other copy is the one someone runs during an
outage. A fix that stays here never reaches the recovery that actually happens.

Two files of the workspace copy are not here: `guard-232.sh`, a one-off from session 232 that names a fixed
`C:\BC250\kmd185` client path, and a `.pre-gamerunner` backup of `guard-http.sh`. `guard-http.sh` resolves the
client at every call and supersedes both.

## What this channel is not

It is not a shell and it is not a second sshd. It runs a fixed list of actions, each of them bounded. It cannot
fix a machine that does not boot, and it cannot answer when the unit is hung below the network stack. That
case belongs to the smart plug in `tools/win/smartplug`, and a look at the case through `tools/win/labcam`
before you cut the mains.
