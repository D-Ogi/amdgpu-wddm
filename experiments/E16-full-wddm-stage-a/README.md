# E16: a full WDDM adapter nobody can render on (milestone M7, stage A of ADR 0008)

State: **runs 001 and 002 done (2026-09-21): dxgkrnl refuses the full table after the start, quietly and reversibly (M62), right after DRIVERCAPS, the only question it asks (M63); the cause was the missing UMD name (M64: read offline in the lab's dxgkrnl, confirmed by run 003, which gets two questions further and stops again). Run 004 = 0.7.4. The README above "Result" was written before the runs.**

## Why

M7 turns the display-only miniport into a full WDDM driver: VidMm gets a memory segment and GPU virtual addresses,
VidSch gets a node and submissions. ADR 0008 does that in stages behind a gate, and stage A is the stage with no
hardware in it: `EnableFullWddm = 1` makes `DriverEntry` call `DxgkInitialize` with the table in `driver/kmd/wddm.c`
instead of `DxgkInitializeDisplayOnlyDriver`. Every new DDI succeeds, touches no register, completes submissions in
software from a DPC, and a 16 ms timer stands in for the VSync interrupt.

What no public source says, and what this run is for: does dxgkrnl start such an adapter when it is the only one in
the machine, does the desktop survive on it (DWM has nothing to render with but WARP), and **which DDIs does dxgkrnl
call, in which order, with which arguments** before anybody has a user-mode driver. The answer orders stage B's work:
the DDIs that are called a thousand times before the first window appears are the ones that have to be real first.

## Hypotheses

- H1. Gate closed, 0.7.x is the display-only driver it was (stage 61, presents counting). Already seen in E15 run 002
  with 0.7.0; repeated here with the build under test.
- H2. Gate open (with `EnableMmio` and `EnableVram`, so the segment is the real carve-out): after the driver reload
  the device starts without a problem code and the log shows `DxgkDdiStartDevice`, the `QueryAdapterInfo` sequence
  (driver caps, segments, GPU MMU caps, node metadata) and `CreateDevice`/`CreateContext` for the system's own
  contexts. No DDI on the never-fail list returns a failure.
- H3. The desktop survives: the display keeps its picture, `SetVidPnSourceAddress` or `Present` calls keep arriving,
  VSync ticks are reported and flips retire. Measured from the driver's log; the second witness is `bc250mon`, the
  overlay in the lab's console session: two screenshots (`mon.py screenshot`) some seconds apart in which the overlay's
  clock has moved on show a desktop that still composes, and the overlay answering at all shows a session that lives.
- H4. No TDR and no bugcheck: `ResetFromTimeout` is never called, no new live kernel report, no display event 4101,
  the boot time does not change.
- H5. Without a user-mode driver name (run 1) Direct3D has nothing to load; with `bc250umd.dll` registered (run 2) the
  runtime loads it and asks for an `OpenAdapter` entry point, which answers `E_NOTIMPL`. Whether dxgkrnl's call pattern
  differs between the two is recorded, not predicted.
- H6. Gate closed again: the display-only driver comes back, stage 61, presents counting, as if nothing had happened.
  A witness sweep (GC, MMHUB, MP0, NBIO, OSSSYS) before run 1 and after run 2 differs only where it differed between
  two idle sweeps before.

Added after run 002, before run 003:

- H7 (first version, withdrawn before any run tested it): "the refusal is about the three WDDM 1.2 caps the answer
  left at 0". Microsoft's enforcement page suggested it; an offline reading of the lab's own `dxgkrnl.sys`
  (10.0.22621.6199, `evidence/windows/2026-09-21-E16-run-002/dxgkrnl-static-reading.txt`) says otherwise, and it
  is kept here because a hypothesis replaced is not a hypothesis that never existed.
- H7. The refusal of M63 is the missing `UserModeDriverName` (facts M64, hypothesis). Run 003 changes nothing but
  the package: the same 0.7.3 binary, installed as the run 2 package with the UMD stub, gate open. Prediction:
  the kept log shows a second `QueryAdapterInfo` call after DRIVERCAPS (type 15, which the driver refuses and
  dxgkrnl tolerates) and further adapter queries. Refuted if the log again ends with the DRIVERCAPS line and
  stage 70.
- H8. With the 0.7.3 table the start then fails further down, because `DxgkDdiSetStablePowerState` is NULL at
  interface `0x5023` (same reading). The device ends in a problem code again and the log says how far dxgkrnl got.
  If the device instead starts, H2 to H5 apply at once and the static reading has a hole worth finding.
- Run 004 is 0.7.4: `DxgkDdiSetStablePowerState`, `SupportDirectFlip`, `FlipCaps.FlipIndependent`, and the
  per-engine TDR set (three DDIs plus the cap) that every WDDM 1.2+ sample driver carries. Its hypotheses are
  H2 to H5 as written above, with the UMD stub package.

## Safety

- `wddm.c` holds no register access, no BAR mapping and no doorbell; the memory segment is described, never touched
  by the driver (VidMm may fill it through `BuildPagingBuffer`, which stage A answers without doing anything, so
  nothing reaches the carve-out either).
- The start budget (`UnconfirmedStarts`) counts the full WDDM start as well; a start that kills the desktop is not
  confirmed, and the third one is refused, which hands the display to Basic Display.
- The way back while SSH works: `EnableFullWddm = 0`, device disable/enable. If SSH does not work: the owner's power
  button, and the budget above takes care of the boots after it.
- The engines stay halted for the whole run: `EnableGart`, `EnablePsp`, `EnableGfx`, `EnableIh` closed.
- Temperature read before and after; stop above 85 C.

## Procedure

`e16_target.ps1` on the target, one phase per call; the driver's log ring is read with `bc250kmd_cli log` BEFORE the
gate is closed again, because the ring lives in the driver image and a driver unload takes it along.

1. Install the build under test (every install closes every gate). State, witness sweeps `before`.
2. Run 1: `EnableMmio`, `EnableVram`, `EnableFullWddm` = 1, device disable/enable. State, log, event log summary
   (counts only), after 10 s and after 120 s, each with a screenshot through `bc250mon`. The monitor confirms a start
   by itself only at stage 61 (first display-only present), which a full WDDM start may never reach, so a healthy
   start is confirmed by hand (`e16_target.ps1 -Phase confirm`) and an unhealthy one is left to the budget.
3. Gate closed, device disable/enable. State.
4. Run 2: the INF's `UserModeDriverName` block enabled and `bc250umd.dll` installed (a second package, same binary),
   then as step 2, plus a Direct3D 11 device creation attempt from the SSH session (its HRESULT is the datum).
5. Gate closed, the plain package back (`-Phase unumd`: the run 2 package carries build number `.1` so that its
   install is not a tie Windows resolves in favour of what is already there, and for the same reason the plain
   package cannot be installed over it; removing the run 2 package from the store returns the plain one). `state`
   prints `UserModeDriverName` from the device's class key in every phase, so which run a line belongs to is
   read, not remembered. State, witness sweeps `after`.

## Result

### Run 001 (2026-09-21, bc250kmd 0.7.1 then 0.7.2, plain package)

Evidence: `evidence/windows/2026-09-21-E16-run-001/` (README.txt has the timeline). Facts M61, M62.

- H1 holds: gate closed, 0.7.1 and 0.7.2 are the display-only driver they were (stage 61, presents counting).
- The log ring paid for itself on its first read, before the gate was opened: the POST display's `TargetId` is
  `0xFFFFFFFF` on this machine (M61), and stage A of 0.7.1 would have reported every VSync against it. 0.7.2
  reports against the child's UID. Had the gate been opened with 0.7.1, this run would have measured our own bug.
- **H2 is refuted as written.** Gate open: `DxgkDdiStartDevice` succeeds, dxgkrnl never commits a VidPN, stops the
  device and unloads the driver: `CM_PROB_FAILED_POST_START`, problem status 0 (M62). Windows records no reason.
- H3 and H5 were not reached. H4 is true and worthless: no TDR and no bugcheck, from a driver that never ran.
- H6 holds for a failed start: Basic Display takes the desktop, SSH and the overlay keep working, and gate closed
  plus disable/enable brings the display-only driver back. The failed start is counted by the start budget
  (2 of 2 after it, because the install before it had not been confirmed yet); it was cleared through the registry.
  Lesson for the procedure: confirm the display-only start before opening the gate.
- The instrument failed where it mattered: the ring lives in the driver image, dxgkrnl unloaded the image, and
  the one run the ring was built for left three breadcrumbs. Mądry Polak po szkodzie (a Pole is wise after the
  damage): 0.7.3 writes the ring to a file at the stop, behind its own gate, and logs what it answered, not only
  what it was asked.

### Run 002 (2026-09-21, bc250kmd 0.7.3, plain package, same table and answers as 0.7.2)

Evidence: `evidence/windows/2026-09-21-E16-run-002/` (README.txt has the timeline). Facts M61 (corrected), M63.

- The procedure's lesson was applied: install, stage 61, confirm, and only then the gate. The failed start cost one
  unit of the budget, not all of it.
- The kept log works: two files, `KeepStatus` 0, one from the display-only instance stopped by the disable and one
  from the full-table instance dxgkrnl refused.
- **The refusal is an answer to DRIVERCAPS** (M63). After `DxgkDdiStartDevice` dxgkrnl asks one question,
  `QueryAdapterInfo` type 1 into a 576-byte buffer, gets success, and stops the device 2 ms later. Nothing else of
  the full table is ever entered. Four of the reviewer's seven suspects (a refused type, the segment table,
  CreateContext, the root page table) are out: dxgkrnl never got that far.
- Windows still gives no reason: both DxgKrnl event channels are enabled and empty.
- M61 needed a correction: the POST display's `TargetId` is ours (`0x250001`) when the previous owner was a build
  that fills it at release (0.7.2 and later), and `0xFFFFFFFF` otherwise. The rule for the driver is unchanged.
- My first reading of this was wrong, and it took an hour to find out: "what is left is the content of the answer"
  (the three WDDM 1.2 caps we left at 0, after Microsoft's enforcement page). The lab's dxgkrnl, read offline, stops
  right after DRIVERCAPS for a reason that is not in DRIVERCAPS: the driver's software key has no
  `UserModeDriverName` (M64, hypothesis until run 003). Run 1 of this experiment was designed without a UMD on
  purpose, so it was designed to fail, and H5's first half ("without a user-mode driver name Direct3D has nothing
  to load") never had a chance to be observed. Nie szukaj dziury w całym (do not look for a hole in the whole):
  the part I suspected was fine.

### Run 003 (2026-09-21, the 0.7.3 binary as the UMD stub package 0.7.3.1)

Evidence: `evidence/windows/2026-09-21-E16-run-003/` (README.txt has the timeline). Fact M64.

- **H7 holds.** One difference from run 002, the package, and dxgkrnl asks on: DRIVERCAPS, then type 15
  (`PHYSICALADAPTERCAPS`) exactly as its code said it would, then type 47 (`64BITONLYCAPS`), both refused by the
  driver, then the stop. The missing `UserModeDriverName` was the wall runs 001 and 002 hit. The original split of
  this experiment into "run 1 without a UMD, run 2 with one" is void on this Windows: a full adapter without a UMD
  name does not start, so every gate-open run from here on uses the UMD stub package.
- H8 is compatible with the record and not proven by it: the start still fails (`CM_PROB_FAILED_POST_START`), and
  the log cannot say which check refused it, only that no DDI beyond `QueryAdapterInfo` was entered.
- H1 extended: the display-only table starts normally (stage 61) with a `UserModeDriverName` in the key.
- The way back worked a third time. 67.8 C before and after.

Run 004 is 0.7.4 (see "Added after run 002" above) on the UMD stub package.
