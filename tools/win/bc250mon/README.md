# bc250mon - monitor and overlay on the BC-250's own screen

A window the owner can see on the lab machine while experiments are driven remotely: what is being done
right now, temperature and clocks, results, a log, and a brake. One .NET Framework 4.8 executable (part of
Windows, nothing to install), started at logon in the interactive session by a scheduled task, elevated
because `bc250rd.sys` is admin-only.

## Architecture

```
 providers (threads)            remote agent over SSH            owner at the machine
 GpuProvider, SystemProvider    mon.py -> HTTP 127.0.0.1:2250    hotkeys, buttons
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

## The brake

`Ctrl+Alt+S` or the red button sets the STOP flag: a banner on the overlay, `GET /flags` returns
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
| `Ctrl+Alt+B` | Toggle interactive mode: the window accepts the mouse and shows the action buttons. Otherwise it is click-through and never takes focus |
| `Ctrl+Alt+H` | Hide / show |
| `Ctrl+Alt+S` | STOP |

Buttons today: STOP, clear stop, 1000 MHz / 820 mV, stock 1500 MHz, hide, back to click-through.
Temperature colours: green below 85 C, amber from 85 C, red from 92 C (Tctl).

## Build and deploy

```powershell
pwsh tools\win\bc250mon\build.ps1 -Out P:\BC-250\scratch\build\bc250mon
```

Copy `bc250mon.exe` to `C:\BC250\mon\` and register a task "at logon of the lab user, interactive, highest
privileges" that runs it.

## mon.py

Every call goes over SSH, so the JSON body travels base64-encoded and images come back base64 on one line.

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
```

`screenshot` writes to `P:\BC-250\scratch\screens\<timestamp>.<ext>` unless `--out` says otherwise, creates
the folder, and prints the path, the pixel size and the file size. Half scale is the default because the
reader pays per image token; a 1920x1200 screen at 0.5 is 960x600, about 750 kB as PNG and 30 kB as JPEG 70.

## Not there yet

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
