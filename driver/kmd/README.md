# driver/kmd: bc250kmd, the WDDM miniport

State, with the facts in [docs/facts.md](../../docs/facts.md):

- M3 reached on unit A (E06, M28, M29): the display-only driver installs, runs the firmware's display and
  answers escapes. M4 reached (E07 to E09, M33): GART and VM context 0 through AMD's hub code. M5 reached (E10,
  E11, M34 to M37): the PSP loads the GPU firmware from this driver, and RLC, KIQ, MEC, the queues and SDMA come
  up with every ring test passing. M6 reached: a compute dispatch on a kernel-owned MEC queue (M57, M58).
- M7 stage A, the inert full WDDM table, was run in E16 (M61 to M71). The full table has since grown into the
  one the lab runs: hardware submission from the user-mode drivers (M8, M139), the Vulkan first picture (M10,
  M476) and the bounded GPU desktop run (M723, M724).
- The lab currently runs KMD171 (`DriverVer` 0.7.171.1; the `0.7.N.x` versions are called "KMD N") on the CPU
  desktop path (M727). It was built from 75b8ab6f on a side branch; the INF on `main` still says 0.7.149.0.
- TDR recovery is not implemented: `DxgkDdiResetFromTimeout` fails explicitly instead of claiming a reset it
  did not perform (M179).

What it is (ADR 0006, ADR 0008): one binary for `PCI\VEN_1002&DEV_13FE` with two DDI tables. The display-only
table takes over the firmware's framebuffer through post-display ownership, offers exactly the mode the firmware
left and presents by CPU copy, with **no MMIO** and no interrupt. The full WDDM table (`wddm.c`) is behind the
`EnableFullWddm` gate and carries hardware submission, SDMA paging and Present. The bring-up code next to both
(ADR 0007) touches hardware only behind registry gates that the INF writes as 0. Written from the documented DDI;
no code from Microsoft's MS-PL sample.

| File | Contents |
|---|---|
| `entry.c` | `DriverEntry` and the `EnableFullWddm` gate: the display-only DDI table, or M7's full one |
| `wddm.c` | the full WDDM DDI table behind that gate. Started as M7 stage A (inert, see below); now hardware UMD submission, SDMA paging, Present and VSync. `DxgkDdiResetFromTimeout` fails explicitly (M179) |
| `wddm_allocation_identity.inc` | allocation lookup and publication under the lock `WddmFreeObject` uses, so an allocation address is never reused under a stale snapshot |
| `vidmm.c` | M7 stage B: VidMm's page tables behind `EnableGpuVa`; CPU_VIRTUAL initialization and the serialized pre-RUN bootstrap use CPU writes, while GPU_PHYSICAL updates use SDMA after the first engine RUN. |
| `paging_*.c`, `paging_*.h` | the paging planners the full table uses: windows, intervals, streams, permutations, page table shadow, aperture state, captures, the MC address conversion and the paging private data. Most have host tests in `test/` |
| `gfx_copy.c/.h`, `gfx_copy_fields.h`, `gfx-copy-provenance.md` | GFX10 CP `DMA_DATA` copy on the L2 path, with the packet fields taken unchanged from Mesa's generated RADV headers (provenance file) |
| `gfx_blt.c/.h`, `blit_plan.c/.h` | the GFX Blt: geometry planning in allocation offsets, then packet emission with a resumable cursor |
| `gfx_completion_queue.h`, `present_range.h` | a bounded completion queue independent of CP fetch progress; the check of a CPU-mapped VRAM range against every GPU page |
| `gfx_recovery.h` | the SDMA0 recovery admission at start-up (internal, not an escape) |
| `dcn.c`, `dcn_translate.c/.h` | ADR 0011: the read-only DCN 2.0.1 register dump, the gated flip, the scan-out dump (`fbdump`) and the flip address conversion (host-tested) |
| `display_timing.h`, `display_timing_snapshot.h` | read-only decoding of the timing the firmware left, after Linux's display code (provenance in the file) |
| `smu.c`, `smu.h` | the native SMU owner behind `EnableNativeSmu`: the fixed clock transaction only, after the legacy writer has handed over (`docs/design/startup-clock-ownership.md`) |
| `power.c` | adapter power coordination |
| `startup.c/.h`, `start_health.c/.h` | device-owned initialisation with phase bits, and the start-up health witness the monitor confirms (see below) |
| `umd_blob.c/.h` | the reader of the three private-data blobs a user-mode driver hands the KMD (`driver/contract/bc250_umd_submit.h`), host-tested |
| `umd_caps.c/.h`, `firmware_metadata.h` | the caps blob for `KMTQAITYPE_UMDRIVERPRIVATE`, generated from the contract's filler, with the firmware section filled from this session's PSP load and an identity trailer |
| `gdi_private.h`, `surface_resource_private.h` | allocation private data (`LB7A`), the GDI and the E26R resource-group contracts |
| `bc250kmd.h` | the device structure, the stage table and the interface-version notes |
| `pnp.c` | add/start/stop/remove, the single child (always-connected video output, no EDID), power |
| `display.c` | VidPN (one source, one target, one mode, identity only), `PresentDisplayOnly`, bugcheck display, the escape query |
| `guard.c` | boot-loop guard, stage breadcrumbs in the registry, and the log: `DbgPrintEx` plus a ring inside the driver image that `bc250kmd_cli log` reads back |
| `mmio.c`, `gen_regs.py`, `regs.generated.h` | BAR5 behind `EnableMmio` / `EnableMmioWrite`; every access checked against tables generated through regcalc (E07) |
| `sequence.c` | the kernel backend of `driver/shim`, shared by every bring-up sequence: registers only through the sequence's own generated table, first refused access stops all further writes, a plan executes no write and records what would be written |
| `gart.c` | M4: the GART command (plan, enable, restore) around AMD's imported hub code, behind `EnableGart`; registers through a table generated from amdgpu's own trace of the step (E09) |
| `psp.c` | M5, first part: the PSP command (plan, load, unload) around AMD's imported `psp_v11_0_8.c` and `driver/shim/bc250_psp.c`, behind `EnablePsp`: PSP ring, trusted memory region and the ten firmware images, staged in the top of VRAM from files in `C:\BC250\firmware` (E10) |
| `gpumem.c` | M5, second part: GPU memory and doorbells for `driver/shim` (`bc250_shim_mem_alloc/free`, `bc250_shim_wdoorbell64`): a VRAM pool 32 MB to 8 MB below the end of VRAM (checked clear of the firmware's framebuffer), GTT pages of contiguous system memory entered into M4's GART table, one doorbell page of BAR2 with even indices up to `0x31E`. A GTT page returns to Windows only unbound and with the engines read halted, otherwise it is leaked on purpose; a plan never writes the GART table or a doorbell |
| `gfx.c` | M5, second part: the GFX command (plan, run to a stage, fini, state) around `driver/shim/bc250_gfx.c` and `bc250_sdma.c`, behind `EnableGfx`: doorbell aperture, golden registers, GRBM CAM probe, constants, RLC, CP with its ring tests, SDMA, one stage at a time; needs the GART enabled and the PSP load done; registers through a table generated from amdgpu's trace of these steps; a plan answers reads of registers it has planned a write to with the planned value, as the host replay does (E11: all seven stages and the undo run on unit A, facts M36, M37) |
| `ih.c` | M6: the interrupt routine and the DPC of the miniport and the IH command (`ih plan`, `init`, `fini`, `state`), behind `EnableIh` (needs `EnableGfx`). State of 0.6.0: counting only, no interrupt is claimed while the IH ring is off; `state` reports what Windows assigned (message or line, vector) and the counts, with the gate closed as well. The INF's `.HW` section asks for one MSI message (E12 part A: granted, and silent, facts M38) |
| `vram.c` | the VRAM carve-out by system physical address and through BAR0, behind `EnableVram` / `EnableVramWrite`; one page per access, writes only in a test page (E08) |
| `bc250kmd_escape.h` | private escape data shared with lab tools |
| `bc250kmd.inf`, `build.ps1` | package and build (direct `cl`/`link`, `/W4 /WX`, test-signed) |
| `test/` | host tests and their runners (`run_*.ps1`) for the parts that compile without the WDK |

## Built for a lab without a kernel debugger

- **Start budget.** `Services\bc250kmd\Parameters\UnconfirmedStarts` is incremented at every
  `DxgkDdiStartDevice`. At 2 the driver refuses to start and Windows falls back to the Basic Display driver.
  Under the full table a start is confirmed through its health witness (`start_health.c`,
  [docs/design/wddm-start-confirmation.md](../../docs/design/wddm-start-confirmation.md)): the monitor watches
  60 s of fresh completed-primary flips for one start identity, then asks `health confirm <generation> <epoch>`,
  and the kernel checks that identity before it writes 0 and flushes the key (M457). The display-only table keeps
  the older rule (`LastStage` 61 and 60 s up). By hand it is `bc250kmd_cli confirm` (`tools/win/bc250kmd_cli`)
  or `mon.py action kmd.confirm`, recorded as a human decision. Installing the package resets the counter.
- **Breadcrumbs.** `LastStage` (a `BC250_STAGE` number) and `StageHistory` in the same key are written and
  flushed at every step of start-up and at the first commit and first present. After a hang and a power
  cycle they say how far the driver got. `bc250mon`'s bc250kmd panel and `bc250kmd_cli stages` read them and
  name them; both keep a copy of the `BC250_STAGE` table that a test checks against this driver's header.
- **A readable log** (0.7.1). `GuardLog` writes to `DbgPrintEx`, which on this lab machine - headless, over SSH,
  no kernel debugger, no DebugView - reaches nobody. Every line therefore also goes into a ring of 1024 lines
  inside the driver image, each with a sequence number and the milliseconds since `DriverEntry`, and
  `bc250kmd_cli log` reads it back through an escape (administrators only). The first 256 lines of a load are
  never overwritten, because the order of a start-up is usually the evidence; the rest wrap, and the lines the
  wrap cost are counted rather than quietly dropped. Appending takes a spin lock, so a caller above
  `DISPATCH_LEVEL` is counted instead of logged, and the count is printed with the rest. The ring survives a
  device stop and start but not a driver unload, which makes it a reload detector as well: see "Running stage A".
- The service is `ErrorControl = 0`: a failed start never stops the boot.

## The DDI table gate (M7 stage A, ADR 0008)

`Services\bc250kmd\Parameters\EnableFullWddm`, a `REG_DWORD` that the INF writes as 0, like every other gate in
this driver. The lab's install scripts open the gates they need afterwards; the current lab deployment runs the
full table (M727 reports health flags 15, of which flag 1 is full WDDM, see `tools/win/bc250kmd_cli/README.md`):

| Value | `DriverEntry` calls | Table |
|---|---|---|
| 0 (INF default) | `DxgkInitializeDisplayOnlyDriver` | the display-only table of M3 |
| 1 | `DxgkInitialize` | `wddm.c`: the full graphics miniport table |

The gate is read once, in `DriverEntry`, before the driver object is touched, so at 0 not a line of `wddm.c` runs.
Both paths are counted by the same start budget, because both can cost a boot.

**One binary, one interface version.** The whole driver is compiled at `DXGKDDI_INTERFACE_VERSION_WDDM2_0`
(`bc250kmd.h` says why, with the measurement behind it): the WDK headers change the *shape* of `DXGKRNL_INTERFACE`
and of most `DXGKARG_*` structures with that macro, and `BC250_DEVICE` embeds a `DXGKRNL_INTERFACE`, so two
translation units at two versions would disagree about the layout of this driver's own device structure. The
display-only table is unaffected: `KMDDOD_INITIALIZATION_DATA` and all 30 of its members are identical at both
versions, and so are every member of `DXGK_DRIVERCAPS`, `DXGKARG_QUERYADAPTERINFO` and `DXGKARG_ESCAPE` that
`display.c` reads or writes. `Version` inside each table is a run-time value and is unchanged in either path.
ADR 0019 (accepted direction, 2026-09-27) moves the binary to the newest version the WDK offers; the code is still
at 0x5023 (`bc250kmd.h`).

### What stage A is, and is not

This section and "Running stage A" are the record of E16 (M61 to M71). They describe the full table as it was
at stage A; what it does today is in the file table above.

Stage A is the smallest driver dxgkrnl will accept as a full graphics miniport. It exists to answer one question
that no public source answers: does the adapter start, and does the desktop survive, when a started full WDDM
adapter is the only one in the machine and nothing can render on it. So it is built to be inert:

- no register, no BAR mapping, no doorbell in any of the new DDIs - the memory segment's geometry comes from what
  `vram.c` already identified behind `EnableVram`, and the display DDIs leave the firmware's framebuffer, which
  `display.c` owns, exactly where it is;
- no submission: a packet is recorded when dxgkrnl hands it over and reported finished from a DPC of our own,
  after the submit DDI has returned;
- a **software VSync**: the driver claims `FlipOnVSyncMmIo`, and dxgkrnl retires a queued flip only when the
  driver reports `DXGK_INTERRUPT_CRTC_VSYNC`. Stage A may not read the display core, so a periodic timer at
  60 Hz stands in for the real one, running only while a source is visible and only reporting while
  `DxgkDdiControlInterrupt` has the VSync switched on. `DxgkDdiGetScanLine` answers from that timer's phase.
  Without this the second flip never leaves the queue and DWM stops - it is the difference between stage A
  meeting its exit criterion and not;
- no failure return from the DDIs whose failure is a bugcheck;
- every new DDI logs its first calls, because *which* DDIs dxgkrnl calls on an adapter nobody can render on, and
  in which order, is the evidence stage A is run for. Counting goes on after the logging stops: each DDI keeps a
  call count and the log line its first call landed on, `QueryAdapterInfo` keeps every information type it was
  asked for and `BuildPagingBuffer` every operation, and presents, flips, VSync ticks and VSync reports are
  counted separately. `bc250kmd_cli log summary` writes all of it into the log ring on demand, and the stop does
  it by itself. `DxgkDdiResetFromTimeout` and `DxgkDdiRestartFromTimeout` are the only DDIs that log **every**
  call: a TDR changes what the whole run means.

  **Reading the summary: a counted thing with no line spent the budget, and `first at N` is not its line.**
  The budget is `BC250_WDDM_LOG_CALLS` (8) calls per DDI, whatever they answered, plus - for
  `QueryAdapterInfo` only - every refusal among the first 256. So a *late refusal* still gets a line and a
  *late success* does not, and "it has no line because it succeeded" is the wrong rule: E16 run 004 logs 9
  `QueryAdapterInfo` lines but counts 12, the three missing ones being types 11 (twice) and 10, which
  succeeded after type 14 had spent the budget, while type 16's refusal came later still and was logged.
  Worse for a reader, `first at N` is the ring's sequence number at the moment that type was first *noted*,
  which for a type that was never logged is a line belonging to some other call: in run 004 the summary says
  `adapter info type 10 ... first at 29`, and line 29 is the type 16 refusal. `tools/runcompare` prints the
  counted-versus-logged difference and refuses to print `first at` for such a type.

### Running stage A

Everything below is under `HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters`, all `REG_DWORD`, and every
install writes every gate back to 0.

**1. The gates.** Open `EnableMmio`, `EnableVram` and `EnableFullWddm`, so that the memory segment reported to
VidMm is the real carve-out and not an empty list. With `EnableVram` closed the driver declares zero segments and
says so in the log; that is a second data point, not the main one. `EnableGart`, `EnablePsp`, `EnableGfx` and
`EnableIh` stay closed: stage A submits nothing and starts no engine.

```powershell
$p = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
Set-ItemProperty $p EnableMmio 1; Set-ItemProperty $p EnableVram 1; Set-ItemProperty $p EnableFullWddm 1
```

**2. The reload, and why it is not optional.** `EnableFullWddm` is read in `DriverEntry` (`entry.c:80`), not at
device start, so changing it does nothing until `DriverEntry` runs again. A device disable and enable in Device
Manager (`pnpdevprop`/`pnputil`, or `Disable-PnpDevice`/`Enable-PnpDevice` over SSH) tears the device down through
`DxgkDdiRemoveDevice` and, when it was the last device the driver object had, Windows unloads the image and calls
`DxgkDdiUnload` (`Bc250Unload` in `pnp.c`). Whether that really happens on this machine is **not** something the
code can promise - it is the PnP manager's decision, and a driver whose image stays referenced is enabled again
without a new `DriverEntry`.

So do not assume it: the log ring answers it by itself. The ring is a static array in the driver image, so an
unload wipes it and the sequence number starts at 0 again.

```powershell
bc250kmd_cli log 0 | Select-Object -First 3      # note the highest sequence number and the gate line
Disable-PnpDevice -InstanceId <id> -Confirm:$false; Enable-PnpDevice -InstanceId <id> -Confirm:$false
bc250kmd_cli log 0 | Select-Object -First 3
```

If the second `log` starts at sequence 0 with a fresh `gate: EnableFullWddm 1` line, the driver unloaded and
re-read the gate: the full table is live. If the sequence carries on counting from where it was, the image never
went away, the gate was never re-read, and the only way in is a **reboot** (`shutdown /r /t 0`).

**3. Reading the log.** Read it **before** closing the gate again, for the same reason: closing the gate needs
another reload, and the reload takes the ring with it.

```powershell
bc250kmd_cli log summary > stage-a.txt      # counters into the ring first, then the whole ring
```

The output is `sequence seconds.milliseconds text`. The header line says how many lines exist, how many the
wrapping tail overwrote and how many were dropped because a caller was above `DISPATCH_LEVEL`, and whether the
full table is live at all (`table FULL WDDM`). The first 256 lines of a load are never overwritten, so the
start-up order survives however long the run was; a gap between sequence 255 and the next line is the wrap, and
`lost` counts it. Ask for `log summary` **once, at the end**: each call appends a block of about 40 lines, and
in a run long enough to wrap, every such block evicts as many lines of the run's middle from the tail. Plain
`bc250kmd_cli log` reads without writing and can be run as often as wanted.

**4. If the desktop freezes but SSH still works.** Close the gate and reload:

```powershell
Set-ItemProperty $p EnableFullWddm 0
bc250kmd_cli log summary > stage-a-frozen.txt        # the evidence, before the reload destroys it
Disable-PnpDevice -InstanceId <id> -Confirm:$false; Enable-PnpDevice -InstanceId <id> -Confirm:$false
```

If SSH does not answer either, the way back is the power button, and the start budget below takes over.

**5. What the start budget does in full WDDM mode.** `GuardCheckAndCountStart()` is called from
`Bc250StartDevice` (`pnp.c`), and `WddmBuildTable`, which `DriverEntry` calls when the gate is open, puts
that same function into the full table (`Data->DxgkDdiStartDevice = Bc250StartDevice`), so the budget covers
both tables with one counter. Every start increments
`UnconfirmedStarts`; `bc250kmd_cli confirm` clears it once the desktop is up. At
`BC250_MAX_UNCONFIRMED_STARTS` (2, `bc250kmd.h`) the driver refuses to start and Windows falls back to Basic
Display - so a full WDDM table that kills the desktop costs two boots, not an endless loop, and the third boot
comes up on Basic Display with SSH and the registry reachable. At stage A the confirmation was manual, because
`bc250mon` then confirmed a start by itself only at stage 61 (the first display-only present). The full table
now has its own automatic confirmation through the health witness (see "Built for a lab without a kernel
debugger", M457).

**6. The two runs.** Measured since this section was written (E16 runs 001 to 003, facts M62 to M64): the first
of the two runs below **cannot start** on the lab's Windows. dxgkrnl refuses a full, non compute-only adapter whose
software key has no `UserModeDriverName`, right after the DRIVERCAPS query. Every gate-open run uses `package-umd`;
the plain `package` is for display-only use. The original plan is kept below because the build still produces both.

Stage A was planned to run **twice**, the difference being the `UserModeDriverName` block in
`bc250kmd.inf`, which ships commented out:

1. **without it** (`package`) - the adapter has no user-mode driver name at all, so Direct3D cannot even load one;
2. **with it** (`package-umd`), and `bc250umd.dll` from `driver/umd-stub` installed - the DLL loads and every
   `OpenAdapter` entry point returns `E_NOTIMPL`. Each one writes a line through `OutputDebugString` first, so
   which entry point the runtime asks for, and in which order, is visible to any debug-output viewer without the
   DLL keeping state.

Those are two different failures and the runtime may not treat them alike. Both packages come out of one
`build.ps1 -UmdStub` run (below) carrying the **same** `bc250kmd.sys`, which the build proves by printing both
copies' SHA256: the second run must differ from the first in the INF and the DLL only, or it measures two things
at once. The run 2 INF is generated from the repository's by the markers documented inside `bc250kmd.inf`
(`;@UMD` enables a line, `;@PLAIN-ONLY` disables one) and the build throws if they are missing, because a run 2
package quietly missing that block is byte for byte the run 1 experiment wearing the wrong name.

`UserModeDriverNameWow` is deliberately **not** written. Only a 64-bit stub is built, so a Wow value naming
`bc250umd.dll` would point 32-bit processes at a file that is not in SysWOW64, and "the runtime never asked for a
32-bit driver" and "it asked and the load failed" would look identical - which is the one distinction run 2
exists to make. Everything that renders in run 2 (DWM on this x64 build, and the Direct3D 11 attempt from the SSH
session) is 64-bit. The INF says what it would take to add it.

## Build

```powershell
pwsh driver\kmd\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Out $env:BC250_ROOT\scratch\build\bc250kmd
pwsh driver\umd-stub\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Out $env:BC250_ROOT\scratch\build\bc250umd
```

For stage A's second run, build the stub first and hand it to the driver build, which then writes both packages:

```powershell
pwsh driver\umd-stub\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Out $env:BC250_ROOT\scratch\build\bc250umd
pwsh driver\kmd\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Out $env:BC250_ROOT\scratch\build\bc250kmd `
                          -UmdStub $env:BC250_ROOT\scratch\build\bc250umd
```

`<Out>\package` is run 1 and `<Out>\package-umd` is run 2; both are catalogued and test-signed, and the run 2
catalog covers `bc250umd.dll` as well. Without `-UmdStub` nothing about the plain package changes.

## What M3 had to show on hardware (acceptance, mirrors E05)

Met in E06 (M28, M29); kept as the record of the criteria.

1. Installs on the device, starts without a problem code, desktop stays visible at the firmware's mode.
2. `LastStage` reaches 61 (first present done).
3. A BAR5 sweep with `bc250rd` before and after shows no change caused by the driver.
4. Survives disable/enable and a reboot; uninstall brings Basic Display back on the same framebuffer.
5. Measured, either way: does `D3DKMTEscape` reach a display-only driver (decides the control channel for M4).
