# bc250kmd_cli - the user-mode end of the M3 miniport

A small native tool that talks to `driver/kmd` (bc250kmd) from user mode. It exists for two reasons: to settle
one of the open questions of ADR 0006 - **does `D3DKMTEscape` reach a display-only miniport at all?** - and to
read the driver's breadcrumbs on a machine with no kernel debugger.

```
bc250kmd_cli info [hardware-id]   the escape, to the adapter with that PnP hardware id
                                  (default PCI\VEN_1002&DEV_13FE)
bc250kmd_cli list                 every graphics adapter, with hardware id, interface path, handle and LUID
bc250kmd_cli board-caps           board capabilities and limits from the driver, without hardware access
bc250kmd_cli oc                   the overclock tier of this start, its reason, the stored tier and board, and
                                  whether the stored board is this board
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
                                                  then the ring; only: just the counter block, beside the ring, the form
                                                  for a poller (0.7.216.24, docs/design/kmd-log-ring.md)
bc250kmd_cli telemetry [count [interval ms]]      DPM snapshot and segment statistics: what the monitor's GPU line shows
bc250kmd_cli vram [hardware-id]                   dxgkrnl's segment statistics of any adapter, one line per segment,
                                                  then the dedicated and shared system memory sizes (0.7.216.8)
bc250kmd_cli budget <pid | image.exe> [hardware-id]
                                                  one process on one adapter: budget, requested, usage and Demoted[] per
                                                  segment group, then its committed bytes per segment (0.7.216.8, elevated)
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
                                                  now, and the record of the last start with the stream state
                                                  (driver/kmd/dpaudio.c, ABI 2 from 0.7.216, ABI 1 before)
bc250kmd_cli journal [from]                       the paging journal (docs/design/paging-journal.md)
bc250kmd_cli journal follow SECONDS [MS]          one process, one held adapter, new records every MS (default 1000)
```

Use `board-caps` for the current fan, CPU and GPU limits.
Board setters require an admitted provider. An older driver without this query does not authorize setters.

The last line of `log` counts its escapes: `escapes: N without adapter synchronization, M with HardwareAccess`.
A `log summary` against 0.7.184.1 or later takes exactly one HardwareAccess escape, the summary itself; every
page after it goes without adapter synchronization. Each HardwareAccess escape is a Level Two call, so dxgkrnl
idles the GPU for it and a running game waits: BD-054 was the overlay polling a CLI from before 0.7.184.1, whose
`log summary` took 16 of them every 5 s (280-420 ms per poll). The overlay reads that line and warns when
M is above 1, or when the line is missing.

From KMD 0.7.216.24 `log summary only` costs the ring one line instead of about 320: the driver keeps the block
beside the ring and answers `SummaryFrom` as the sequence of the block's first line, which pages exactly like a
ring sequence (BD-097, `docs/design/kmd-log-ring.md`). `log summary` without a position still writes the block
into the ring, where a reader of the whole trail wants it. The tool asks for the sequence the driver answered
with, and it says one thing more: the driver holds one block at a time, so a summary that another caller takes
between two pages ends this read, and the tool prints that the block was replaced instead of printing a page of
each summary.

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

## Release and lab flavours

A release build (the default) has no raw diagnostic verbs: `read`, `write`, `memory`, `vread`, `vwrite`,
`vcompare`, `vtable`, `gart`, `psp`, `gfx`, `ih`, `fence`, `ib`, `dcn`, `dcnflip`, `sdmacopy`, `sdmaib` and
`fbdump`. A release CLI answers each of them with `unknown command` and exit code 2. Their escapes exist only in a
lab KMD, and a lab KMD answers them only with test signing on and `LabDiagnostics = 1` (`driver/kmd/lab_gate.c`,
BD-167). `build.ps1 -Lab` defines `AMDGPU_WDDM_LAB_FLAVOUR` and builds the CLI and `bc250control.dll` with these
verbs. `tools\win\amdgpu_wddm_control\build.ps1 -Lab` does the same for the copies in the control app.

Both flavours keep every form that a release install calls (`tools/release/cli-commands.json`). Both build scripts
run two gates in the flavour of the build:

- Before the CLI is compiled, `test-verbs.ps1` builds and runs `verbs_test.c`. The test gives every raw verb
  each argument shape that its handler accepts, against a fake adapter. In a lab build, each command line must reach
  its handler and send the escape command of that handler. In a release build, each one must be an unknown command,
  also in upper case, and must open no adapter. No real adapter is opened and no escape reaches a driver.
- After the CLI is compiled, `test-flavour.ps1` runs it once, without arguments. The usage text of each flavour must
  have the release forms. A lab CLI must name every raw verb, and a release CLI must name none of them. No raw verb
  runs against the PC that builds, because a CLI of the wrong flavour would act on it.

`test_lab_verbs.py` holds the verb lists of the dispatch, of `verbs_test.c` and of `test-flavour.ps1` to one
another. `quick.ps1` builds the lab flavour as the `driver-state-cli-lab-x64` check.

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

## Wave state

The wave snapshot reads registers in sequence. It does not stop waves or give an atomic context image.
Equal identity and status rechecks do not prove atomicity. A changed recheck marks the record as unstable.
Read raw values beside the named fields. Do not infer the cause of a hang from one counter alone.

Use an elevated administrator prompt:

```text
bc250kmd_cli waves
bc250kmd_cli waves saved
bc250kmd_cli waves --expect-idle
bc250kmd_cli waves --expect-pc 0xSTART 0xEND
```

`waves` takes a live snapshot through command 34, ABI 1.
It sets only `NoAdapterSynchronization`, with value 8.
The driver serializes the read without requesting GPU idle.
The scan holds `GartLock` for up to about 250 ms and delays node-0 submission.
It can cause a visible hitch in a game. Never poll it from an overlay or GUI.
The KMD owns the register lock. Run the known-spin control below to measure live delivery.
There is no fallback to other escape flags.

`waves saved` reads the last `Snapshot` binary value at `HangRecovery\Waves` through the common state client.
This child key grants access only to administrators and SYSTEM.
It does not access the GPU. The snapshot contains a header and exactly `WaveCount` records.
The tool checks the ABI, length, count, flags and selectors before it prints any record.
Missing or unreadable data fails the command. A failed capture never counts as an idle result.
Use the sequence, fence, VMID and timestamps to associate saved data with its recovery attempt.
The saved record is historical. The tool does not establish that it belongs to the latest recovery attempt.
A failed registry write can leave an older record. The tool prints this warning before the saved data.

Each record prints SE, SH, WGP, SIMD and wave slot selectors.
The tool does not invent a CU number from the WGP selector.
It prints all 16 raw registers, their named fields, PC, EXEC and the status/identity rechecks.
The field table comes from `gc_10_1_0_sh_mask.h` through `gen_wave_decode.py`.
Regenerate it after a source header change:

```text
python tools/win/bc250kmd_cli/gen_wave_decode.py
python tools/win/bc250kmd_cli/gen_wave_decode.py --check
```

`--expect-idle` requires a complete successful scan with zero valid waves and zero read errors.
`--expect-pc` takes hexadecimal GPU addresses and uses the interval `[START, END)`.
It requires a complete successful scan and at least one stable wave inside that interval.
Other desktop waves can remain outside the interval. An empty snapshot fails this positive control.
Both checks return 1 on failure. Invalid arguments return 2, and an unelevated caller gets 3.
A plain read also returns 1 for an incomplete capture, after it prints the available records.

`waves_test.c` checks fixed register values, damaged saved blobs, capture failures and the control checks.
It replaces all adapter, state and token calls. The build runs this host test before it builds the tool.
These tests do not establish a working hardware read. Run the idle and known-spin controls on the target first.

## State JSON interface

Scripts use the common state client through these commands:

```text
bc250kmd_cli state catalog
bc250kmd_cli state paths
bc250kmd_cli state snapshot
bc250kmd_cli state get NAME [PATH] [--override]
bc250kmd_cli state set NAME DWORD [PATH]
bc250kmd_cli state clear NAME [PATH]
```

`PATH` is a relative catalog path, such as `StartRecord`. Omit it for a root value.
Unknown names and paths fail before the tool opens the store. `DWORD` accepts decimal or `0x` hexadecimal.
The common store checks access, type and range. A write succeeds only after the checked persistence flush.
`clear` removes an override or clears a record whose catalog access permits that operation.
`paths` returns the OS service log directory as `logs`. It does not create a directory or open a GPU.
On failure, `logs` is empty and `error` reports the Windows error.

Each command prints one JSON object with `abi: 1`, `command` and a Win32 `error` number.
`catalog` returns `items` with each value's path, name, kind, type, size limit and access fields.
It also returns the compiled `schema` and `catalog_id` for comparison with the package catalog.
It does not open the store. `snapshot` adds `override` and `effective` objects for each setting.
For records, it adds a `record` object. Each read has `status`, `os_error`, `error`, `source`,
`type`, `bytes` and `value`. `get` returns the read object as `result`.
It reads the effective setting unless `--override` is present.

Sources are `none`, `override`, `factory` and `code`. Defaults apply only to an absent override.
Errors such as access denial, invalid data or an incomplete store remain errors.
DWORD values are JSON numbers. QWORD values are decimal strings to preserve all bits.
Strings use JSON escapes, string lists use arrays, and binary values use hexadecimal strings.
Failed reads have a null value. A snapshot can contain failed or absent reads while the command succeeds.
Scripts must check the status of every value they use.

Exit codes are 0 for success, 1 for a failed operation, 2 for invalid arguments, and 3 for an absent `get` value.
Only Win32 error 1060 identifies an uninstalled service. A missing store is an error, not an uninstalled service.
The state commands, `stages`, `confirm` and `waves saved` do not open a GPU adapter.
