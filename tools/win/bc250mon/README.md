# bc250mon - monitor and overlay on the BC-250's own screen

A window the owner can see on the lab machine while experiments are driven remotely: what is being done
right now, temperature and clocks, results, a log, and a brake. One .NET Framework 4.8 executable (part of
Windows, nothing to install), started at logon in the interactive session by a scheduled task, elevated
because `bc250rd.sys` is admin-only.

## Architecture

```
 providers (threads)                  remote agent over SSH      owner at the machine
 GpuProvider, SystemProvider,         mon.py -> HTTP            hotkeys, buttons
 KmdProvider, KmdInfoProvider,        127.0.0.1:2250
 TelemetryProvider
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

**Confirming a start** (ADR 0006 point 3). Full WDDM uses a typed KMD
snapshot through `bc250control.dll`. The monitor requires 60 seconds of fresh,
advancing completed-primary witnesses for one device-start generation and
visibility epoch. Missing samples, gaps over 15 seconds, stale progress or an
identity change restart observation. The kernel then checks the named live
identity and readiness before performing a checked durable registry reset.

Display-only retains its stage61/60-second policy only when a valid typed reply
positively identifies that mode. An old DLL, unsupported ABI or failed query
never selects a fallback confirmation path. Explicit human confirmation also
checks the native registry flush result. These checks certify startup progress,
not correct pixels, GPU rendering or complete M9 acceptance.

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

## Active graphics pipeline

`GraphicsPipelineProvider.cs` samples every 5 seconds. It inspects the DLL actually
loaded by DWM, hashes the module file, and looks up the exact build in
`graphics-modules.json` in the monitor data directory. Copy the supplied manifest
alongside the executable when deploying. Add a new hash only after identifying
the renderer and shader execution path of that binary; a build with selectable
backends also needs runtime backend evidence before assigning a renderer label.
Unknown hashes are displayed as unknown, without a CPU/GPU claim.

The panel shows DWM PID, UMD module and build, renderer, CPU/GPU draw execution,
shader interpreter/JIT, and the live KMD version/table. Driver `log summary`
provides hardware flip counts, enabled presentation paths, compute fence counts
and SDMA paging counts. These counts are cumulative for the device session,
not utilization percentages or evidence of work during the latest sample.
Enabled CPU Blt is shown separately from its actual count. Full WDDM and DCN
scanout do not imply GPU execution of DWM draws.

The sampled time makes the observation's age visible. The existing `kmdinfo`
panel remains available through the API; the overlay omits that duplicate panel
when the pipeline panel exists. An overlay update needs only an overlay task
restart, not a Windows or DWM restart.

## Active 3D app

`GraphicsApiProvider.cs` samples every 3 seconds which graphics API a windowed application in the
overlay's session started and which driver path serves it. The evidence is the set of image files mapped
into that process, not the registry or the router configuration. Candidates are the foreground window's
process and the owners of other visible, uncloaked windows of at least 200x150 (at most 8 per poll);
shells, launchers and our own tools are excluded by name. The panel shows at most 3 applications: the
foreground one first, then the ones on our GPU path.

A green row needs the whole user-mode stack of one render path: the shell the runtime opened, the engine
that shell loads and the ICD the engine draws with. The module names are the ones the release installs
(`tools/release/release-sources.json`), and the x86 payload uses the same names minus D3D12 and zink, which
it does not have (`tools/release/installer/common.ps1`), so 32-bit processes are covered by the same table.

| Loaded | Row |
|---|---|
| `d3d12.dll` / `d3d12core.dll` + `amdgpu_wddm_d3d12.dll` + `amdgpu_wddm_vkd3d.dll` + `amdgpu_wddm_radv.dll` | `D3D12  GPU (amdgpu-wddm vkd3d + RADV)` (green) |
| `d3d11.dll` / `d3d10*.dll` + `amdgpu_wddm_d3d11.dll` + `amdgpu_wddm_dxvk.dll` + `amdgpu_wddm_radv.dll` | `D3D11  GPU (amdgpu-wddm DXVK + RADV)` (green) |
| ... shell, or shell and engine, without the rest | `our D3D11 shell loaded, no engine yet` / `... no ICD yet` |
| ... + `bc250d3d_zink.dll` + `amdgpu_wddm_radv.dll` / `bc250d3d.dll` | `GPU (zink, desktop route)` (green) / `CPU (llvmpipe, app route)` (amber) |
| ... + `bc250d3d_router.dll` only | `router loaded, no backend yet` |
| `d3d10warp.dll` | `CPU (WARP)` (amber) |
| `d3d9.dll` + `d3d9on12.dll` / `bc250umd.dll` | `via D3D9On12 (D3D12 path)` / `stub UMD, no D3D9 renderer` |
| `vulkan-1.dll` + `vulkan_radeon.dll` | `Vulkan  GPU (RADV ICD)` (green) |
| a Direct3D runtime or `dxgi.dll` mapped from outside `\Windows\` | `replacement DLL next to the app, not the system runtime` (amber) |
| `d3d12.dll` in a 32-bit process | `no x86 D3D12 in this package` (amber) |

An API runtime with none of our modules gives `none of our UMDs loaded`: many D3D12 games load `d3d11.dll`
without drawing with it, so no claim is made, and another adapter's UMD is not ours to name.
`amdgpu_wddm_radv.dll` is the ICD our own shells load by name; the Vulkan row ignores it, because only the
registered `vulkan_radeon.dll` proves the Vulkan loader chose RADV.

A shell without its engine or ICD is a device still being created, and the row is grey for the first 10
seconds. After that it is a failed `LoadLibrary` and the row turns amber with `engine never loaded (load
failed)`: D3D11 usually falls back to WARP in that case, which the WARP row reports, but D3D12 has no
implicit fallback, so the shell stays mapped alone for the life of the process and nothing else would say
so. `d3d10warp.dll` is shared by D3D11 and D3D12; when both runtimes are loaded the WARP row cannot say
which one uses it.

`bc250d3d.dll` is the CPU UMD of both router decisions (`driver/umd/router/router-policy.h`). `dwm` is
excluded from this panel, so a process that shows it took the application decision (`AppRouter` `Mode`,
`Deny`), which is the knob to change; a `HostedClients` entry on the desktop CPU route maps the same two
files and cannot be told apart from the modules alone.

An OpenGL row appears whenever `opengl32.dll` is mapped and no row claims one of our GPU paths: most GL
engines map `d3d11.dll` or `dxgi.dll` for the display, so the presence of a Direct3D runtime must not hide
the API the application actually draws with.

A `d3d11.dll`, `d3d12.dll`, `d3d9.dll`, `d3d10*.dll` or `dxgi.dll` mapped from outside the Windows directory
is a translation layer placed next to the application. The application then does not go through the installed
Windows driver (it is not native in the sense of the project's acceptance rule), so the row reports the
replacement instead of a driver path, whatever else is mapped in the process. Paths come from
`GetMappedFileName`, so the test is on the device path, not on a DOS path the process could change.

A process the overlay cannot open for reading shows `API unknown (access)`. A change of the foreground
application's paths is written to the log once.

Cost: no KMD escape, no remote thread and no loader lock in the target. `EnumProcessModulesEx` reads the
loader list with `ReadProcessMemory` while the target runs, and the names (`GetMappedFileName`, one query per
module) are read again only when the set of module handles changes or the cached list is 30 s old (measured
here: 0.25 ms for the handle list and 2.1 ms for a full name sweep of a 529-module process).

The scan has two off switches in the data directory, like the pipeline panel's `graphics-summary.pause`:

- `graphics-api.pause` - the next poll opens no process at all and the panel keeps the last rows with
  `paused; sample HH:mm:ss`. Use it in a measured session, so this provider's cost cannot be mistaken for
  a driver change.
- `graphics-api.skip` - process base names, one per line (`#` comments allowed), that are never opened.
  The name is read through a query-limited-information handle first, so a skipped or excluded process never
  sees the `PROCESS_VM_READ` handle that an anti-tamper check may dislike.
`test-graphics-api.ps1` checks classification, side-loading, 32-bit answers, ordering and rows on fake
module lists; `build.ps1` runs it. The panel is only a report of which modules are mapped: it does not say
that the application draws anything, and `GraphicsPipelineProvider` above still answers the same question
for DWM.

## Cached Vulkan inventory

`VulkanInventoryProvider.cs` reads `vulkan-inventory.json` from the monitor data
folder every five seconds. A relative `BC250MON_VULKAN_INVENTORY` override is
resolved against that folder; an absolute override is also accepted. The provider
never launches Vulkan tools, creates a Vulkan instance, or opens a GPU device.
The collector runs separately on an explicit request. Publish the finished JSON
by replacing the previous file, and retain the full text report beside it.

The panel shows capture UTC and age, device/API/driver, the loader-observed ICD
library (or only the requested manifest when unverified), extension/layer counts,
format counts, and selected timeline semaphore, buffer device address, FP16 and
sparse binding features. `?` means not captured, not unsupported. These are
queried Vulkan capabilities, separate from the active DWM renderer shown in the
pipeline panel. Enumeration does not validate rendering or inference results.

Schema 1 examples are in `test/fixtures/vulkan-*.json` and are synthetic. Status
is `ok`, `partial`, `error` or `unsupported`. Partial captures display a warning
alongside known fields; truncated format statistics remain null. Error captures
do not reuse older capability rows. `CapturedUtc` is required and UTC; caches
older than 24 hours or captured with a different known KMD version are marked.
`IcdPath` is the requested manifest and `IcdSha256` hashes that manifest.
`IcdLibraryPath`/`IcdLibrarySha256` identify the actual library when observed;
`IcdVerified` refers to that loader observation, described by `LoaderEvidence`,
not merely a requested environment override. The JSON also retains collector,
tool hash, captured KMD version, layer names and full report path.

The overlay moves whole panels into additional columns when they exceed the
monitor working area's height. Font size, click-through and hotkeys are unchanged.
`test-vulkan-inventory.ps1 -Out <build directory>` checks actual provider parsing,
cache replacement and panel geometry without opening a window or querying a GPU;
`build.ps1` runs this check. The source fixtures are never deployed as live data.

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

## GPU telemetry line

One line under the title, `GPU  Tctl 67.5 C  load 12 %  GFX 1000 MHz  VRAM 1234/2048 MB`, from
`TelemetryProvider.cs`. Every source is read-only. The DPM snapshot and the segment statistics are answered from
memory the KMD or dxgkrnl already holds; the clock fallback is the same typed read the SoC panel makes anyway:

| Value | Source |
|---|---|
| Tctl, GFX clock | the DPM governor's published snapshot (`BC250_ESCAPE_RUN_DPM`, op READ, KMD 0.7.175 and later) through `Bc250Dpm` in `bc250control.dll`. When the snapshot does not carry them (governor stopped, no flag), the clock escape the SoC panel already uses stands in; the JSON says which in `temperatureSource` / `clockSource` |
| load | the mean of the governor's `BusyAvgPermille` over the 2 s window, only when the snapshot carries `BC250_DPM_FLAG_HW_BUSY` (GRBM_STATUS busy share, KMD 0.7.177). Older KMDs publish a submit-to-fence share that read 7 % while ETW saw 91 %, so it is not shown: the line says `n/a` and the note names the KMD version |
| VRAM | `D3DKMTQueryStatistics` per segment through `Bc250VideoMemory`: used = resident bytes of the non-aperture segments (what Task Manager calls dedicated usage), total = their commit limits, or the adapter's dedicated video memory when dxgkrnl reports no limit. The aperture (system memory the GPU maps) is kept apart in the JSON |

Samples are taken every 250 ms and published every 2 s. A missing value is `n/a`, never a guess, and the
reason goes to `note` (logged once when it changes).

**Cost.** DWM composes this desktop on the CPU: a 250 ms full-window repaint of a small window cost DWM 28 %
of the machine during a game (trials 139/140). So the telemetry never causes a frame of its own: the header
clock repaints only its own rectangle once a second, and the telemetry line is invalidated in that same tick,
only when its text changed. A full repaint happens only when a panel, the log or the status changes. The
monitor's own work per second is about 15 us managed (8 samples, one publish, one line paint; the telemetry
host test measures it) plus four DPM escapes and, every 2 s, one statistics digest, each an adapter open, the
call and a close of a few microseconds (the adapter path is cached; looking it up walks SetupAPI for ~0.8 ms and is
retried at most every 2 s while the adapter is absent).

## The brake

`Ctrl+Alt+F12` or the red button sets the STOP flag: a banner on the overlay, `GET /flags` returns
`{"stop": true}` and the file `C:\BC250\mon\STOP` exists. Every test script that can run for more than a
few seconds has to poll one of the two and end. `mon.py stop?` does it from the PC (exit code 1).

## API

`http://127.0.0.1:2250`, loopback only, no authentication, reached through SSH.

| Endpoint | What it does |
|---|---|
| `GET /state` | everything the overlay shows: status, panels, log, stop flag, telemetry |
| `GET /telemetry` | the telemetry line as JSON: `available`, `temperatureC`, `loadPercent`, `gfxMHz`, `vramUsedMB`, `vramTotalMB`, their sources, `ageSeconds`, `note` (`null` values are n/a) |
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
pwsh tools\win\bc250mon\build.ps1 -Out $env:BC250_ROOT\scratch\build\bc250mon -ControlDll $env:BC250_ROOT\scratch\build\bc250kmd_cli\bc250control.dll
```

The build runs the Python layout tests (`test_stages.py`, `test_telemetry.py`: `DpmSnapshot` against the KMD's
`BC250_ESCAPE_DPM`, `VideoMemorySnapshot` against the control DLL) and the host tests `test-vulkan-inventory`,
`test-start-confirmation`, `test-graphics-summary`, `test-telemetry` and `test-graphics-api` before it compiles. The telemetry line
needs a `bc250control.dll` that exports `Bc250Dpm` and `Bc250VideoMemory`; the two files are deployed together.

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
mon.py telemetry [--format text|json] tctl_c=67.5 load_pct=n/a gfx_mhz=1000 vram_used_mb=1234 vram_total_mb=2048 age_s=0.4
                                      (a "# note" line follows when a value is n/a; exit code 1 without a sample)
mon.py windows                        handle, process, geometry and title of every titled window
mon.py screenshot [--scale 0.5] [--format png|jpg] [--quality 80] [--overlay 0|1]
                  [--window TITLE | --handle 0x...] [--out FILE]
mon.py scanout [--scale 0.5] [--format png|bmp] [--out FILE]     what HUBP0 actually scans out (full WDDM table)
```

`screenshot` writes to `<BC250_ROOT>\scratch\screens\<timestamp>.<ext>` (`BC250_ROOT` is the workspace
root, by default the parent directory of this repository) unless `--out` says otherwise, creates
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

## Native clock client migration (M440)

The current source uses the typed KMD client in bc250control.dll, built by
`tools/win/bc250kmd_cli/build.ps1`; pass that DLL to this build with `-ControlDll`
and keep it beside bc250mon.exe. The earlier bc250rd descriptions above document
the deployed legacy version, not this new source. The SoC panel takes paired
clock/VID/temperature samples in-process and labels the KMD backend. Clock changes
are one KMD transaction; the unforced-voltage stock button has been removed.
No raw mailbox IOCTL or automatic fallback exists. Do not deploy before completing
native KMD owner activation and the legacy-writer handover.
