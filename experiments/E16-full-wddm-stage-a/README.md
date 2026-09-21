# E16: a full WDDM adapter nobody can render on (milestone M7, stage A of ADR 0008)

State: **planned (2026-09-21). README written before the run.**

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
5. Gate closed, the plain package installed again. State, witness sweeps `after`.

## Result

(after the run)
