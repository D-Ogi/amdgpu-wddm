# bc250kmd_cli - the user-mode end of the M3 miniport

A small native tool that talks to `driver/kmd` (bc250kmd) from user mode. It exists for two reasons: to settle
one of the open questions of ADR 0006 - **does `D3DKMTEscape` reach a display-only miniport at all?** - and to
read the driver's breadcrumbs on a machine with no kernel debugger.

```
bc250kmd_cli info [hardware-id]   the escape, to the adapter with that PnP hardware id
                                  (default PCI\VEN_1002&DEV_13FE)
bc250kmd_cli list                 every graphics adapter, with hardware id, interface path, handle and LUID
bc250kmd_cli stages               LastStage / StageHistory / UnconfirmedStarts, with names
bc250kmd_cli confirm              explicit human confirmation, checked durable zero (elevated)
bc250kmd_cli health read          cached device-start identity and completed-presentation witness
bc250kmd_cli health confirm <generation> <epoch>   checked confirmation of that observed start
bc250kmd_cli read <offset>        one register through the driver (hex BAR5 byte offset from regcalc; ADR 0007)
bc250kmd_cli read <name>          the same for a register of the named read list (gen_regs.py EXTRA_READS),
                                  as NAME, mmNAME or IP.NAME; the tool prints the offset it resolved
bc250kmd_cli write <offset> <v>   one register on the driver's write table, read back
bc250kmd_cli memory               where the framebuffer, BAR0 and the VRAM carve-out are (E08)
bc250kmd_cli vread <phys|bar0> <offset>          one 32-bit word of VRAM through either path
bc250kmd_cli vwrite <phys|bar0> <offset> <v>     same, inside the driver's test page only
bc250kmd_cli vcompare <offset> <count>           the same words through both paths, exit code 4 if they differ
bc250kmd_cli gart plan|enable|restore            the M4 sequence: list what it would write, run it, give the firmware's state back (E09)
bc250kmd_cli psp plan|load|unload                firmware through the PSP: list registers and commands, run them, DESTROY_TMR and ring stop (E10)
bc250kmd_cli gfx plan N|run N|fini|state         RLC, CP, KIQ, queues, ring tests, SDMA in stages 1..7: list the writes, run up to stage N, halt and free (E11)
bc250kmd_cli dcn                                 read-only dump of the DCN 2.0.1 ("DMU") display registers, decoded HUBP0/OTG0 summary against the Linux reference (ADR 0011 point 3)
bc250kmd_cli dcnflip <phys hex> [fill <argb hex>] | dcnflip restore
                                                  one gated flip on HUBP0/OTG0 (ADR 0011 point 3 step 2)
bc250kmd_cli sdmacopy [bytes]                     SDMA copy/fill positive control, read back and compared by the CPU (ADR 0013)
bc250kmd_cli fbdump <file.bmp>                    the scanned-out surface (HUBP0), assembled from several read-only bands into a BMP
bc250kmd_cli sdmaib [bytes]                       VMID0 indirect SDMA copy/fill control
bc250kmd_cli clock read | clock set <MHz> <mV>    one KMD clock sample, or the complete clock policy with readback
bc250kmd_cli log [from]                           the driver's log ring, 64 lines per escape, without the adapter lock (0.7.184)
bc250kmd_cli log summary [from | only]            the WDDM counters written into the ring first (one HardwareAccess escape),
                                                  then the ring; only: just the summary's own lines, the form for a poller
bc250kmd_cli telemetry [count [interval ms]]      DPM snapshot and segment statistics: what the monitor's GPU line shows
bc250kmd_cli vram [hardware-id]                   dxgkrnl's segment statistics of any adapter, one line per segment
bc250kmd_cli dpm [count [interval ms]]            the KMD DPM governor's state, with the thresholds and floor in force (docs/design/dpm.md)
                                                  the header also carries the idle state: its point, window and counters (0.7.207)
bc250kmd_cli dpm tune <up> <target> <down> [hold_ms] | dpm tune reset
                                                  governor thresholds (permille) at run time, elevated, not persisted (0.7.185)
bc250kmd_cli dpm tune thermal <hot_ms> <soft_mC|off> <soft_ms>
                                                  the thermal cap's hot step and soft release below 87 C (0.7.197, ABI 2)
bc250kmd_cli dpm floor <MHz|off>                  a runtime clock floor; thermal, critical and max-setting limits still win
bc250kmd_cli dpm confirm                          clears a pending DPM start (elevated)
bc250kmd_cli interop                              the GPU DWM interop switches this start runs with, and why
bc250kmd_cli dpaudio [state]                      DP audio: the step 0 check table, the decision a start would take
                                                  now, and the record of the last start (driver/kmd/dpaudio.c)
bc250kmd_cli journal [from]                       the paging journal (docs/design/paging-journal.md)
bc250kmd_cli journal follow SECONDS [MS]          one process, one held adapter, new records every MS (default 1000)
```

The last line of `log` counts its escapes: `escapes: N without adapter synchronization, M with HardwareAccess`.
A `log summary` against 0.7.184.1 or later takes exactly one HardwareAccess escape, the summary itself; every
page after it goes without adapter synchronization. Each HardwareAccess escape is a Level Two call, so dxgkrnl
idles the GPU for it and a running game waits: BD-054 was the overlay polling a CLI from before 0.7.184.1, whose
`log summary` took 16 of them every 5 s (280-420 ms per poll). The overlay reads that line and warns when
M is above 1, or when the line is missing.

`log_test.c` holds that contract against a fake driver and `build.ps1` runs it before it compiles the tool.
`mutate-log-test.ps1` is its mutation control: the CLI of KMD 196, paging through `SendEscape`, no sentinel
fixup and a changed count line must each fail the test. Recorded run of both:
[evidence/windows/2026-10-03-BD-054-log-poll-controls](../../../evidence/windows/2026-10-03-BD-054-log-poll-controls/RESULT.md).

Exit codes: `0` done, `1` the operation failed (the failing call and its NTSTATUS are printed), `2` bad usage
or the driver is not installed, `3` the start budget is used up (`stages`) or the driver refused the command
(gate closed, offset outside its tables or windows, caller not an administrator), `4` (`vcompare`) the paths differ.

## Startup health (ABI 1)

`health read` returns a synchronized software snapshot without hardware access
or GPU scheduler idling. Flags are full WDDM (1), engines ready (2), visible
active output (4), and this generation/epoch durably confirmed (8). Completion
counts refer to distinct successfully programmed and completed primary flips;
repeated vblanks and old-buffer fallback reports do not count as progress.

The monitor observes at least 60 seconds of fresh progress before issuing
`health confirm`. A direct command is a diagnostic endpoint, not a replacement
for that observation interval. Confirmation checks the live generation/epoch,
readiness, age and native registry flush result. The separate `confirm` command
records an explicit human decision and does not claim automatic health proof.
See [startup confirmation](../../../docs/design/wddm-start-confirmation.md).

## How the adapter is found

`D3DKMTOpenAdapterFromGdiDisplayName` names a *display*, not a device, so with more than one adapter, or while
ours is installed but not driving the screen, it can open the wrong one. `D3DKMTEnumAdapters2` hands out
handles with no way back to the PnP device. So the tool enumerates device interfaces with SetupAPI, matches
`SPDRP_HARDWAREID` against the id it was given, and opens that device's interface path with
`D3DKMTOpenAdapterFromDeviceName`.

The interface class must be **`GUID_DISPLAY_DEVICE_ARRIVAL`**, which dxgkrnl registers for every graphics
device, display-only ones included. Measured on the development PC on 2026-09-21: paths of
`GUID_DEVINTERFACE_DISPLAY_ADAPTER`, the obvious-looking name, are refused by
`D3DKMTOpenAdapterFromDeviceName` with `STATUS_INVALID_PARAMETER` (0xC000000D) for every adapter, while the
`GUID_DISPLAY_DEVICE_ARRIVAL` path of the same device opens.

## The escape, and what a failure means

`info` sends `BC250_ESCAPE_GET_INFO` as `D3DKMT_ESCAPE_DRIVERPRIVATE` with the `BC250_ESCAPE` structure of
`driver/kmd/bc250kmd_escape.h` (included by relative path, never copied). Every step prints its own NTSTATUS,
because the failure is a measurement too. On success it prints version, last stage, the mode (display-only vs
full WDDM table), presents (the display-only path's own counter) plus blits/flips (the full table's, both 0
outside it - `wddm.c`'s `WddmCounters`, `BC250_ESCAPE.Reserved[0]`/`[1]`), and which gates are open (`Flags`).
`tools/win/bc250mon`'s overlay runs this every few seconds for its "bc250kmd live" panel; see its README.

- against a driver that has no such escape, the call fails and the hex status is the answer;
- against bc250kmd, a failure means dxgkrnl does not route escapes to a display-only miniport, and M4 needs
  another control channel (ADR 0006, open questions).

Measured on unit A, 2026-09-21, same request every time, evidence
`evidence/windows/2026-09-21-escape-probe/`:

| Driver on the device | Hardware id | `D3DKMTEscape` |
|---|---|---|
| Microsoft Basic Display Adapter | `PCI\VEN_1002&DEV_13FE` | `0xC000000D` STATUS_INVALID_PARAMETER |
| Microsoft KMDOD sample (E05, display-only) | `PCI\VEN_1002&DEV_13FE` | `0xC00000BB` STATUS_NOT_SUPPORTED |
| Microsoft Basic Render Driver | `ROOT\BasicRender` | `0xC00000BB` STATUS_NOT_SUPPORTED |

`D3DKMTOpenAdapterFromDeviceName` returned STATUS_SUCCESS in all three, so the adapter was reached and the
escape itself is what failed. None of these drivers has a private escape of ours to answer with, so this is
a baseline, not an answer: the question is settled by the same command against bc250kmd, which does fill
`DxgkDdiEscape`.

## fbdump: what the display controller is actually scanning out

Under the full WDDM table, `mon.py screenshot`'s GDI capture reads the CDD's surfaces and shows black (facts
M84) - it is not looking at what HUBP0 is scanning out. `fbdump` is `BC250_ESCAPE_RUN_FBDUMP`
(`driver/kmd/dcn.c`): read-only, no gate beyond `EnableMmio` (BAR5 mapped), the same condition `dcn` already
answers to. Each call reads `HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS[_HIGH]` and `..._SURFACE_PITCH` fresh (a
present between two calls of the same dump is a torn frame in the resulting BMP, not a driver bug - locking
OTG0 for the whole dump would make this the write escape it deliberately is not), maps `RowCount * Pitch` bytes
read-only (`MmMapIoSpaceEx(..., PAGE_READONLY | PAGE_NOCACHE)`) and copies them out. `BC250_FBDUMP_MAX_ROWS`
(64) rows per call keeps one escape's buffer at 512 KiB regardless of the true row count, well short of the
9 MB the whole 1920x1200 A8R8G8B8 surface would take in one call; `bc250kmd_cli fbdump` asks for that many rows
at a time (about 19 calls for the firmware's own mode) and assembles a bottom-up 32-bit `BI_RGB` BMP - no
channel swap needed, since A8R8G8B8's in-memory byte order (B, G, R, A) already matches BMP's.

The band's physical range must land inside the VRAM carve-out (`EnableVram`,
`Device->VramPhysical`/`VramLength` - the same range `dcn.c`'s `AddressAllowed` and `wddm.c`'s Blt validate
against) or inside the firmware's own framebuffer, which `display.c` maps at every device start regardless of
any gate - so a fresh boot that has never taken `EnableVram` can still be dumped.

`tools/win/bc250mon`'s `mon.py scanout` runs this on the target and pulls the BMP back; see its README.

## Stage names

`g_Stages` repeats `enum BC250_STAGE` from `driver/kmd/bc250kmd.h`. `test_stages.py` parses both and fails the
build if they drift apart (`build.ps1` runs it; the monitor keeps a third copy with its own test).

## Build

```powershell
pwsh tools\win\bc250kmd_cli\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Out $env:BC250_ROOT\scratch\build\bc250kmd_cli
python -m unittest discover -s tools/win/bc250kmd_cli
```

Headers and import libraries come from the SDK NuGet packages, the compiler from the installed Visual Studio:
the same flow as `tools\win\bc250rd\build.ps1`, without the driver and the signing. On the target it lives in
`C:\BC250\kmd\`.

## Typed clock client

`clock read` returns one paired KMD MHz/VID/temperature sample; `clock set MHz mV`
runs the complete serialized clock policy and verifies readback. The native
`bc250control.dll` built beside this CLI exports the same path to the monitor
and reader compatibility CLI. No raw SMU message passthrough or bc250rd fallback.
The KMD must have completed native-owner handover before these calls can succeed;
an offline owner returns DEVICE_NOT_READY. See `docs/design/startup-clock-ownership.md`.

## Telemetry exports

Two more `bc250control.dll` exports feed the monitor's GPU telemetry line (`tools/win/bc250mon/README.md`), and
`telemetry` / `vram` print the same data:

- `Bc250Dpm(BC250_ESCAPE_DPM*, 160)`: the DPM governor's published snapshot, `BC250_ESCAPE_RUN_DPM` op READ
  (KMD 0.7.175 and later), with `NoAdapterSynchronization` as the KMD requires and open to non-admin callers.
  A KMD without the command answers `STATUS_DEVICE_NOT_READY` or leaves the status unknown
  (`STATUS_NOT_SUPPORTED` here). The structure comes from `driver/kmd/bc250kmd_escape.h`; a guarded copy of KMD
  0.7.177's stays in the source for a header that predates the escape, and `tools/win/bc250mon/test_telemetry.py`
  checks it against the header.
- `Bc250VideoMemory(hardware-id or NULL, BC250_VIDEO_MEMORY*, 264)`: `D3DKMTQueryStatistics` per segment of
  the adapter (default the BC-250), summed into memory and aperture segments, with the adapter's
  `KMTQAITYPE_GETSEGMENTSIZE` dedicated size. Limits saturate: an aperture may report a commit limit of 2^64-1
  (measured on the development PC's RTX 4090, 2026-09-30).

Two more feed the control application (`tools/win/amdgpu_wddm_control/README.md`), on the same cached path and with
the same `NoAdapterSynchronization`-only flags:

- `Bc250Interop(BC250_ESCAPE_INTEROP*, 104)`: the GPU DWM interop decision of this start (`BC250_ESCAPE_RUN_INTEROP`
  op READ), open to every caller.
- `Bc250LogRead(from, BC250_ESCAPE_LOG*, 10812)`: one page of the log ring (`BC250_ESCAPE_GET_LOG`), answered without
  the adapter lock from 0.7.184.1. No `HardwareAccess` fallback and no `LOG_SUMMARY`: an older driver's refusal is
  returned as it is.

All of them open the adapter per call and close it again. The interface path is looked up once and cached; an absent
adapter is looked for again at most every 2 s, since the SetupAPI walk costs ~0.8 ms of CPU against ~6-9 us
for a cached call (development PC, `vram`). `test-telemetry.ps1`, run by `build.ps1`, tests both exports with
the D3DKMT calls replaced.
