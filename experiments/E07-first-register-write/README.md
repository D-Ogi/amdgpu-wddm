# E07: first register write under Windows, through our own miniport

State: **run 001 done, H1-H5 hold** (2026-09-21). Evidence: `evidence/windows/2026-09-21-E07-run-001/`.

## Why

Everything after M3 needs the miniport to reach registers. This experiment introduces that path (ADR 0007:
gated BAR5 mapping, generated read/write tables, escapes for administrators) and proves it with the most
harmless write there is: `SCRATCH_REG0`, a register without function that amdgpu itself writes eleven times
during init as its ring-test target (facts M24). Prior art passed a scratch write/read-back test with every
other address wrong (`docs/prior-art-vs-hardware.md`), so one register is not enough: `SCRATCH_REG1` is the
second control, and an independent driver (`bc250rd`, its own BAR mapping) is the witness.

Offsets come from `tools/regcalc` (`SCRATCH_REG0` = `0x30100`, `SCRATCH_REG1` = `0x30104`, as generated into
`driver/kmd/regs.generated.h`); the target resolves names through `bc250rd`'s `reglist.txt`.

## Hypotheses

- H1. Closed gates: after a plain install `read` and `write` escapes are refused (`STATUS_DEVICE_NOT_READY`,
  "not mapped"), and the driver reaches stage 61 through the new stage 35 exactly like the M3 build.
- H2. `EnableMmio = 1`, device restarted: reads through the miniport return the same values as `bc250rd` for
  `GRBM_STATUS`, `GB_ADDR_CONFIG`, `CP_ME_CNTL`, `SCRATCH_REG0`, `SCRATCH_REG1`; a read of an offset that is not
  in the table (`GRBM_GFX_CNTL`, excluded for its read side effect, facts M25) is refused with `STATUS_ACCESS_DENIED`; a write is still refused
  (`STATUS_DEVICE_NOT_READY`).
- H3. `EnableMmioWrite = 1`, device restarted: `write SCRATCH_REG0 = 0xCAFEDEAD` and `SCRATCH_REG1 = 0x0BC25001`
  read back through the miniport **and through `bc250rd`**; a write to a register outside the write table
  (`GRBM_SCRATCH_REG0`) is refused with `STATUS_ACCESS_DENIED` and its value does not change.
- H4. A GC sweep after the writes differs from the sweep before them in exactly those two registers, outside
  the noise set of two unchanged sweeps.
- H5. Writing the old values back restores both registers; the display never noticed (stage 61, presents
  keep counting, no problem code).

What would refute: any register other than the two changing (H4), a read-back through `bc250rd` that differs
from the miniport's (wrong mapping), a refused offset that was executed anyway.

## Safety

Two scratch registers without function, values restored afterwards. The GPU stays underclocked. If the
machine hangs, the start budget and the closed-by-default gates mean the next boot runs the plain M3 display
path: the gates are opened by this experiment's script only and closed again at its end.

## Procedure

`e07_target.ps1` on the target, one phase per call, logs under `C:\BC250\e07\out`:
`install` (new package, gates closed) -> `closed` (H1) -> `gate -Mmio 1` + restart device -> `reads` (H2) ->
`gate -Mmio 1 -Write 1` + restart device -> two noise-floor sweeps -> `writes` (H3) -> sweep (H4) ->
`restore` (H5) -> `gate -Mmio 0 -Write 0` + restart device.

## Result (run 001)

| | Outcome |
|---|---|
| H1 closed gates | **holds**: every register escape refused with `STATUS_DEVICE_NOT_READY`; start history `10 20 30 31 32 33 34 35 39 50 60 61` |
| H2 read gate | **holds**: six registers bit-identical through `bc250kmd` and `bc250rd`; `GRBM_GFX_CNTL` refused with `STATUS_ACCESS_DENIED`; writes still refused |
| H3 writes | **holds**: `SCRATCH_REG0 = 0xCAFEDEAD`, `SCRATCH_REG1 = 0x0BC25001`, read back by the driver and by the independent witness; a write to `GRBM_SCRATCH_REG0` refused with `STATUS_ACCESS_DENIED`, value unchanged |
| H4 nothing else changed | **holds**: the sweep after the writes differs from the control in exactly these two registers outside the noise set |
| H5 restore, display untouched | **holds**: both back to 0; stage 61, presents counting, no problem code throughout; gates closed again at the end |

This is the first write to a GPU register under Windows on this project, and it went to the address regcalc
computed (facts M30). Three device restarts inside one boot went through because the script confirms each
start once it has seen stage 61.
