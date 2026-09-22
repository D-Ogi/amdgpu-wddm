# E22 - the display flip on DCN 2.0.1 under Windows (ADR 0011)

State: **steps 1 and 2 done** (run 001 M92, run 002 M94); H5's monitor half waits for the owner (a repeat of run 002 costs a minute); **step 3 (VidPN + interrupt) built, bc250kmd 0.7.24, review 24 GO, not yet run in the lab** - design in `docs/design/vidpn-flip.md`.

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
2. **One gated flip, no VidPN yet, no interrupt yet** (bc250kmd 0.7.20, `bc250kmd_cli dcnflip`, escape
   `BC250_ESCAPE_RUN_DCNFLIP`): behind a new gate `EnableDcnWrite` (needs `EnableMmio`), the M87 write sequence
   for HUBP0/OTG0 only - `OTG0_OTG_MASTER_UPDATE_LOCK` = 1, `HUBPREQ0_DCSURF_FLIP_CONTROL` = 0,
   `_SURFACE_CONTROL` = 0, the new address, `OTG0_OTG_MASTER_UPDATE_LOCK` = 0, `OTG0_OTG_TRIGA_MANUAL_TRIG` = 1,
   then a poll of `_FLIP_CONTROL` bit 0x100 (`SURFACE_FLIP_PENDING`) up to 50 ms - on a write allow list of its
   own (`g_MmioDcnWriteAllow`, `gen_regs.py`'s `DCN_WRITE_REGISTERS`, six registers). The firmware's own HUBP0
   address is captured once per device start (the first `dcnflip` of any kind) and restored at stop if the last
   flip left HUBP0 elsewhere (ADR 0011, consequences). An optional CPU fill (`Fill`/`FillColor`, needs
   `EnableVramWrite`, refused for the firmware's own address) paints the target surface before the flip, so a
   flip shows something visibly different without `SetVidPnSourceAddress` or a real allocation yet. Not
   `OTG0_OTG_GLOBAL_CONTROL0`, `OTG_VUPDATE_KEEPOUT` or `FLIP_CONTROL2`: amdgpu wrote those to values the
   firmware already has (0, 0, and `0x440` triple buffering) - M87's note - so this driver leaves them alone.
   Address validation: 4 KiB aligned, the whole 1920x1200 A8R8G8B8 surface (pitch 7680) inside the VRAM
   carve-out (M31), or the firmware's own address (always allowed, the no-op flip and the restore).
3. **`SetVidPnSourceAddress` and the interrupt** (bc250kmd 0.7.24, `EnableVidPnFlip`, ADR 0011
   point 3 step 3): the same M87 write sequence (`DcnFlipWriteSequence`, factored out of step 2's
   `DcnFlipCore`), called from `Bc250WddmSetVidPnSourceAddress` itself instead of the escape, on a real address
   change only. `DcnTranslateCardAddress` turns dxgkrnl's card-relative `PrimaryAddress` into the physical
   address the registers want (`physical = VramBase + (CardAddress - VramMcBase)`, facts M85, both numbers
   already measured by `VramStart` - never a literal), then the same `AddressAllowed` range check step 2 uses.
   The vblank interrupt (`OTG0_OTG_GLOBAL_SYNC_STATUS` bit 12 `VUPDATE_NO_LOCK_INT_EN` / IH client 4 src 0x57,
   M88) is armed by `DxgkDdiControlInterrupt(DXGK_INTERRUPT_CRTC_VSYNC)` in place of the software timer once the
   gate is open, acked at DIRQL directly on the register (no lock needed - `MmioDcnRead`/`MmioDcnWrite` take
   none), and reported from the vsync DPC as `DXGK_INTERRUPT_CRTC_VSYNC` carrying the address last flipped -
   after polling `HUBPREQ0_DCSURF_FLIP_CONTROL`'s `SURFACE_FLIP_PENDING` (M94: clears within one frame) so a
   report never claims a flip that has not actually landed. The firmware's own address is restored at
   `StopDevice` (`DcnStop`, unchanged, ordered before `MmioStop` by `WddmStop`'s own disarm of the hardware
   vsync source). Full reasoning, IRQL legality and the gate-closed regression argument: `docs/design/vidpn-flip.md`.
   Driven from the lab with `e19_target.ps1 -Phase gate -Full 1 -VidPnFlip 1` (sets `EnableDcnWrite` and
   `EnableVidPnFlip` together, Full-gated like every other engine).

Hypotheses for run 002 (step 2, the escape only):

- **H4**: a flip to the firmware's own address (a no-op flip: `dcnflip <firmware address>` or `dcnflip
  restore` before anything else has flipped) leaves the picture on the monitor intact, `SURFACE_FLIP_PENDING`
  clears, and `OTG0_OTG_STATUS_FRAME_COUNT` keeps advancing across it.
- **H5**: a flip to a driver-filled surface (`dcnflip 0x271000000 fill 0xFF2060C0`) shows the fill on the
  monitor - a blue field with a white border and a diagonal line - in place of the desktop, and `dcnflip
  restore` afterwards brings the desktop back.

Hypotheses for the first step 3 run (`EnableVidPnFlip`, not yet run in the lab):

- **H6**: with the CDD driving the desktop under the full WDDM table and `EnableVidPnFlip` open, dxgkrnl's own
  presents (DWM's flips, not the `dcnflip` escape) land on the monitor - the desktop is visible and moves when
  a window is dragged - and `WddmCounters`' `DcnFlipsHardware` advances roughly once per DWM composition, with
  `DcnFlipRefused` staying 0 across ordinary desktop use.
- **H7**: the VUPDATE_NO_LOCK interrupt arrives at the display's own rate (facts M86/M87/M94: 59.95 Hz, the
  1920x1200 CVT-RB timing) while a source is visible, `DcnVsyncTicks` and `wddm->VSyncReports` both advance at
  that rate, and
  `DcnVsyncDeferred` stays at or near 0 - a nonzero, growing `DcnVsyncDeferred` would mean
  `SURFACE_FLIP_PENDING` is not clearing within one frame the way M94 measured under the escape's own poll
  (`docs/design/vidpn-flip.md` section 10, open question 1).
- **H8**: no `HUBP_UNDERFLOW_STATUS` bit sets and no `Display 4101 (TDR)` event appears in `e19_target.ps1
  -Phase state`'s event counts across a run that includes ordinary desktop use (moving windows, opening an
  app) - the double-buffered hardware flip should be at least as clean as the software-timer path 0.7.23
  already exercises, never worse. `EnableDcnWrite`/`EnableVidPnFlip` closing again at the end of the run
  (`-Phase gate -Full 0`) should show every new counter unchanged from its last value, never a hardware access
  attempted afterward - the gate-closed regression bar `docs/design/vidpn-flip.md` section 9 states.

## Runs

| Run | Build | What | Result |
|---|---|---|---|
| 001 | 0.7.19 (d1b7651) | dump twice, EnableMmio only, display-only | all 75 registers read, values = Linux firmware state, frame count moving. `evidence/windows/2026-09-22-E22-dcn-read-run-001/` |
| 002 | 0.7.22 (b197b2b) | as planned, plus `fbdump` while HUBP0 pointed at the fill and after the restore; owner absent | H4 confirmed (address unchanged, pending cleared in 16 ms, frame count on); H5 confirmed at the registers and by the scanout read-back (blue field, border, diagonal), the monitor unobserved; restore OK, desktop back. M94. `evidence/windows/2026-09-22-E22-dcn-flip-run-002/` |

Run plan for 002 (a script like
`evidence/windows/2026-09-22-E22-dcn-read-run-001/run-001-script.ps1`, extended):

```
Set-ItemProperty $params -Name EnableMmio -Value 1 -Type DWord
Set-ItemProperty $params -Name EnableDcnWrite -Value 1 -Type DWord
Set-ItemProperty $params -Name EnableVram -Value 1 -Type DWord
Set-ItemProperty $params -Name EnableVramWrite -Value 1 -Type DWord
# disable/enable the device, as run 001 did, then:
& $cli dcn                                              # baseline: firmware state, as M92
& $cli dcnflip <firmware address from the dump above>   # H4: no-op flip
& $cli dcnflip 0x271000000 fill 0xFF2060C0               # H5: owner should see a blue field, white border, on the monitor
& $cli dcnflip restore                                  # back to the firmware's address
& $cli dcn                                              # confirm: ADDRESS/_HIGH, INUSE back to the firmware values
Set-ItemProperty $params -Name EnableDcnWrite -Value 0 -Type DWord
Set-ItemProperty $params -Name EnableMmio -Value 0 -Type DWord
```

Expected register values after each step (M85-M87, M92): after the no-op flip, `HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS`/`_HIGH`
unchanged (`0x70000000`/`0x2`), `SURFACE_FLIP_PENDING` cleared, `OTG_STATUS_FRAME_COUNT` higher than before. After
the fill flip, the address registers hold `0x71000000`/`0x2` (`0x271000000` split), `DCSURF_SURFACE_INUSE` catches
up to it once the flip lands, `HUBP_UNDERFLOW_STATUS` (`DCHUBP_CNTL` bits `0x70000000`) clear. After `restore`, the
address registers are back to `0x70000000`/`0x2`.
