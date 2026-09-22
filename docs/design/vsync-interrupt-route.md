# OTG0 VUPDATE_NO_LOCK: the route from hardware event to this driver, and what is missing

Written 2026-09-22 against bc250kmd 0.7.24 (no version bump), in answer to M98/M99: the interrupt this driver
arms (`OTG0_OTG_GLOBAL_SYNC_STATUS` bit 12) never arrives, with 0 interrupts and 0 vectors taken over 40 s of
desktop in E22 run 004, even with gart/psp/gfx/ih all up and the same MSI vector known to deliver GFX/CP
interrupts (M77). Every claim below carries a `file:line` or an evidence path; a line is marked **PROVEN**
when it comes from reading `ref/linux-src` or this repo's own source and matches a measured value, and
**OPEN** when it can only be settled by a lab run. Style follows `docs/design/vidpn-flip.md`.

## 1. The route amdgpu takes on DCN 2.0.1 (PROVEN, read end to end)

1. `amdgpu_dm_crtc.c:amdgpu_dm_crtc_set_vblank` (`ref/linux-src/.../amdgpu_dm/amdgpu_dm_crtc.c:330-355`): on this
   part `amdgpu_ip_version(adev, DCE_HWIP, 0) != 0`, so enabling DRM vblank takes `adev->vupdate_irq` through
   `amdgpu_irq_get`/`put` - a **software reference count**, not itself a register write.
2. `amdgpu_irq_get`/`put` call `amdgpu_dm_irq.c:dm_irq_state` (`amdgpu_dm_irq.c:690-849`), which calls
   `dc_interrupt_set(adev->dm.dc, irq_source, st)` - DC's own enable entry point.
3. Registration (software only, `amdgpu_dm.c` around line 4687): `amdgpu_irq_add_id(adev, SOC15_IH_CLIENTID_DCE,
   i, &adev->vupdate_irq)` builds the driver's own client_id/src_id -> handler dispatch table. It performs no
   MMIO. `SOC15_IH_CLIENTID_DCE = 0x04` (`ref/linux-src/.../include/soc15_ih_clientid.h:37`).
4. DC's own table, `dc/irq/dcn20/irq_service_dcn20.c` (read in full, 386 lines): the `vupdate_no_lock_int_entry`
   macro expands to `IRQ_REG_ENTRY(OTG, reg_num, OTG_GLOBAL_SYNC_STATUS, VUPDATE_NO_LOCK_INT_EN,
   OTG_GLOBAL_SYNC_STATUS, VUPDATE_NO_LOCK_EVENT_CLEAR)` with `.funcs = &vupdate_no_lock_irq_info_funcs`, whose
   `.set` and `.ack` are both `NULL`. A `NULL` here means DC's generic path does the whole job with one register
   (`dc/irq/irq_service.c:dal_irq_service_set`): read-modify-write the enable bit, no per-source callback needed.
5. `dal_irq_service_set()` (`irq_service.c`, fetched via `git show HEAD:...`, not materialized locally) calls
   `dal_irq_service_ack()` first, unconditionally, then `dal_irq_service_set_generic()` - **two** separate RMWs of
   the same register, not one combined write. Functionally equivalent to one combined write for a status
   register whose ack field is write-1-to-clear; not a second register, not a missing step.
6. Hardware: OTG0's timing generator raises `OTG_GLOBAL_SYNC_STATUS` bit 14 (`VUPDATE_NO_LOCK_EVENT_OCCURRED`)
   once a frame; if bit 12 (`VUPDATE_NO_LOCK_INT_EN`) is set this also raises IH client 4, source `0x57`
   (`DCN_1_0__SRCID__OTG0_IHC_V_UPDATE_NO_LOCK_INTERRUPT`, facts M88).
7. The IH ring: **on this IP block there is only one ring**. `navi10_ih.c:sw_init` (read in full, 730 lines)
   sets `adev->irq.ih1.ring_size = 0` and `adev->irq.ih2.ring_size = 0` explicitly
   (`ref/linux-src/.../navi10_ih.c:580-581`), and `navi10_ih_irq_init`'s bring-up loop
   (`navi10_ih.c:352-358`) only calls `navi10_ih_enable_ring()` for a ring whose `ring_size` is nonzero - so
   ring 0 is the only one ever enabled on this part. There is no per-source ring assignment table a DCE source
   could be routed to instead of ring 0: GFX/CP's end-of-pipe interrupts and DCE's VUPDATE_NO_LOCK interrupt,
   if it fires, share the exact same ring and the exact same MSI vector.
8. Doorbell/MSI, then the OS's ISR, then DRM's own client_id/src_id dispatch table (built at step 3) calls
   `amdgpu_dm_irq_handler` -> `dm_vupdate_high_irq`, which completes the pending flip once
   `dc_get_flip_pending_on_otg` says none is pending (M88's read counts: 2158 reads of
   `HUBPREQ0_DCSURF_FLIP_CONTROL`, 360 seen pending).

## 2. Is there a second, IH-side enable/credit register? No (PROVEN)

This was the task's central question, checked three ways:

- **DC's own table already says no**: `.set = NULL` on `vupdate_no_lock_irq_info_funcs` (section 1 point 4) is
  DC's way of saying the generic single-register RMW *is* the entire enable; a source with a real second step
  (a `.set` callback) looks different in this same file - compare `hpd1_int_entry`, whose `.funcs` do have a
  `.set`. VUPDATE_NO_LOCK gets the plain path because it does not need one.
- **No per-client IH register exists on this IP block**: `navi10_ih.c` was read in full (730 lines) with a
  targeted grep for `IH_CLIENT_CFG`, `client_id` routing and credit fields - zero matches in this file. A
  repository-wide grep for `IH_CLIENT_CFG` finds it in exactly one place, `psp_v3_1.c`, which is the PSP
  command ring's own unrelated config register, not an IH per-client gate. Team-lead's hypothesis
  (`IH_CLIENT_CFG_DATA`/`INDEX`) does not apply to this IP version; it does not exist here to be missing.
- **NBIO's `ih_control` is not per-client either**: `nbio_v2_3_ih_control()` (`ref/linux-src/.../nbio_v2_3.c:206-225`,
  read in full) writes exactly two registers - `INTERRUPT_CNTL2` (the dummy-read page address) and
  `INTERRUPT_CNTL` (dummy-read-override and non-snoop enable bits) - plus `ih_doorbell_range()`
  (`nbio_v2_3.c:186-203`) sets `BIF_IH_DOORBELL_RANGE`'s OFFSET/SIZE fields, the doorbell aperture for ring 0.
  Nothing here names a client, a source, or DCE specifically. Our shim's `bc250_nbio_ih_control` and
  `bc250_nbio_ih_doorbell_range` (`driver/shim/bc250_nbio.c:94-130`) write the same two registers with the same
  fields, and the doorbell-range write is checked against a real hardware trace in the shim's own comment
  (`bc250_nbio.c:108-113`: `W NBIO.BIF_IH_DOORBELL_RANGE 0x03bc8 00020BC0`, decoded and matched).
- `amdgpu_irq_add_id` (section 1 point 3) is pure software dispatch-table registration with no MMIO effect -
  there is nothing here for `ih.c` to replicate in hardware, and it does not gate whether the hardware event
  reaches the ring.

**Conclusion**: the enable is genuinely the single register this driver already writes. No second register,
named or unnamed, was skipped.

## 3. What this driver does today (PROVEN, current source)

- `driver/kmd/dcn.c:567-579` `DcnVsyncEnable`: one RMW of `OTG0_OTG_GLOBAL_SYNC_STATUS` - sets or clears
  `VUPDATE_NO_LOCK_INT_EN`, always ORs in `VUPDATE_NO_LOCK_EVENT_CLEAR`. Matches section 1 point 5's *net*
  effect exactly (the ack-then-set split is not reproduced, and does not need to be - see section 1 point 5).
- `driver/kmd/dcn.c:588-...` `DcnVsyncInterrupt`: called directly by the ISR on **every** interrupt this driver's
  MSI message delivers (see next bullet), not by decoding an IH ring entry. It polls
  `OTG0_OTG_GLOBAL_SYNC_STATUS` itself, checks `VUPDATE_NO_LOCK_EVENT_OCCURRED`, acks if set. This is a valid
  simplification specifically because of section 1 point 7: one ring, one MSI message, no per-client hardware
  gate to decode - so "was it ours" collapses to "is the status bit set", the same question the IH ring's own
  entry would have answered.
- `driver/kmd/pnp.c:186-196` `Bc250InterruptRoutine`: `InterlockedIncrement(&device->InterruptCount)` happens
  **first, unconditionally**, before either `IhInterrupt(device)` or `DcnVsyncInterrupt(device)` runs; both
  always run, regardless of what the other found (`pnp.c:184-185`'s own comment). This matters for reading
  E22 run 004's log: "interrupt routine called 0 times" means `Bc250InterruptRoutine` itself was never invoked
  by the OS - not that it ran and `DcnVsyncInterrupt` found the status bit clear. Windows/the hardware never
  called into this driver at all during the 40 s window.
- `driver/kmd/ih.c` / `driver/shim/bc250_ih.c`: a faithful transcription of `navi10_ih_irq_init` (disable,
  `nbio_ih_control`, enable ring 0, `nbio_ih_doorbell_range`, `pci_set_master` equivalent, enable). E22 run 004
  itself shows this bring-up succeeding: "ih init: done... 21 register writes executed", "ring ENABLED",
  MSI vector `0x70`. Using the same code, GFX/CP end-of-pipe interrupts are reliably delivered on this same
  vector (facts M77, E19 run 005) - proving the ring/MSI/ISR plumbing itself works; the gap is specific to the
  DCN source, not the delivery mechanism.

## 4. Cross-check against measured registers (PROVEN where cited)

| Register | Linux (source) | This driver | Verdict |
|---|---|---|---|
| `IH_RB_CNTL` | idle/running `0x403301A1` (facts M40; `evidence/linux/2026-09-22-E21-linux-reference-4/regs-state.txt:37`, a second independent Linux boot agreeing with M40 exactly) | derived field by field, `driver/shim/bc250_ih.c:66-75,103-105,128`: `MC_SPACE=4`, `WPTR_OVERFLOW_ENABLE=1`, `WPTR_WRITEBACK_ENABLE=1`, `MC_SNOOP=1`, `MC_RO=0`, `MC_VMID=0`, `RB_ENABLE=1`, `RB_GPU_TS_ENABLE=1`, `ENABLE_INTR=1`, `RPTR_REARM=1` (MSI in use) | Every field decoded from `0x403301A1` against `osssys_5_0_0_sh_mask.h` matches our own derivation. Only `RB_SIZE` differs - a ring-size choice, not a bug. |
| `IH_CNTL` | `0x01000000` (`regs-state.txt:45`) | not written by `navi10_ih.c` or our shim (a performance-tuning register: `WPTR_WRITEBACK_TIMER`, `IH_FIFO_HIGHWATER`) | Untouched by both; matches by construction. |
| `OTG0_OTG_GLOBAL_SYNC_STATUS` | firmware/idle `0x00104104`, no interrupt enabled (M92) | armed to `0x00115104` (E22 run 004 log line 38: `write 0x14128 = 0x00115104`) | **Not present in any of E21's static captures** - grepped `dmupre.txt`, `dmupost.txt`, `dmu/dmu-pre.log`, `dmu/dmu-post.txt`, `sweep-pre.log`, `regs-state.txt`: zero matches for `GLOBAL_SYNC_STATUS` in all of them. The only Linux values on record for this register come from the flip register-write **trace**, not a sweep (M87: armed `0x00115104`; M88: acked `0x0011D104`, 360 times in 6 s). A DMU sweep captured under a genuinely idle, un-flipping Linux desktop was never taken, so there is no Linux reference for what this register reads while idle with vupdate armed - only while actively flipping. |
| `OTG0_OTG_DOUBLE_BUFFER_CONTROL` | differs pre/post amdgpu load: `0x00010000` -> `0x02010000` (`dmu/dmu-pre.log:57`, `dmu/dmu-post.txt:57`) | not touched by this driver | Looked like a candidate second register controlling vupdate delivery; ruled out by reading `dcn_2_0_1_sh_mask.h:14535`: the changed bits (`0x02000000` of the `0x03000000` mask) are `OTG_RANGE_TIMING_DBUF_UPDATE_MODE`, a VRR/range-timing field, unrelated to `VUPDATE_NO_LOCK`. |
| `OTG0_OTG_GLOBAL_CONTROL0` / `OTG0_OTG_VUPDATE_KEEPOUT` / `HUBPREQn_DCSURF_FLIP_CONTROL2` | amdgpu writes `0`, `0`, `0x440` (M87) | left alone (`gen_regs.py:16-22`'s comment) | **Already established by the team before this pass**, re-checked here, not a new finding: M87's own values equal what the firmware already has in these three registers (`gen_regs.py`'s comment, `experiments/E22-dcn-flip/README.md:33-34`), so omitting the writes is provably value-neutral. Not the missing piece. |

`OTG0_OTG_VUPDATE_KEEPOUT` (checked via `dcn20_optc.c:optc2_triplebuffer_lock/unlock`, read via `git show`) sets
and clears `OTG_MASTER_UPDATE_LOCK_VUPDATE_KEEPOUT_EN` around a lock/unlock pair - a race-protection window
against the CRTC's active/blank boundary, not an interrupt gate; confirms it is unrelated to section 2's
question by a second, independent path.

## 5. What remains open (lab-only, not settled by reading)

1. **Whether the hardware event itself ever latches during the failing window.** `OTG0_OTG_GLOBAL_SYNC_STATUS`
   bit 14 (`VUPDATE_NO_LOCK_EVENT_OCCURRED`) is the one field that can separate "the event never fires under
   this engine combination" from "it fires but nothing between OTG0 and this driver's ISR ever reports it" -
   M94 proved the OTG latch/pending-clear mechanism works, but only under **display-only** gates (no
   gart/psp/gfx/ih); E22 run 004's failing case had gart+psp+gfx+ih all active, a combination never checked
   against this specific bit before. This is the single most decisive next measurement, and section 6 adds
   exactly the read needed for it.
2. **Whether `Bc250InterruptRoutine` truly never ran, at the OS/hardware level, for a reason upstream of OTG0
   itself** - `InterruptCount == 0` (section 3) is consistent with "OTG0 never raised its line" but not, by
   itself, exclusive proof of it. Made unlikely, not impossible, by GFX/CP interrupts arriving reliably on the
   identical MSI vector in the same code path (M77) - if OS/vector dispatch were broken generally, GFX would
   show it too. Not resolvable statically; needs point 1's answer first, since a `1` there would make this
   question moot (an OS/vector-level problem) and a `0` would make it the answer (a DCN/DMU-side problem, not
   IH/MSI at all).

## 6. What this pass changed (diagnostic only, no behavior change)

Everything below only adds reads and log/printf detail; no write sequence, gate, or refusal condition changed.

- `driver/kmd/gen_regs.py`: added `("DMU", "mmOTG0_OTG_VUPDATE_KEEPOUT")` to `NAMED` (already on the 75-register
  `DCN_REGISTERS` allow list, so no allow-list change was needed); regenerated `regs.generated.h` with the
  script (never hand-edited) - `BC250_REG_DMU_OTG0_OTG_VUPDATE_KEEPOUT = 0x1413Cul`.
- `driver/kmd/bc250kmd_escape.h`: four new `BC250_ESCAPE_DCN` fields after `Otg0VblankIntEnabled` -
  `Otg0VupdateEventOccurred` (bit 14 of `GLOBAL_SYNC_STATUS`, section 5 point 1's answer), `Otg0MasterUpdateLocked`,
  `Hubp0FlipPending`, `Otg0VupdateKeepoutEn` - each documented with what a nonzero reading would mean.
- `driver/kmd/dcn.c`: `DcnEscape` (now `dcn.c:17-117`) reads `OTG0_OTG_MASTER_UPDATE_LOCK`,
  `HUBPREQ0_DCSURF_FLIP_CONTROL` and `OTG0_OTG_VUPDATE_KEEPOUT` (three more `MmioDcnRead` calls, all on
  offsets the escape's own 75-register loop already proved safe), decodes the four fields (the event-occurred
  bit reuses the `OTG0_OTG_GLOBAL_SYNC_STATUS` value already read for `Otg0VblankIntEnabled`, no extra read),
  and extends the `GuardLog` summary line with all four.
- `tools/win/bc250kmd_cli/bc250kmd_cli.c`: `Dcn()` prints the four new fields, so the next lab run's console
  output shows them directly.
- `driver/kmd/gart.c`: unrelated to the interrupt route - see section 7.

## 7. Side question: `gart restore` returned exit code 3 in E22 run 004

`bc250kmd_cli`'s exit code 3 is `BC250_ESCAPE_STATUS_REFUSED` (`bc250kmd_cli.c:489`,
`return g.Status == BC250_ESCAPE_STATUS_DONE ? 0 : 3;`). `GartEscape` (`driver/kmd/gart.c:230-234`) refuses
every GART op, restore included, with `STATUS_INVALID_DEVICE_STATE` while `GfxIsActive(Device)` or
`IhIsActive(Device)` is true, with the reasoning already in its comment: "Every GART command sets the shim's
device up afresh, and gfx.c keeps its state in that device; a restore would also take the GART away from under
mapped queues. amdgpu's order: the engines go first (gfx.c's FINI)."

E22 run 004's undo ran `psp unload` (exit 0), then `gart restore` (exit 3), then the gate closed - with no
`ih fini` in between. The IH ring was therefore still active when `gart restore` ran, so the refusal is
correct and deliberate: GART was left in **this driver's own mapped state** (`gart->Enabled` stays `TRUE`),
not restored to the firmware's, which is the intended, safe consequence of refusing the operation rather than
running it against a device gfx.c/ih.c still consider live. This mirrors amdgpu's own teardown order (engines
and IH before GART) rather than deviating from it.

**Verdict: not a defect.** The only change made (`driver/kmd/gart.c`) is diagnostic: the refusal now names
which precondition triggered it (`"gfx active (gfx fini needed first)"` or `"ih active (ih fini needed first)"`)
in the `GuardLog` line, so a REFUSED exit code has an answer without cross-referencing `GfxIsActive`/`IhIsActive`
by hand. The refusal's condition and behavior are unchanged.

## 8. Next lab run

Run E22 run 004's steps again (gate `-Full 1 -Engines 1 -Blit 1 -VidPnFlip 1`, gart enable, psp load, ih init,
arm the hardware vsync), then `bc250kmd_cli dcn` **before** the 40 s wait and again **after** it, and diff the
four new fields (the read is non-destructive: `OTG0_OTG_GLOBAL_SYNC_STATUS`'s event bit is write-1-to-clear via
a separate field, section 1 point 4, so a plain read cannot itself clear or mask what it is measuring):

- **`Otg0VupdateEventOccurred` reads `1` after the wait**: the hardware event does fire; the gap is entirely
  between OTG0 and this driver's ISR (section 5 point 2 becomes the live question - worth a `bc250kmd_cli ih`
  read at the same two points to see if the ring's own wptr moved at all even without an ISR call).
- **`Otg0VupdateEventOccurred` reads `0`**: the event never fires under this engine combination; something in
  gart/psp/gfx bring-up (all untouched by section 1-4's reasoning, which covers only the DCN/IH side) disturbs
  OTG0's ability to raise it - not yet measured, not yet even narrowed to a candidate register, since gart/psp/gfx
  write GC/MMHUB/MP0/MP1, never DMU (`gen_regs.py`'s own IP separation). `Otg0MasterUpdateLocked` and
  `Hubp0FlipPending` staying clear both before and after would at least rule out a stuck double-buffer lock as
  the cause.
