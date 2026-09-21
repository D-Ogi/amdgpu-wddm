# E19: the full WDDM table, stage C - a real submission on the gfx ring

Date: 2026-09-22. State: run 001 done (M75); run 002 (bc250kmd 0.7.13) prepared. ADR 0008 stage C; follows E18 (M73).

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
- H4 (if H3 holds): the same submission with a SET_UCONFIG_REG write in the buffer (`kmtprobe --submit --ib-dwords`,
  if the probe has it by then) changes the scratch register: the buffer's CONTENT was executed, not only fetched.

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
