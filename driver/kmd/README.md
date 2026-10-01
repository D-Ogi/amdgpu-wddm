# driver/kmd: bc250kmd, the WDDM miniport

State: **milestones M3 and M4 reached on unit A** (experiment E06, facts M28 and M29): the driver is installed on the
lab machine and runs its display. The acceptance list below is met; escape reaches the driver. M5's first part
(0.5.2): the PSP takes the GPU firmware from this driver (experiment E10, facts M34 and M35). M5 reached (0.5.5):
RLC, KIQ, MEC, the queues and SDMA brought up from this driver, every ring test passes (experiment E11, facts M36
and M37).

What it is (ADR 0006): a display-only miniport for `PCI\VEN_1002&DEV_13FE` that takes over the firmware's
framebuffer through post-display ownership, offers exactly the mode the firmware left, and presents by CPU
copy. The display path performs **no MMIO** and enables no interrupt; the bring-up code next to it (ADR 0007)
touches hardware only behind registry gates that default to 0 and that every install closes. Written from
the documented DDI; no code from Microsoft's MS-PL sample.

| File | Contents |
|---|---|
| `entry.c` | `DriverEntry` and the `EnableFullWddm` gate: the display-only DDI table, or M7's full one |
| `wddm.c` | M7 stage A: the full WDDM DDI table behind that gate, inert (see below) |
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

## Built for a lab without a kernel debugger

- **Start budget.** `Services\bc250kmd\Parameters\UnconfirmedStarts` is incremented at every
  `DxgkDdiStartDevice`. At 2 the driver refuses to start and Windows falls back to the Basic Display driver.
  User mode confirms a good start by writing 0: `bc250mon`'s `KmdProvider` does it by itself once the desktop
  has been up for 60 s and `LastStage` has reached 61, and by hand it is `bc250kmd_cli confirm`
  (`tools/win/bc250kmd_cli`) or `mon.py action kmd.confirm`. Installing the package resets it.
- **Hang evidence** (0.7.172). `g_Bc250Progress` records entries and exits of the ISR, DPCs and submission
  paths, and `EnableHangBugcheck=1` arms a test-only detector that bugchecks with `0xBC250BAD` when ordinary
  threads stop running. Default off. See `docs/design/hang-detector.md`.
- **Hang recovery** (0.7.194, M15.12 stage 1). `HangRecoveryMode=1` lets `DxgkDdiResetEngine` kill the waves of
  a hung node-0 job and, if its fence then retires, report the engine reset as done instead of refusing it, so
  that only the guilty process loses its device. Every call leaves a flushed verdict in
  `Parameters\HangRecovery`, which outlives the 0x116 of a refusal. Default off, closed by every install. Not
  yet run on the lab. See `docs/design/hang-recovery.md`.
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

`Services\bc250kmd\Parameters\EnableFullWddm`, a `REG_DWORD` that every install sets back to 0, like every other
gate in this driver:

| Value | `DriverEntry` calls | Table |
|---|---|---|
| 0 (default) | `DxgkInitializeDisplayOnlyDriver` | the display-only table of M3, unchanged, byte for byte the driver that runs the owner's display today |
| 1 | `DxgkInitialize` | `wddm.c`: the same 28 pointers plus what a full graphics miniport may not leave out |

The gate is read once, in `DriverEntry`, before the driver object is touched, so at 0 not a line of `wddm.c` runs.
Both paths are counted by the same start budget, because both can cost a boot.

**One binary, one interface version.** The whole driver is compiled at `DXGKDDI_INTERFACE_VERSION_WDDM3_1`
(`bc250kmd.h` says why, with the measurement behind it): the WDK headers change the *shape* of `DXGKRNL_INTERFACE`
and of most `DXGKARG_*` structures with that macro, and `BC250_DEVICE` embeds a `DXGKRNL_INTERFACE`, so two
translation units at two versions would disagree about the layout of this driver's own device structure. The
display-only table is unaffected: `KMDDOD_INITIALIZATION_DATA` and all 30 of its members are identical at both
versions, and so are every member of `DXGK_DRIVERCAPS`, `DXGKARG_QUERYADAPTERINFO` and `DXGKARG_ESCAPE` that
`display.c` reads or writes. `Version` inside each table is a run-time value and is unchanged in either path.
ADR 0019 stage B1 moved the interface version from 0x5023 to 0x10004, the newest whose whole table, caps buffer
and callback interface the lab's dxgkrnl (22621) reads; `DXGK_DRIVERCAPS.WDDMVersion` stays 2.0 and every DDI the
2.1-3.1 headers add stays NULL until the WDDMVersion steps of stage B4 (`bc250kmd.h`, `wddm.c` `WddmCheckReserved`).

### What stage A is, and is not

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
`DxgkDdiUnload` (`Bc250Unload`, `pnp.c:254`). Whether that really happens on this machine is **not** something the
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
`Bc250StartDevice` (`pnp.c:41`), and `WddmBuildTable`, which `DriverEntry` calls when the gate is open, puts
that same function into the full table (`Data->DxgkDdiStartDevice = Bc250StartDevice`), so the budget covers
both tables with one counter. Every start increments
`UnconfirmedStarts`; `bc250kmd_cli confirm` clears it once the desktop is up. At
`BC250_MAX_UNCONFIRMED_STARTS` (2, `bc250kmd.h:52`) the driver refuses to start and Windows falls back to Basic
Display - so a full WDDM table that kills the desktop costs two boots, not an endless loop, and the third boot
comes up on Basic Display with SSH and the registry reachable. The confirmation is manual here: `bc250mon`
confirms a start by itself only at stage 61 (the first display-only present), which a full WDDM start may never
reach.

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
pwsh driver\kmd\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\bc250kmd
pwsh driver\umd-stub\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\bc250umd
```

For stage A's second run, build the stub first and hand it to the driver build, which then writes both packages:

```powershell
pwsh driver\umd-stub\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\bc250umd
pwsh driver\kmd\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\bc250kmd `
                          -UmdStub P:\BC-250\scratch\build\bc250umd
```

`<Out>\package` is run 1 and `<Out>\package-umd` is run 2; both are catalogued and test-signed, and the run 2
catalog covers `bc250umd.dll` as well. Without `-UmdStub` nothing about the plain package changes.

## What M3 has to show on hardware (acceptance, mirrors E05)

1. Installs on the device, starts without a problem code, desktop stays visible at the firmware's mode.
2. `LastStage` reaches 61 (first present done).
3. A BAR5 sweep with `bc250rd` before and after shows no change caused by the driver.
4. Survives disable/enable and a reboot; uninstall brings Basic Display back on the same framebuffer.
5. Measured, either way: does `D3DKMTEscape` reach a display-only driver (decides the control channel for M4).
