# E22 - the display flip on DCN 2.0.1 under Windows (ADR 0011)

State: **step 1 done** (run 001, facts M92); step 2 (the flip) not started.

## Why

ADR 0011 says the present is a flip, not a CPU blit: `SetVidPnSourceAddress` (and the CDD's presents once
the full table runs) must land the surface's system physical address in `HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS`
the way amdgpu does it (E21, facts M85-M88), and a vblank interrupt must come from `OTG0` (M88). Before the first
display write the offsets and the mapping have to be proven under Windows. Kto nie ryzykuje, ten w kozie nie
siedzi (who takes no risk sits in no jail) - so no risk in step 1: reads only.

## Steps

1. **Read-only dump** (bc250kmd 0.7.19, `bc250kmd_cli dcn`, escape `BC250_ESCAPE_RUN_DCN`, review 19): 75 DMU
   registers on their own generated allow list (`gen_regs.py`, ip DMU), decoded HUBP0/OTG0 summary next to the
   Linux reference values of `evidence/linux/2026-09-22-E21-linux-reference-4/dmupre.txt`.
   - H1: the offsets regcalc derives for the DMU IP read the same registers under Windows as `regs2.py` read
     under Linux -> **confirmed**, every value of the firmware state equal (M92).
   - H2: the firmware's display programming is untouched by the display-only driver and by dxgkrnl's POST path
     -> **confirmed** (M92).
   - H3: OTG0 keeps scanning at 60 Hz while our driver runs -> **confirmed**, frame count +60 in ~1 s (M92).
2. **Flip** (next build): behind a new gate `EnableDcnFlip`, `SetVidPnSourceAddress` writes the M87 sequence
   for HUBP0 only (the firmware lights HUBP0 alone; amdgpu's HUBP0+HUBP3 split is wishlist L30), with the
   address converted MC -> physical (M85), the firmware's address restored at stop, and the vblank interrupt
   from `OTG0_OTG_GLOBAL_SYNC_STATUS` bit 12 / IH client 4 src 0x57 (M88) reported through
   `DxgkCbNotifyInterrupt` as `DXGK_INTERRUPT_CRTC_VSYNC`. Flip-done = `HUBPREQ0_DCSURF_FLIP_CONTROL` bit 0x100
   clear, polled from the vblank DPC the way 6.18.52 does. Hypotheses to write before that run: H4 the first
   flip to the firmware's own address (a no-op flip) leaves the picture intact; H5 a flip to a driver-filled
   surface shows it; H6 the interrupt arrives once per frame and stops when disabled.

## Runs

| Run | Build | What | Result |
|---|---|---|---|
| 001 | 0.7.19 (d1b7651) | dump twice, EnableMmio only, display-only | all 75 registers read, values = Linux firmware state, frame count moving. `evidence/windows/2026-09-22-E22-dcn-read-run-001/` |
