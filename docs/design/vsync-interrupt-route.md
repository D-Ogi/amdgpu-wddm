# OTG0 VUPDATE_NO_LOCK: the route from hardware event to this driver, and what is missing (resolved: nothing)

Written 2026-09-22 against bc250kmd 0.7.24 (no version bump), in answer to M98/M99: the interrupt this driver
arms (`OTG0_OTG_GLOBAL_SYNC_STATUS` bit 12) never arrives, with 0 interrupts and 0 vectors taken over 40 s of
desktop in E22 run 004, even with gart/psp/gfx/ih all up and the same MSI vector known to deliver GFX/CP
interrupts (M77). Every claim below carries a `file:line` or an evidence path; a line is marked **PROVEN**
when it comes from reading `ref/linux-src` or this repo's own source and matches a measured value, and
**OPEN** when it can only be settled by a lab run. Style follows `docs/design/vidpn-flip.md`.

**Update, same day.** This pass built against 0.7.25 and, without any version-changing edit of its own, picked
up 0.7.26 in the working tree: a concurrent agent's fix for the unrelated stack-overflow bugcheck of M104/M105
(`tools/win/stackbudget.py`, now part of the standard build). Section 5's two open points are
now both closed. Point 1 (does the hardware event latch under the failing engine combination) was answered by
M103: yes, in all four dumps of E24 run 003, with the engines fully up. Point 2 (does anything at all reach the
IH ring in that kind of device start) looked answered the same way until M103's own text flagged the gap this
document's section 5 point 1 had already named as untested: E24 run 003 submitted no GPU work, so it never
proved the ring was consuming anything in that device start, CP fences included. E24 run 004 ran the positive
control section 5 point 2 asked for - a CP fence, in the same device start, right after the same eight-stage
bring-up - and it is unambiguous (M106): `ih state` went from 0 interrupts/0 vectors/rptr 0x0 wptr 0x0 to 202
interrupts taken as ours, 203 vectors consumed, rptr = wptr = 0x1960, and the last two vectors decoded are
`client 4 source 87` - the exact VUPDATE_NO_LOCK identity of M88, not the fence's own `client 20 source 181`
(M79). Sections 9-13 below add the mechanism-level answers the team asked for on top of this (every register
and firmware question, checked against what is actually imported into this repo), which corroborate M106
without being needed to explain it: nothing found anywhere in the register or firmware path was ever capable of
blocking this delivery, and now there is a direct measurement saying it does not.

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
   **RESOLVED by M103**: it latches, continuously, in all four dumps of E24 run 003 (engines fully up),
   `OTG0_OTG_GLOBAL_SYNC_STATUS` = `0x0010D104` throughout, frame counter advancing. The event fires under the
   full engine combination exactly as it does display-only. This branch of the investigation is closed.
2. **Whether `Bc250InterruptRoutine` truly never ran, at the OS/hardware level, for a reason upstream of OTG0
   itself** - `InterruptCount == 0` (section 3) is consistent with "OTG0 never raised its line" but not, by
   itself, exclusive proof of it. Made unlikely, not impossible, by GFX/CP interrupts arriving reliably on the
   identical MSI vector in the same code path (M77) - if OS/vector dispatch were broken generally, GFX would
   show it too. Not resolvable statically; needs point 1's answer first, since a `1` there would make this
   question moot (an OS/vector-level problem) and a `0` would make it the answer (a DCN/DMU-side problem, not
   IH/MSI at all).
   **RESOLVED by M106**: point 1 came back `1` (latches), which made this the live question, exactly as
   predicted - and E24 run 003 itself could not answer it, because (its own README, and its own text in facts.md)
   it "submitted no GPU work at all", so a `0` interrupt count there was equally consistent with "OTG0 reaches
   the ISR and nothing else happened to trigger it" and "OTG0 never reaches the ISR". E24 run 004 supplied the
   missing positive control (a CP fence, in the same device start, right after the same eight stages) and
   `Bc250InterruptRoutine` went from called 0 times to called 202 times in the same run, with the ring's own
   decode identifying the traffic as `client 4 source 87` (M88's VUPDATE_NO_LOCK identity) on both of the last
   two vectors printed. `Bc250InterruptRoutine` does run for this source. Both branches of this investigation
   are closed, and closed the same way: not a driver defect, a missing positive control in the two runs (E22 run
   004, E24 run 003) that had reported 0.

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

This section originally proposed re-running E22 run 004 with a before/after diff of the four new DCN fields.
E24 run 004 ran a strictly stronger version of that plan (same idea, plus the CP fence positive control section
5 point 2 needed) and answered both of this section's branches at once: **RESOLVED, see section 5.** Nothing
below is still open as a way to explain a 0-interrupt reading; a 0 from here on means "nothing was raised in
this window", not "the route is broken", unless a future run manages to reproduce 0 immediately after a fence
that itself reads back success - which is now the actual control to reach for if this ever regresses.

What is left, now that "does it arrive" is closed, is "what happens to it once it does" - not this document's
question (the team asked about the route to the IH ring, section 1-4 and 9-12 below), but the natural next
step: today, `ih.c`'s DPC decodes a `client 4 source 87` vector exactly like any other source (`Note()`,
`driver/kmd/ih.c:104-130` - tally by kind, keep it in the `Last[]` ring, nothing dispatched on the source id).
Turning that arrival into a flip-complete/vsync notification to dxgkrnl is present-path work in
`driver/kmd/wddm.c`, which another agent is already changing this round; not attempted here.

The diagnostic fields already added (section 6, `Otg0VupdateEventOccurred`/`Otg0MasterUpdateLocked`/
`Hubp0FlipPending`/`Otg0VupdateKeepoutEn`, plus section 13's `IH_STATUS` fields below) stay in the escape either
way - they answer "did the DCE side see it" and "did the IH side see it" independently for any future interrupt
source, which is a cheaper first move than a positive-control run whenever a *different* source looks silent.

## 9. Q1: the hardware path and every register on it (PROVEN, read against files actually imported here)

Sections 1 and 2 already establish this end to end; this section adds the two things Q1 asked for that they did
not already cover by name.

**The full register list, start to finish, and nothing else on it:**

| Register | IP | Role |
|---|---|---|
| `OTG0_OTG_GLOBAL_SYNC_STATUS` | DMU | arm (`VUPDATE_NO_LOCK_INT_EN`, bit 12), ack (`_EVENT_CLEAR`, bit 16), status (`_EVENT_OCCURRED` bit 14, `_INT_STATUS` bit 15, section 12) - one register does all four jobs (section 1 point 4-5) |
| `IH_RB_CNTL` | OSSSYS | ring mode, MSI vs INTx, write-back (section 4 table; ring-wide, not per-source) |
| `IH_RB_WPTR`, `IH_RB_RPTR` | OSSSYS | ring pointers (ring-wide) |
| `IH_STATUS` | OSSSYS | read-only, ring-wide idle/pending view (section 13, added this round) |
| `INTERRUPT_CNTL`, `INTERRUPT_CNTL2` | NBIO/BIF | dummy-read control (ring-wide, `nbio_v2_3.c:206-225`, section 2) |
| `BIF_IH_DOORBELL_RANGE` | NBIO/BIF | doorbell aperture for ring 0 (ring-wide, `nbio_v2_3.c:186-203`, section 2) |

Nothing else is read or written anywhere in `ref/linux-src`'s traced path from OTG0's timing generator to the
ISR. Every register past `OTG0_OTG_GLOBAL_SYNC_STATUS` is **ring-wide**: none of them carry a client id, a
source id, or any other way to single out DCE - confirmed again this round (below) and already established in
section 2.

**The aggregator search Q1 asked for, done directly against the header we actually import**: grepped
`third_party\linux-amdgpu\dcn_2_0_1_offset.h` (the offset header `gen_regs.py` generates `BC250_REG_DMU_*` from)
for `DISP_INTERRUPT_STATUS`, `DMU_INTERRUPT`, `_IHC_` and `DC_IRQ` - zero matches, all four. "IHC" only ever
appears as part of the interrupt's *source name* (`DCN_1_0__SRCID__OTG0_IHC_V_UPDATE_NO_LOCK_INTERRUPT`, M88) -
a label on the wire, not a register. There is no DMU/DCE interrupt aggregator on this IP block for this offset
header to have a register for.

**One correction to how section 1-4 were reasoned about, caught while re-checking this round's own added claims
against files that actually exist in this repo**: an earlier pass (of this same round, before this document was
updated) reasoned that Van Gogh's real DC resource-pool dispatch is family-keyed to a `dcn301_resource.c`/
`irq_service_dcn30.c` pair, overriding the `irq_service_dcn20.c` citation sections 1 and 2 already use. That
does not check out: `ref/linux-src` under `dc/` contains exactly two irq-service files, `dc/irq/dcn20/` and
`dc/irq/dcn201/` (confirmed by listing the tree), no `dc/resource/` directory at all, and no `FAMILY_VGH` match
anywhere in `amdgpu_dm.c`. Whatever real upstream Linux does for a part identifying that way, this repository
has not imported the file that would prove it, so it cannot be cited from here. Sections 1-4's original
`irq_service_dcn20.c` citation stands, on the same basis section 6/ADR 0011 already use for the register header
of the same name (dcn20/dcn201 are what is imported; treat them as the closest available, not a proven exact
stepping match - see section 10 for why this uncertainty does not reach the conclusion).

## 10. Q2: does DMCUB matter here (PROVEN, and precisely measured this round)

**This unit's actual DCE_HWIP discovery value, measured directly, not assumed**: `hw_id 271` (`DMU_HWID`, per
`ref/linux-src/.../soc15_hw_ip.h:38`) reads `major 2, minor 0, revision 3` -
`evidence/linux/2026-09-22-E21-linux-reference-4/ip_discovery.txt:113-120`. `IP_VERSION(2, 0, 3)`. This is the
first time this exact tuple has been pulled from a debugfs dump with a citation rather than paraphrased as
"DCN 2.0.1" (M88's own prose, and this document's section 1); it is worth a fact of its own (docs/facts.md is
not mine to edit - flagged to team-lead).

This number is exactly what settles Q2, because it is the same value `dm_init_microcode()`
(`ref/linux-src/.../amdgpu_dm.c:5756-5815`, read in full) switches on to decide whether to request DMUB firmware
at all. Its case list: `(2,1,0)`, `(3,0,0)`, `(3,0,1)` (Van Gogh's own textbook identity - `FIRMWARE_VANGOGH_DMUB`,
line 5773-5775, worth naming because it is the reason this needed checking rather than assuming), `(3,0,2)`,
`(3,0,3)`, `(3,1,2)`, `(3,1,3)`, `(3,1,4)`, `(3,1,5)`, `(3,1,6)`, `(3,2,0)`, `(3,2,1)`, `(3,5,0)`, `(3,5,1)`,
`(3,6,0)`, `(4,0,1)`. `(2,0,3)` - what this unit actually reports - is not among them:

```c
default:
    /* ASIC doesn't support DMUB. */
    return 0;
```

No DMUB firmware is requested for this unit, independent of what a datasheet identity would suggest. This
matches the PSP load list measured on unit A: M34 and this round's own `run_psp.ps1` host replay both show
exactly ten `LOAD_IP_FW` commands (SDMA0, SDMA1, CP_CE, CP_PFP, CP_ME, CP_MEC1+JT, CP_MEC2+JT, RLC_G) and no
DMCUB/DMCU entry. Even if it were loaded, its IH registration is structurally separate from vupdate's: DMUB's
outbox interrupt is added under `DCN_1_0__SRCID__DMCUB_OUTBOX_LOW_PRIORITY_READY_INT`
(`amdgpu_dm.c:4738-4739`, `register_outbox_irq_handlers()`), a different `amdgpu_irq_add_id` call and a
different source id than `vupdate_irq`'s (`amdgpu_dm.c:4687`, section 1 point 3) - the two could not interact
even if both existed. **DMCUB does not matter here, on two independent grounds.**

## 11. Q3: could the interrupt *output* be gated separately from the source bit (PROVEN)

Grepped `third_party\linux-amdgpu\dcn_2_0_1_offset.h` (the header this project's own tooling generates DMU
offsets from) for `DOMAIN`, `DC_MEM_PWR` and `DMU_CLK` - zero matches, all three. There is no power-domain-gate
or clock-gate register concept exposed for this IP block in the header we import at all; not "not written by
this driver", not present as a name to write.

This is not an assumption standing in for a measurement: M92 already shows the block powered and clocked under
this driver's own display-only gate (`OTG0_OTG_STATUS_FRAME_COUNT` advancing between two reads a second apart),
and M103 shows the same thing holds with gart/psp/gfx/ih all up (frame counter `0x41CB` to `0x5FF5` across the
four dumps quoted in the team's brief). `OTG0_OTG_VUPDATE_KEEPOUT` was checked separately in section 4's table
and by a second path via `dcn20_optc.c`'s triplebuffer lock/unlock - a race-protection window, not a gate.
Nothing in this driver's bring-up gates the DMU's interrupt output, because there is nothing here that could.

## 12. The empirical reading of VUPDATE_NO_LOCK_INT_STATUS (bit 15)

Not asked directly, but load-bearing for reading M103 correctly, and now exposed in the escape (section 13).
Amdgpu's own source never reads this bit - `irq_service_dcn20.c`'s generic path (section 1 point 4-5) only ever
writes `OTG_GLOBAL_SYNC_STATUS`, so there is no source-code statement of what `INT_STATUS` means. Two measured
values are the only evidence on record:

- M92 (display-only, arming closed): `0x00104104` - bits 2, 8, 14, 20 set (the four `EVENT_OCCURRED`s, all
  free-running per section above), bits 12 and 15 clear.
- M103 (full engine set, arming open): `0x0010D104` - bits 2, 8, 12, 14, 15, 20 set.

Bit 15 is the only bit besides the enable bit itself (12) that differs between the two, and it moved with 12,
not with 14 (which was already set in both). Reading: `INT_STATUS` is `EVENT_OCCURRED` qualified by `INT_EN` -
"this occurrence is armed to actually raise the interrupt" - not a second copy of `EVENT_OCCURRED`. This is
consistent with, and gives an independent register-level reason to expect, M106's result: bit 15 already read
`SET` in M103, before E24 run 004's fence ever ran, meaning the DCE side had already done its part; E24 run 004
only had to prove the IH side was listening.

## 13. Q4: the diagnostic reads added this round, and what each one settles

Everything below is diagnostic only: reads and log/printf detail, no write sequence, gate, or refusal condition
changed. Continues section 6's list; nothing in section 6 was touched again.

- **`driver/kmd/gen_regs.py`**: `mmIH_STATUS` (OSSSYS) added to `NAMED` and to the `Ih` sequence's read-only
  allow list (a `navi10_ih_irq_init` bring-up trace never reads it, so no trace window would ever have caught
  it - the same "not traced, added with a reason" convention the `Gfx` sequence already uses for
  `CP_HQD_DEQUEUE_REQUEST`). Regenerated `regs.generated.h` with the script, never by hand.
  `BC250_REG_OSSSYS_IH_STATUS = 0x04588ul`, matching `evidence/.../regs-state.txt:46` and
  `evidence/.../sweep-pre.log:5398` exactly.
- **`driver/kmd/bc250kmd_escape.h`**: three new `BC250_ESCAPE_IH` fields - `Idle`, `InputIdle`,
  `BifInterruptLine` (`IH_STATUS` bits 0, 1, 10) - and one new `BC250_ESCAPE_DCN` field,
  `Otg0VupdateIntStatus` (section 12).
- **`driver/kmd/ih.c`**: `IhEscape` reads `IH_STATUS` best-effort (0 with `EnableIh` closed, same convention as
  every other field in this escape) and decodes the three bits; `GuardLog` extended.
- **`driver/kmd/dcn.c`**: `Otg0VupdateIntStatus` decoded from the `syncStatus` read section 6 already added -
  no extra `MmioDcnRead` call.
- **`tools/win/bc250kmd_cli/bc250kmd_cli.c`**: `ih` and `dcn` both print the new fields.

Mapped against what Q4 actually asked for:

| Q4 asked for | Where it already was, or is now |
|---|---|
| `IH_RB_WPTR` read through the escape | Already there before this round (`Data->Wptr`, `BC250_ESCAPE_IH`) |
| "the IH status registers" | New this round: `IH_STATUS`'s `Idle`/`InputIdle`/`BifInterruptLine` |
| "whatever DCE-side status register would show a pending outbound interrupt" | Already there (`Otg0VupdateEventOccurred`, section 6) plus this round's `Otg0VupdateIntStatus` (section 12) for the qualified/armed reading |

As it turned out, M106 answered the question this round's work was aimed at without needing any of these -
`ih state`'s existing vector decode (client/source per entry) was already enough once a positive control was
run. They remain useful for the next time a *different* source looks silent and a lab run is expensive: reading
`Otg0VupdateEventOccurred`/`Otg0VupdateIntStatus` and `IH_STATUS.InputIdle`/`BifInterruptLine` at the same two
moments distinguishes "nothing was raised" from "raised but not queued" from "queued but not delivered" without
needing a fence-based positive control every time - a ranked, cheapest-first order for that future situation:

1. **`Otg0VupdateEventOccurred`** (or the equivalent latch bit for whatever source is silent): did the hardware
   event happen at all. A `0` ends the search on the DCE/source side.
2. **`IH_STATUS.InputIdle`**: does IH's own input see anything pending right now. Momentary by nature (a DIRQL
   race), so a single read is weak evidence; useful mainly as a sanity check alongside 1 and 3.
3. **`IH_STATUS.BifInterruptLine`**: is the host interrupt line asserted at NBIO/BIF's own view, right now -
   the closest thing to "is a vector waiting to be picked up by the OS" this register set has.
4. **`ih state`'s rptr/wptr and vector count**, before and after: did anything land in the ring. This is what
   M106 actually used, and - now that section 5 is closed - is very likely sufficient on its own; reach for 1-3
   only if this stays at 0 and the source's own latch bit (1) already reads set, which is the specific
   contradiction none of M92/M98/M99/M103 ever actually presented (each of those either had the latch bit
   unread, or the ring not yet proven live by a positive control).
5. **A positive control from a different, already-trusted source** (the CP fence, M77/M106): the tool of last
   resort, because it is the only one of the five that tells "the ring/ISR/DPC plumbing itself" from "this one
   source" apart - which is exactly what M98/M99/M103 were missing until E24 run 004 supplied it.
