# bc250mon - monitor and overlay on the BC-250's own screen

A window the owner can see on the lab machine while experiments are driven remotely: what is being done
right now, temperature and clocks, results, a log, and a brake. One .NET Framework 4.8 executable (part of
Windows, nothing to install), started at logon in the interactive session by a scheduled task, elevated
because `bc250rd.sys` is admin-only.

## Architecture

```
 providers (threads)                  remote agent over SSH      owner at the machine
 GpuProvider, SystemProvider,         mon.py -> HTTP            hotkeys, buttons
 KmdProvider, KmdInfoProvider         127.0.0.1:2250
        |                               |                               |
        v                               v                               v
   +---------------------------- State (State.cs) ----------------------------+
   | panels (named, ordered) | status line | log ring + flushed file | STOP   |
   +---------------------------------------------------------------------------+
        ^                               ^                               |
        |                               |                               v
   Driver.cs (bc250rd.sys)        Actions.cs (one table of named actions)   OverlayForm.cs (paints State)
```

- **State** is the only meeting point. Providers, API and UI never call each other, so each can be
  replaced or extended alone.
- **Providers** own one panel each and poll on their own period. Adding a data source (the future KMD's
  debug channel, ETW, TDR events, fan controller) means one class implementing `IProvider` and one line in
  `Program.cs`.
- **Actions** are a table of named operations. Buttons are generated from it, hotkeys and the HTTP API call
  into it, so a new control is reachable from everywhere at once and every invocation is logged with its
  origin.
- **API**: JSON over HTTP on the loopback interface only; it is reached through SSH (`mon.py`), never from
  the network. Remote panels let an experiment put its own table on the screen without changing this tool.
- **Screenshot** (`Screenshot.cs`) is the remote side's only pair of eyes: the SSH session lives in session 0
  and has no desktop, so only this process, running in the console session, can capture anything.
- **Log**: every event is appended and flushed to `C:\BC250\mon\log\<date>.log` first, so the file is
  useful after a hard hang.

## The bc250kmd panel

`KmdProvider.cs` polls the miniport's registry key every 5 seconds and shows what the driver managed to write
before it stopped talking (ADR 0006 points 3 and 4, `driver/kmd/README.md`):

| Row | From |
|---|---|
| `Driver` | does `HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd` exist |
| `Key` | only when `--kmd-key` points somewhere else, so the overlay cannot lie about what it is watching |
| `Stage` | `Parameters\LastStage`, number plus name |
| `History` | the tail of `Parameters\StageHistory` |
| `Unconfirmed` | `Parameters\UnconfirmedStarts`, green at 0, amber at 1, red at 2 (the guard then refuses to start) |
| `Confirm` | where the automatic confirmation stands |

When the service key does not exist the panel says `not installed` and writes nothing to the log; the state
of the lab today is not an error.

**Confirming a start** (ADR 0006 point 3). This process is started at logon in the interactive session, so
the fact that it is polling at all is the evidence that the desktop came up. Once the driver is installed, the
monitor has been running for 60 seconds and `LastStage` has reached 61 (`StageFirstPresentDone`), it writes
`UnconfirmedStarts = 0` once per boot and logs it. "Once per boot" survives a restart of the monitor inside
one boot through the marker file `C:\BC250\mon\kmd-confirmed.txt`, which holds the system's `BootId`.

Two actions drive the same thing by hand during an install: `kmd.confirm` forces the reset, `kmd.budget`
prints the current values into the log. Neither has a button; they are for `mon.py action`.

The stage names are a copy of `enum BC250_STAGE` in `driver/kmd/bc250kmd.h`. `test_stages.py` parses both and
fails the build if they drift apart:

```powershell
python -m unittest discover -s tools/win/bc250mon      # build.ps1 runs this too
```

`--kmd-key HKCU\...` (or the environment variable `BC250MON_KMD_KEY`) points the provider and the actions at
another registry key. It is a debug switch: it lets the "installed" rows be exercised on a machine where no
such service exists, without administrator rights.

## The "bc250kmd live" panel

`KmdInfoProvider.cs` is the KMD panel's practical-debugging counterpart (owner's request, 2026-09-22): where
`KmdProvider` reads the registry trail (works even when the driver has stopped answering), this one asks the
driver itself, every 5 seconds, by running `C:\BC250\kmd\bc250kmd_cli.exe info` (`BC250_ESCAPE_GET_INFO`) and
condensing its output:

| Row | From |
|---|---|
| `Version` | the driver build |
| `Stage` | `last stage`, same numbers as the KMD panel's `Stage`, red at 90/91 |
| `Mode` | `display-only` or `FULL WDDM TABLE` (amber) |
| `Presents` | the display-only path's own present counter |
| `Counters` | the full table's own present counters, blits and flips (0 outside it) |
| `Gates` | `mmio`, `mmio writes`, `vram`, `vram writes`, `gart`, `psp`, `gfx`, `ih` |
| `Temperature` | Tctl, via `Driver.cs` - the same `bc250rd.sys` method `tools/win/bc250rd/temp.py` drives from the development PC, not a second copy of it: `GpuProvider`'s panel already shows it too, this one repeats it next to the driver's own state for one glance instead of two |

When `bc250kmd_cli.exe` is missing, times out, or the escape itself refuses (not an administrator, or
`bc250kmd` is not the adapter's active driver), the panel shows that one line under `info` instead of going
empty or throwing.

## Scanout screenshots (the full WDDM table's own picture)

Under the full WDDM table, `screenshot`'s GDI capture (`Screenshot.CaptureScreen`, `CopyFromScreen`) reads the
CDD's surfaces and comes back solid black (facts M84) - it is not looking at what the display controller is
actually scanning out. `mon.py scanout` is the counterpart that is: it runs `bc250kmd_cli fbdump`
(`BC250_ESCAPE_RUN_FBDUMP`, `driver/kmd/dcn.c` - read-only, HUBP0's own registers) on the target through the
same SSH transport `mon.py` already uses for a command, pulls the BMP back with `target.py pull`, and downsizes
it the way `screenshot` does (half scale by default) - in pure Python (`struct` + `zlib`, no Pillow needed on
either end, matching the workspace rule against installers): a small nearest-neighbour resample and, for the
default `png` format, a minimal hand-rolled PNG encoder. `--format bmp` skips the encoder and writes the
(downscaled) BMP as-is. See `tools/win/bc250kmd_cli/README.md`'s fbdump section for the escape itself.

```
mon.py scanout [--scale 0.5] [--format png|bmp] [--out FILE]
```

## The brake

`Ctrl+Alt+F12` or the red button sets the STOP flag: a banner on the overlay, `GET /flags` returns
`{"stop": true}` and the file `C:\BC250\mon\STOP` exists. Every test script that can run for more than a
few seconds has to poll one of the two and end. `mon.py stop?` does it from the PC (exit code 1).

## API

`http://127.0.0.1:2250`, loopback only, no authentication, reached through SSH.

| Endpoint | What it does |
|---|---|
| `GET /state` | everything the overlay shows: status, panels, log, stop flag |
| `GET /flags` | `{"stop": bool}` - what long-running test scripts poll |
| `POST /status` | `{"text": "...", "level": "info\|good\|warn\|error"}` |
| `POST /log` | `{"text": "...", "level": "...", "source": "..."}` |
| `PUT /panel/<name>` | `{"title": "...", "order": 100, "rows": [["label", "value", "level"], ...]}` |
| `DELETE /panel/<name>` | remove a remote panel |
| `GET /actions` | the action table |
| `POST /action/<name>` | invoke one action, arguments in the body |
| `GET /windows` | visible top-level windows that have a title |
| `GET /screenshot` | the primary screen as `image/png` or `image/jpeg` |
| `GET /screenshot/window` | one window, `?handle=0x...` or `?title=<substring>` |

Both capture endpoints take `scale` (0.1 to 1.0, default 0.5, bicubic downscale), `format` (`png` default or
`jpg`), `quality` (1 to 100, default 80, `jpg` only) and `overlay` (`1` default, `0` leaves this tool's own
window out of the picture). They answer with the image bytes and an `X-Capture-Size: <w>x<h>` header;
`/windows` and every error answer with JSON. A window is found by handle, or by the first title that contains
the given text, case-insensitive, nearest the front first.

**Every capture is logged** with the source `screenshot`, so the owner reads on the overlay that a picture of
their screen was taken. That is the deal, not a debug aid.

## Controls

| Hotkey | Effect |
|---|---|
| `Ctrl+Alt+F9` | Toggle interactive mode: the window accepts the mouse and shows the action buttons. Otherwise it is click-through and never takes focus |
| `Ctrl+Alt+F10` | Hide / show |
| `Ctrl+Alt+F12` | STOP |

Function keys, not letters. `AltGr` is `Ctrl+Alt` on the Polish layout and on every other layout that has
one, so a global `Ctrl+Alt+<letter>` eats the character the owner is typing: `Ctrl+Alt+S` swallowed `s` with
an acute accent. A hotkey another process already holds is refused silently by Windows, so the monitor logs a
warning when a registration fails rather than leaving a dead key.

Buttons today: STOP, clear stop, 1000 MHz / 820 mV, stock 1500 MHz, hide, back to click-through.
Temperature colours: green below 87 C, amber from 87 C (the stop limit; 85 C before 2026-10-01), red from 92 C
(Tctl).

## Build and deploy

```powershell
pwsh tools\win\bc250mon\build.ps1 -Out P:\BC-250\scratch\build\bc250mon
```

Copy `bc250mon.exe` to `C:\BC250\mon\` and register a task "at logon of the lab user, interactive, highest
privileges" that runs it. To replace a running one, stop the process, overwrite the file and start the task
again, so the new one lands back in the interactive session:

```powershell
Get-Process bc250mon | Stop-Process -Force
Copy-Item C:\BC250\mon\new\bc250mon.exe C:\BC250\mon\bc250mon.exe -Force
schtasks /run /tn "BC250 monitor overlay"
```

## mon.py

Every call goes over SSH, so the JSON body travels base64-encoded and images come back base64 on one line.
Which machine and which of its addresses is `tools/win/target.py`'s decision, from the configuration outside
the repository; `mon.py` holds no address and no ssh options of its own.

```
mon.py state
mon.py status "reading GC block" [info|good|warn|error]
mon.py log "text" [level]
mon.py panel e02 "E02 control read" "registers=5542" "identical=5074:good" "hangs=0:good"
mon.py unpanel e02
mon.py action clock.cool              mon.py action clock.set '{"mhz": 1200, "mv": 850}'
mon.py stop?                          exit code 1 if the owner asked to stop
mon.py windows                        handle, process, geometry and title of every titled window
mon.py screenshot [--scale 0.5] [--format png|jpg] [--quality 80] [--overlay 0|1]
                  [--window TITLE | --handle 0x...] [--out FILE]
mon.py scanout [--scale 0.5] [--format png|bmp] [--out FILE]     what HUBP0 actually scans out (full WDDM table)
```

`screenshot` writes to `P:\BC-250\scratch\screens\<timestamp>.<ext>` unless `--out` says otherwise, creates
the folder, and prints the path, the pixel size and the file size. Half scale is the default because the
reader pays per image token; a 1920x1200 screen at 0.5 is 960x600, about 750 kB as PNG and 30 kB as JPEG 70.

`scanout` runs entirely outside the overlay's HTTP API (it shells `bc250kmd_cli fbdump` on the target through
`target.py`, then pulls and re-encodes locally), so it needs no `mon.py` command of the overlay's at all; it
still logs nothing of its own on the overlay side, unlike `screenshot`, since the escape it drives already logs
each band (`GuardLog`, `dcn.c`). It writes to the same `scratch\screens` folder, `-scanout.<ext>` suffixed, and
deletes the full-resolution intermediate BMP it pulls once the downscaled picture is written.

## Not there yet

- `scanout` supports `png` (its own minimal encoder) and `bmp` (no re-encoding) only - no `jpg`, which would
  need a real JPEG encoder; and HUBP0 only, the pipe the firmware already scans out on.

- No authentication on the API; acceptable only because it is loopback-bound behind SSH.
- `/screenshot/window` asks the window to draw itself (`PrintWindow` with `PW_RENDERFULLCONTENT`). A window
  that refuses, or answers with one flat colour, is copied from the screen region it occupies instead, and
  then whatever lies on top of it is in the picture. The log line says which of the two was used.
- A minimized window has nothing to copy from the screen, so the fallback gives a blank image.
- `overlay=0` uses `WDA_EXCLUDEFROMCAPTURE`, which needs Windows 10 2004 or later. It leaves the window on
  the monitor and only hides it from the capture, so the owner sees no flicker; if Windows refuses, the
  capture still happens with the overlay in it and the log line says `overlay could not be hidden`.
- Only the primary screen; a second monitor would need the endpoint to take a display index.
- A capture runs on the API's single thread, so it blocks other requests for a few hundred milliseconds.
