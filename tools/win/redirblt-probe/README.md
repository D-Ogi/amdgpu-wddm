# redirblt-probe - the DWM redirected-blt handshake from a non-runtime client

One user-mode tool for ADR 0018 Path B after E45, E46 and DWM035 (facts M598, M601, M677): the token-less
`D3DKMTPresent(Blt)` of a linear VRAM allocation into a window is refused at admission with
`STATUS_GRAPHICS_VIDPN_SOURCE_IN_USE`, and the KMD's interop cap plus a hosted GPU DWM did not change that. The
documented producer of a *redirected* windowed Blt is a handshake the OpenGL runtime performs for an ICD
(Windows 7 documentation, `ref\win32-docs`, `dwmdxgetwindowsharedsurface.md`): `DwmDxGetWindowSharedSurface`
(dwmapi ordinal 100) with `DWM_REDIRECTION_FLAG_SUPPORT_PRESENT_TO_GDI_SURFACE`, then `D3DKMTPresent` with
`Flags.RedirectedBlt` and a `D3DKMT_PM_REDIRECTED_BLT` token carrying the update id. This tool performs the
handshake itself, opens the shared surface on our device, and presents once per variant. One bounded run then
says which of these holds on the target's build: the ordinal is gone, the DWM declines the window, the handle
does not open on our adapter, or dxgkrnl admits or refuses each variant.

The background and the eight conditions under which a refusal is a finding are in the local research note
`scratch\pdev-research\redirected-blt\REPORT.md` (workspace, outside the repository) and message 127 of the
agent discussion. Nothing here is a lab claim; the transcript of a run is.

## What a run does

| Step | Calls | What the transcript line says |
|---|---|---|
| 0 | `LoadLibraryExW(dwmapi)`, `GetProcAddress` ordinals 100/101, `DwmIsCompositionEnabled`, session ids | `PROBE ... ordinal100=1 ordinal101=1 composition=1 session=N console=N` |
| window | `CreateWindowExW` 641x479 client, `ShowWindow`, square corners, no GDI paint (`WM_PAINT` validates only) | `WINDOW hwnd=... exstyle=...` with `WS_EX_NOREDIRECTIONBITMAP` and `WS_EX_LAYERED` asserted clear; `STAMP` rows with the exact UTC for the trace |
| adapter | `EnumAdapters2` (WARP is listed too), per adapter the registry strings, `KMTQAITYPE_ADAPTERTYPE` and `KMTQAITYPE_WDDM_1_2_CAPS` as dxgkrnl reports them (raw word plus decoded bits: software device, `SupportKernelModeCommandBuffer` = GDI hardware acceleration, `SupportSoftwareDeviceBitmaps`) | `ADAPTERTYPE 0x... render=1 ...`, `WDDM_1_2_CAPS 0x... kernel_mode_command_buffer=0 ...` |
| device | `OpenAdapterFromLuid`, `CreateDevice`, `CreatePagingQueue`, `CheckVidPnExclusiveOwnership(0)` (context only: SUCCESS means no EXCLUSIVE owner, not that a present will be admitted) | `ADAPTER luid=...` |
| source | `CreateAllocation2` with the KMD's 32-byte LB7A block (A8R8G8B8, 256-byte pitch), map, `MakeResident`, `Lock2` fill with the colour, `Unlock2` | `SOURCE halloc=... size=... pitch=...` |
| context | `CreateContextVirtual` node 0, affinity 1, `ClientHint VULKAN`, no flags: the fork's present context field for field | |
| 1 | ordinal 100 on a worker thread with a deadline (the WAIT flag is documented as 0, so a call may block a VSync); twice, to see whether the update id moves | `HANDSHAKE n=1 hr=0x00263005 fmt=87 handle=... update=...` |
| 1b | `GetSharedResourceAdapterLuid` (global handle, then NT handle: a query that also classifies the handle), then `QueryResourceInfo` and `OpenResource` (or the NT-handle forms), map and make resident | `SHARED kind=global luid=... ours=1`, `OPENED numalloc=1 halloc=... first16=...`, the LB7A block, `DESTINATION ready=1 from=<handle> update=<id>: ...` |
| 2 | one `D3DKMTPresent` per variant, a fresh handshake before each token-carrying one (a reopen when the handle changed, a `REBIND` line when only the update id did), stop at the first `STATUS_SUCCESS` | `PRESENT variant=A flags=0x000100C1 ...` then `RESULT variant=A status=0x... composition=...`; `SKIP variant=B: <reason>` when a prerequisite is missing |
| 3 | only with `--update` and only after an `S_OK` handshake (dedicated DX surface): ordinal 101 once. Never after `DWM_S_GDI_REDIRECTION_SURFACE`, whatever a variant returned | `UPDATE hr=...` |
| hold | `capture_ready rgb=... x= y= width= height= hold_ms=` as the colour control prints it, then the message pump | |

Variants, all with `hContext` = the present context, `hWindow`, `hSource`, `SrcRect = DstRect =` the client
rect, `SubRectCnt = 1`:

| Variant | `Flags` | `hDestination` | Token |
|---|---|---|---|
| V0 | `Blt, SrcRectValid, DstRectValid` (0xC1, the E45 form) | 0 | none |
| A | V0 + `RedirectedBlt` (0x100C1) | 0 | `Model 3`, `EventId` = update id, one dirty rect |
| B | as A | the opened shared surface | as A |
| C | as A | as B | as A + `hPhysicalSurface` = the raw shared handle |
| D | as B | as B | as B, on a second context with `ClientHint OPENGL` |

The token's two surface fields have no documented source; A and C bracket them. `EventId` = update id and the
client rect as the dirty region are inferences from the Windows 7 page and from Mesa's `gldrv.h` callback,
not documentation. A `STATUS_SUCCESS` alone confirms none of them: only the trace rows below and the colour
on the screen do.

Exit codes: 0 the sequence ran (a refusal is a result), 1 a step before the variants failed, 2 bad arguments,
3 ordinal 100 is absent, 4 the watchdog fired, 5 a handshake call did not return within its deadline (or the
wait failed): the process ends at once, without the ordinary teardown, because the worker still holds the
window, the dwmapi module and the probe block. On every other path teardown runs in reverse; NT handles the
LUID query classified as such are closed once each, global share handles and unclassified handles never.

The decisions (what an unresolved wait does, when ordinal 101 may be called, when a destination is ready, which
variants may run, when a fresh handshake keeps, rebinds or reopens the destination, which handles are closed, what `--handshake-only`
needs) are pure functions in `redirblt_policy.h`, pinned by `redirblt-policy-test.c`, which `build.ps1` runs
on the development PC before the probe is built. No window, no DWM, no D3DKMT in that test.

B, C and D run only with a destination that is ready: the surface's adapter is ours, the blob is the KMD's LB7A
block, the map and the residency succeeded, and it is bound to the handshake whose update id goes into the
token: opened from that handle, carrying that id. A later handshake that returns the same handle with a new id
rebinds the id to the opened surface (no reopen); a different handle closes and reopens. Anything else is a
`SKIP` line, never a present with `hDestination 0`.

## Options

| Option | Meaning |
|---|---|
| `--match <text>` / `--luid H:L` | adapter choice, as `kmtprobe` |
| `--size WxH` | client area, default 641x479 (odd, so its allocation stands out in the trace) |
| `--color <hex32>` | ARGB pixel of the source, default `FF0000FF` (blue, the colour control's oracle) |
| `--variants <list>` | subset of `V0,A,B,C,D`, in that order; `--all` runs every listed one |
| `--handshake-only` | window, adapter identity, handshake and (unless `--no-open`) the open: no source, no context, no present. The first discriminating run |
| `--gdi-paint` | fill the client area once with GDI before the handshake (the "does painting create the surface" control) |
| `--no-open` | diagnostic: do not open the handle; B, C and D are skipped. With `--handshake-only`, no device or paging queue is created either: window, adapter LUID, DWM answer, handle kind |
| `--update` | step 3, after an `S_OK` handshake only |
| `--flags <hex>` | `dwFlags` of ordinal 100 (default `10`, SUPPORT_PRESENT_TO_GDI_SURFACE; `0` asks for the historical dedicated-surface S_OK path, an exploratory variation) |
| `--pump <ms>`, `--hold <s>`, `--handshake-timeout <ms>`, `--timeout <s>` | pump after `ShowWindow` (500), hold after the variants (5), per-call deadline (2000), watchdog (90, raised to cover the rest) |

## Reading the trace

`decode-redirblt.ps1 -Etl <dxgkrnl.etl> -ProcessId <pid> [-Hwnd 0x...]` (development PC or target, inbox
manifest) prints, for the probe's process: the `Present` rows (184) with flags and status, the `Blit_Info` (166)
and `BlitCancel` (44) rows with the PresentMon reading of `bRedirectedPresent`, the present-history tokens by
model, and every `CddStandardAllocation` (287) with size and `GdiSurfaceType`. It writes one CSV next to the
trace. With the Kernel-Process provider in the session, pids are named after the latest `ProcessStart` before
the row (pids are reused within a trace).

The GPU-GDI route, from `PresentMonTraceConsumer.cpp`: `Blit_Info` with `bRedirectedPresent = 0` followed by a
token of model 3 (or 0). `bRedirectedPresent = 1` is the deprecated dedicated-surface route the OS emulates with
a readback. A `Present` row with `ReturnStatus 0xC01E0342` and no `Blit_Info` is the admission refusal of E45,
E46 and DWM035.

On the DWM035 trace (`scratch\g0-hosted\dwm035\gpu.etl`) the decoder reproduces facts M677 for pid 6084: one
Present row, flags 0xC1, status 0xC01E0342, no blit, no token; and it shows no `CddStandardAllocation` at all
after the DWM's start (ten rows at 14:12:11.7, all `TEXTURE_CPUVISIBLE`, sizes of the desktop's existing
windows), none of the probe window's size and none in the probe's process. Whether that window had a GDI
redirection surface on the GPU is therefore not shown by that trace; step 1 of this probe asks the DWM directly
(`DWM_S_GDI_REDIRECTION_SURFACE_BLT_VIA_GDI` means "system memory or another adapter").

## Build

```
pwsh tools\win\redirblt-probe\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Out $env:BC250_ROOT\scratch\build\redirblt-probe
```

`/W4 /WX`, x64, `/MT`, SDK NuGet headers and import libraries only (`gdi32`, `user32`, `dwmapi`, `version`).
The build compares the LB7A magic against `driver\kmd\gdi_private.h`, builds and runs the policy test, builds
the probe, runs `--help` and prints the artifact's SHA-256, which a runner pins. The development PC only ever
runs `--help` of the probe: the tool creates a window.

## Run, on the target

Interactive user session, hosted GPU DWM with interop 1 and a confirmed start (health flags 15), DxgKrnl and
Kernel-Process ETW running, the same gates as DWM035. A CPU-DWM run afterwards is the negative control
(expected `DWM_E_ADAPTER_NOT_FOUND` or `BLT_VIA_GDI`). The ordinal check alone (`--handshake-only` exits before
any present) can ride on any run.

## Hazards

- `D3DKMTPresent` with `RedirectedBlt` from a non-runtime client is untested on this build. The context carries
  no `DisableGpuTimeout`, as the fork's does; a refused present costs nothing, an admitted one runs the KMD's
  engine Blt into a surface the DWM samples.
- Ordinal 100 may block until a VSync. The worker thread has a deadline; a call that never returns is reported
  and the thread left alone, and the watchdog ends the process.
- The opened surface, if any, is mapped and made resident in our process for the length of the run.
