# E19: the full WDDM table, stage C - a real submission on the gfx ring

Date: 2026-09-22. State: DONE. H2, H3 and H4 hold (M76, M77, M80): stage C's exit criterion is met and the buffer's content ran. ADR 0008 stage C; follows E18 (M73).

## Why

Stage B left page tables in the hardware's format that nothing walks. Stage C points a VMID at them and sends a DMA
buffer down the gfx ring, with the fence reported from the interrupt (research document section 5.3). Everything below
the DDI ran under the display-only table in M6 (E11, E12, E15): GART, PSP firmware load, the gfx and compute rings, the
IH ring, fences, a compute dispatch. The first question is therefore not about packets.

## Hypotheses

- H1 (run 001, bc250kmd 0.7.12 unchanged). M6's bring-up does not care which DDI table is running: with
  `EnableFullWddm` = 1 and the engine gates open, `gart enable`, `psp load`, `ih init`, `gfx run 8` return what they
  returned in E15, gfx fences raise their end-of-pipe vectors, a 16-workgroup dispatch fills its bytes, and the undo
  (`gfx fini`, `ih fini`, `psp unload`, `gart restore`) returns 0. Meanwhile the full table goes on as in stage B:
  presents, software fences, no TDR. What could break it: the reserved tail of VRAM (GART table, TMR, ring pool) is
  outside VidMm's segment by construction (`BC250_VRAM_TOP_RESERVED`), and the interrupt routine is the same function
  in both tables - both read from the code, neither measured until now.
- H2 (run 002, bc250kmd 0.7.13, full table with the engines up as in run 001 - M75 says the table makes no difference). The gfx ring takes an INDIRECT_BUFFER packet:
  `bc250kmd_cli fence gfx 1 ib` has the driver build M6's ring test (a SET_UCONFIG_REG write of a pattern to
  `mmSCRATCH_REG0`'s ring-test twin) inside a GTT page of its own, submit it at VMID 0 followed by the fence, and the
  pattern reads back and the fence arrives with its end-of-pipe vector. Positive control for everything after it: the
  same ring test as direct packets (`fence gfx 1 test`) in the same device start. Until now only direct packets ran
  on this ring. The vector's `ring_id` for the gfx ring is recorded (never measured; amdgpu expects me 0 pipe 0).
- H3 (run 002, same device start, only if H2 held; full table, EnableGpuVa = 1, EnableGpuSubmit = 1, engines up). A D3DKMT client with a context
  (`kmtprobe --submit`) gets a `SetRootPageTable` for that context (stage B saw none without a context, M73); its
  command buffer of PM4 NOPs at a GPU VA of its choosing arrives in `SubmitCommandVirtual` with `DmaBufferSize` != 0;
  wddm.c hands it to `GfxSubmitIb` at VMID 1 with the context's root (physical, M73); the GPU walks VidMm's tables,
  fetches the buffer, the fence arrives through the IH DPC and is reported as DMA_COMPLETED; the client's monitored
  fence reaches its value. No TDR, no VM protection fault vector in `ih state`, no watchdog line in the ring log.
  CDD's presents (size 0) stay software completions and keep their order around the hardware one.
- H3 fails safely if the walk faults: the watchdog (500 ms) completes the fence in software, closes the ring path for
  this device start and says so in the log; the scheduler sees no timeout. A page fault on this part may still wedge
  the gfx engine (no reset, M53) - then `gfx fini` will say so and the unit needs a power cycle. That is the risk the
  second half of run 002 takes; its first half exists to take everything else out of it first.
- H4 (run 006; written after run 005, before run 006). The same submission with the shim's own ring-test packet at
  the head of the buffer (`kmtprobe --submit --scratch BC250B01`: `PACKET3_SET_UCONFIG_REG` of `mmSCRATCH_REG0`, the
  three dwords of `bc250_gfx_ib_ring_test_build()` with another value). Prediction: `bc250kmd_cli read 30100` gives
  `DEADBEEF` before it (left by run 006's own `fence gfx 1 ib`, which is also the proof that the read-back works) and
  `BC250B01` after it, with `submit=ok` and one more end-of-pipe vector. Then the CP fetched the buffer's bytes
  through VidMm's page tables at VMID 1 and executed them. Falsifier: `submit=ok` with `DEADBEEF` still there - the
  packet was passed but its content never ran (a fetch from somewhere else, or a fault that the CP skipped).

## Safety

No GPU reset exists on this part (facts M53): a hung engine costs the owner a trip to the power button. Run 001 uses
only sequences that passed three times in one device start in E15 run 002. Temperature read between steps, stop above
85 C. The gate is one-shot, AutoReboot is on, STOP flag honoured. Installs only with the gate closed (M74).

## Procedure

`e19_target.ps1` = E18's script plus `-Engines 0|1` on the gate phase (EnableGart, EnablePsp, EnableGfx, EnableIh) and
E15's pass-through phases (`gart`, `psp`, `ih`, `gfx`, `fence`). From run 002: `-GpuSubmit 0|1` on the gate phase
(EnableGpuSubmit), fence mode `ib`, and phase `submit` (`kmtprobe --submit`).

## Result

### Run 001 (2026-09-21, bc250kmd 0.7.12)

H1 holds in every part (facts M75, `evidence/windows/2026-09-21-E19-run-001/`): the bring-up, three gfx fences, a 16-workgroup dispatch and the undo
all ran under the full table, which did not notice. The ground under stage C is the one M6 built.

### Runs 002 to 005 (2026-09-22, bc250kmd 0.7.13)

Evidence and the run-by-run account: `evidence/windows/2026-09-22-E19-run-005/`.

- Run 002 hung the unit inside `gfx run 8` (M78): second bring-up of a boot, incomplete first undo. Not a bugcheck.
  Kto sieje wiatr, ten zbiera burzę - who sows the wind reaps the storm; run 001's short undo was the wind.
- H2 holds (M76, n = 3). Correction to the hypothesis as written above: its control, `fence gfx 1 test`, does not
  exist - mode `test` is for the SDMA rings, and the escape refused it before touching the ring. The control that ran
  is the plain fence. A stale bc250kmd_cli on the target cost one more refusal (the grown BC250_ESCAPE_FENCE).
- H3 holds (M77, n = 1, run 005), with the limit H3 itself did not state: NOPs prove that the CP got past the IB
  packet at VMID 1 and that no fault vector came, not that the buffer's bytes were read through the page tables.
  Run 004's attempt submitted nothing: the script passed `--timeout` in milliseconds to an option in seconds.
- The gfx ring's end-of-pipe vector: client 20, source 181, `ring_id` 0 (M79), also for the submission at VMID 1 -
  so run 005's third interrupt was an end-of-pipe, not a fault.
- Procedure from now on: fresh boot before a bring-up, complete undo after it (`gfx fini`, `ih fini`, `psp unload`,
  `gart restore`), the cli pushed to the directory the script uses.

### Run 006 (2026-09-22, bc250kmd 0.7.13, kmtprobe --scratch)

H4 holds as predicted (facts M80, `evidence/windows/2026-09-22-E19-run-006/`): `SCRATCH_REG0` read `DEADBEEF` before the submission and `BC250B01`
after it, with `submit=ok`, one more end-of-pipe vector and the fence reported from the hardware path. H3 repeated on
the way (M77, n = 2). The warm restart before this run did not come up (M21 again); the run used the boot that followed.
What stage C leaves for later: the watchdog and `ResetFromTimeout` paths were never exercised on the hardware; a
second bring-up in one boot after a complete undo was never measured (M78); one submission in flight at a time.
