# lab-runner - one game session on the lab unit

A game session is a measurement, not a game. `game-runtime.ps1` starts one game on unit A in the console
session, watches it, records what the driver and the hardware did, and ends the session itself. Everything
that is specific to a game comes from a profile in `profiles/`, so a new game needs a profile and no new
code. The runner also carries the interactive channel: the operator sends keys and clicks from the
development PC, and only text leaves the lab, never a full-resolution image.

## Two rules of a measured session

These two rules are owner decisions. Do not tune them.

**The thermal stop.** The runner stops the session when the KMD temperature (Tctl) stays at or above 87 C
for 10 s, or at once at 89 C (owner, 2026-10-04). A scene load spikes the temperature for a few seconds, so
a single hot sample is not a stop. An unreadable sample is -1 and never a temperature: only a readable
sample below 87 C ends a hot stretch. `test_lab_runner.py` holds this rule against the script.

**No upscaler and no dynamic resolution.** A measured session runs at the native resolution with the
upscaler off and the resolution scale at 100 percent (owner, 2026-10-01). The runner does not read the
game's graphics options, so the operator sets them and checks them again after every preset change: the
Witcher 3 HIGH preset sets anti-aliasing to AMD FSR by itself. `test_lab_runner.py` refuses a profile whose
command line asks for an upscaler or a dynamic resolution.

## What is here

| File | What it does |
|---|---|
| `game-runtime.ps1` | The session on the lab: the start (the Steam client or the executable), the running gate, the module witness, the frame and job counters, the temperature watch with the thermal stop, the interactive channel, and the end of the session. `-DryRun [-Offline]` prints the plan and starts nothing |
| `ocr-frame.ps1` | Full-resolution OCR of the lab screen for the interactive channel (Windows.Media.Ocr, in process, no window). `game-runtime.ps1` dot-sources it. The frame is read and deleted at once: only text and boxes leave the lab |
| `kmdlog-stream.ps1` | Appends the kernel driver log, the clock and temperature readings and the memory counters to `game-kernel.log` while the game runs, each write through the cache, so a machine-wide stop keeps what was read |
| `profiles/*.json` | One profile per game: the Steam entry, the executable, the APIs with their router rule and module witness, the process names, the readiness mode, the settings policy, the modules to witness and the default session length |
| `session.py` | The host side of a session outside the Witcher 3 route: `facts` prints the profile as the kit reads it, `dry` stages an attempt and runs `game-runtime.ps1 -DryRun -Offline` under Windows PowerShell 5.1 |
| `profile-check.py` | Checks every profile with the trial kit's own loader and prints what a session would use. Offline: no lab, no Steam data, no game file |
| `make-unity-profiles.py` | Writes the profiles of the small Unity games (the player selects the API with `-force-d3d11` or `-force-d3d12`) |
| `input/inp.py` | The host side of the interactive channel: one ssh call per command, an optional screenshot through `bc250mon/mon.py` at 0.33 scale |
| `input/input-server.ps1` | The lab side: a bounded input server in the interactive session. It polls a command file every 100 ms and runs the actions with `SendInput` |
| `input/start-input.ps1` | Registers and starts the server as the one-shot task "Lab game input", and `-Stop` ends it |
| `input/input-exec.ps1` | Writes the next command for the server and waits for its answer |
| `smoke/*.ps1` | Small lab helpers around a session: the boot state, one click or key tap, the game witness, the evidence of the start, the modules, the stacks and their symbols, the thread rates, the windows, the shader cache, the crash dump and the game settings |
| `test_lab_runner.py` | The host tests: the profiles, the two owner rules and the screenshot directory. The `lab-runner` check of `tools/quality/quick.ps1` runs them |

## How to run it

```
python tools/win/lab-runner/profile-check.py                     every profile, as a session reads it
python tools/win/lab-runner/profile-check.py rottr the-ascent    two of them
python tools/win/lab-runner/session.py facts rottr               the shell facts a session script takes
python tools/win/lab-runner/session.py dry 123 rottr --out <dir> stage an attempt and dry-run the runtime
python tools/win/lab-runner/input/inp.py start game-rottr 1200   start the input server (at most 1200 s)
python tools/win/lab-runner/input/inp.py game-rottr "clickat:0.64:0.89;wait:800;tap:1C" --shot
python tools/win/lab-runner/input/inp.py stop game-rottr         quit the server and remove the task
python -m unittest discover -s tools/win/lab-runner               the host tests
```

## What it needs

`game-runtime.ps1` does not run by itself. The trial kit stages it into one package directory together with
`durable.ps1`, `config.json` and `game-profile.json`, and the kit's dispatcher runs the modes. The kit lives
in the workspace scratch directory and is not part of this repository, because every attempt freezes its own
copy of the package as evidence. `session.py` and `profile-check.py` therefore ask for the kit:
`BC250_ROOT` names the workspace root, by default the parent directory of this repository.

The host-side tools reach the lab only through `tools/win/target.py`, which holds the address and the ssh
options, and they use the overlay through `tools/win/bc250mon/mon.py`. No address and no key is in this
repository.

Witcher 3 sessions do not go through `session.py`: they have their own session script in the kit, because a
preset, a resolution and an upscaler check belong to it. `session.py` refuses the profile.

## Hazards

- **Screenshots of the lab screen never enter this repository.** They go to the workspace scratch directory,
  and `BC250_SHOTS_DIR` names another directory outside the repository. The lab monitor also shows game
  content, and the owner asked for resized images only.
- **The runner never overwrites a capture.** An existing `game` directory in the package is an error, because
  attempt 067 kept nothing after a second start.
- **The input server runs in the interactive session**, as the one-shot task "Lab game input". The name stays
  outside the competing-task gate of the trial kits (`BC250|DWM|G0|WSI`), which once refused a capture under
  another name. The server ends at its bound whatever happens, and the bound is at most 1200 s.
- **Keys and buttons are held 150-250 ms.** A game at a few frames per second misses a shorter press.
- **Do not open a held ssh session before the trial runs.** In trials 220 and 221 a session opened before the
  push met an sshd accept stall (BD-051).
- The kernel log stream ends by itself after 260 s plus the excess of the session length over 300 s. A longer
  session needs the longer stream, or the records of a late fault are lost (trial 184).
- `smoke/stacks-of.ps1` and `smoke/stack-sym.ps1` attach `cdb` without invasion (`-pv`): it suspends a thread
  only while it reads the stack, injects nothing, and the process continues after the detach. Use them on a
  game that already renders, and expect a reset of the device if a stop meets GPU work in flight.

## The scratch copy

The operators still call the copy in the workspace scratch directory, and the trial kit stages the runtime
from its own template. This copy is the source of record: change it here, then copy it to the kit.
