# E19: the full WDDM table, stage C - a real submission on the gfx ring

Date: 2026-09-22. State: run 001 done (M75); the stage C driver build is next. ADR 0008 stage C; follows E18 (M73).

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
- H2 onwards: written with the stage C driver build, after `scratch\tmp\stageC_design.md`.

## Safety

No GPU reset exists on this part (facts M53): a hung engine costs the owner a trip to the power button. Run 001 uses
only sequences that passed three times in one device start in E15 run 002. Temperature read between steps, stop above
85 C. The gate is one-shot, AutoReboot is on, STOP flag honoured. Installs only with the gate closed (M74).

## Procedure

`e19_target.ps1` = E18's script plus `-Engines 0|1` on the gate phase (EnableGart, EnablePsp, EnableGfx, EnableIh) and
E15's pass-through phases (`gart`, `psp`, `ih`, `gfx`, `fence`).

## Result

### Run 001 (2026-09-21, bc250kmd 0.7.12)

H1 holds in every part (facts M75, `evidence/windows/2026-09-21-E19-run-001/`): the bring-up, three gfx fences, a 16-workgroup dispatch and the undo
all ran under the full table, which did not notice. The ground under stage C is the one M6 built.
