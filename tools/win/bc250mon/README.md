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
- **Log**: every event is appended and flushed to `C:\BC250\mon\log\<date>.log` first, so the file is
  useful after a hard hang.

## The brake

`Ctrl+Alt+S` or the red button sets the STOP flag: a banner on the overlay, `GET /flags` returns
`{"stop": true}` and the file `C:\BC250\mon\STOP` exists. Every test script that can run for more than a
few seconds has to poll one of the two and end. `mon.py stop?` does it from the PC (exit code 1).

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
privileges" that runs it. `mon.py` on the PC: `state`, `status`, `log`, `panel`, `unpanel`, `action`, `stop?`.

## Not there yet

- No screenshot endpoint: the remote side cannot see what the owner sees.
- No authentication on the API; acceptable only because it is loopback-bound behind SSH.
- The window's look was not verified by the author, who has no view of the BC-250's screen.
