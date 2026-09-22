# SetVidPnSourceAddress's own flip design note (E22 step 3, ADR 0011 point 3 step 3)

Design decisions taken while implementing `EnableVidPnFlip`, 2026-09-22, against bc250kmd 0.7.23 (no version
bump). Every claim carries a `file:line`; lines marked **DECISION** are this note's own choice, not something
measured. Style follows `docs/design/paging-node.md`.

## 1. What this adds and what it reuses

Step 2 (ADR 0011 point 3 step 2, `dcn.c:227-459`) already proved the M87 write sequence works, through an
escape (`DcnFlipEscape`/`DcnFlipCore`) driven from display-only mode. Step 3 wires the same sequence to the
full WDDM table's own flip path, so that a real `DxgkDdiSetVidPnSourceAddress` call - the one dxgkrnl issues
after every present, not an escape a test tool calls by hand - moves the scanout, and the VUPDATE_NO_LOCK
interrupt reports completion instead of a fixed-period software timer. Everything new sits behind a second
gate, `EnableVidPnFlip`, chained onto the existing `EnableDcnWrite` (`mmio.c:131-134`: `VidPnFlipEnabled =
DcnWriteEnabled && GuardReadSetting(L"EnableVidPnFlip", 0) == 1`) - which itself already means `EnableMmio &&
EnableDcnWrite`. Nothing here is meaningful unless `EnableFullWddm` is also open: `wddm.c`'s `Wddm` pointer is
NULL in display-only mode, and every new call site checks it first (`WddmVSyncArm`, `wddm.c:849`; `WddmDcnVsync`,
`wddm.c:908`).

## 2. Address conversion and validation

**DECISION: reuse the two numbers `VramStart` already measured, never a literal.** dxgkrnl's
`DXGKARG_SETVIDPNSOURCEADDRESS.PrimaryAddress` names an offset into this adapter's one declared segment
(`WddmQuerySegment4`), which this driver bases at `Device->VramMcBase` - the GPU MC address of VRAM byte 0,
read from `GCMC_VM_FB_LOCATION_BASE` in `VramStart` (facts M31). DCN's own surface-address registers want a
*system physical* address instead, and `Device->VramPhysical` is the physical address of that same VRAM byte 0
(also `VramStart`). Facts M85's formula - `physical = VramBase + (CardAddress - McBase)` - is exactly the
arithmetic connecting the two already-measured numbers, so `DcnTranslateCardAddress` (`dcn_translate.c`) does
nothing but that subtraction and addition, with a range check (`CardAddress >= McBase`, `offset < VramLength`)
so an address dxgkrnl never actually owns cannot wrap into something that reads as valid.

The translated address is then checked against the VRAM carve-out a second way, independent of the first: **the
existing `AddressAllowed`** (`dcn.c:246-251`), refactored in place rather than duplicated, so the DDI path and
the escape path enforce the identical rule - an address is allowed only if it is the firmware's own captured
address, or it lies inside `[VramPhysical, VramPhysical + VramLength)` with the flip's fixed surface size
(`BC250_DCNFLIP_SURFACE_BYTES`) fitting before the top and 4 KiB aligned (`DcnAddressFits`,
`dcn_translate.c`, the same alignment DCN's own primary surface address register expects). A card address that
does not translate, or translates outside the carve-out, is refused with `STATUS_ACCESS_DENIED` and counted
(`Device->DcnFlipRefused`, `dcn.c:483-486`) rather than written to any register - **DECISION**: refuse silently
to hardware, loud only in the log and the counter, consistent with the escape path's own refusal (M85 already
established that writing an unvalidated address to these registers is not something to try "to see if it
accepts it", hard rule 3 of `bc250-win/CLAUDE.md`).

Both functions are pure `unsigned long long`/`int` C with no WDK header (`dcn_translate.h`/`.c`), so they are
host-testable (`driver/kmd/test/dcn_translate_test.c`, seven cases: firmware address at offset 0, a mid-range
offset, below `McBase` refused, at/past the carve-out top refused, misaligned refused, a surface that does not
fit refused, a zero-length carve-out refused) and compiled a second time with the real kernel flags
(`driver/kmd/test/run_dcn_translate.ps1`) so a change here cannot pass the host test and fail the driver build.

## 3. Where the write happens, and why it does not reuse the escape's blocking poll

`DcnFlipSourceAddress` (`dcn.c:472-516`) calls the same `DcnFlipWriteSequence` (`dcn.c:295-318`, factored out of
`DcnFlipCore`'s step 2 so the two paths share the instruction sequence, not a second copy of it) but never
`PollFlipPending`'s 50 ms busy-wait, which only the escape path (PASSIVE_LEVEL, an explicit test operation) may
afford. `DXGKDDI_SETVIDPNSOURCEADDRESS` is annotated `_IRQL_requires_min_(PASSIVE_LEVEL)` /
`_IRQL_requires_max_(PROFILE_LEVEL - 1)` (d3dkmddi.h) - a flip may be programmed from inside the VSync interrupt
itself - so a busy-wait there would stall the very DPC that is supposed to report the flip's completion
(section 5). `Bc250WddmSetVidPnSourceAddress` (`wddm.c:2476-2528`) calls `DcnFlipSourceAddress` only when the
address actually changed (`InterlockedExchange64` on `wddm->PrimaryAddress`, unconditionally on IRQL, since
`MmioDcnRead`/`MmioDcnWrite` take no lock of their own - `mmio.c` - and are legal at any level reachable here);
`WddmVSyncArm(device, TRUE)` - which does take a spin lock and, with the gate closed, calls `KeSetTimerEx` -
is skipped above `DISPATCH_LEVEL` exactly as it already was before this change (`wddm.c:2526`, `high` computed
at `wddm.c:2489`), counted separately (`FlipsAboveDispatch`) so the assumption that `SetVidPnSourceVisibility`
has already armed the source stays measured rather than hoped for.

## 4. ISR ack: location and IRQL legality

**DECISION: ack the VUPDATE_NO_LOCK event with a direct register read-modify-write at DIRQL, not through the
generic IH ring.** `Bc250InterruptRoutine` (`pnp.c`) now calls `DcnVsyncInterrupt` (`dcn.c:558-582`)
unconditionally, next to `IhInterrupt`, and ORs the two booleans for its own return value. The function is a
no-op (`FALSE`, nothing read) unless both `Device->VidPnFlipEnabled` and `Device->DcnVsyncArmed` are true - the
second flag is what `WddmVSyncArm`'s hardware branch (`wddm.c:851-864`) sets, under `wddm->Lock`, exactly the
way the software path already sets `wddm->VSyncArmed`. What makes the RMW itself legal at DIRQL is that
`MmioDcnRead`/`MmioDcnWrite` (`mmio.c`) take no lock at all - a binary search over a static allow-list plus a
volatile access to mapped BAR5 - so this needed none of the generic IH ring's single-consumer, DPC-only
machinery (`ih.c`) to stay correct. The ack ORs `VUPDATE_NO_LOCK_EVENT_CLEAR` into the value just read
(`dcn.c:572-573`), so `VUPDATE_NO_LOCK_INT_EN` and every other field of `OTG0_OTG_GLOBAL_SYNC_STATUS` go back
exactly as read - the identical RMW `DcnVsyncEnable` (`dcn.c:537-549`) uses to arm or disarm the source, so the
sequence is written once, not twice. A read or write that is refused (BAR5 gone, offset not on the allow-list)
counts as `DcnVsyncRefused` and returns `FALSE` rather than retrying - a transient refusal must never hang the
ISR.

`Device->DcnVsyncArmed` is read racily inside the ISR, the same accepted race `ih.c` already takes on
`ih->Active`: at worst one interrupt arrives either armed a tick early or after the gate has already started
closing, and `WddmStop`'s own ordering (section 7) is what actually has to be race-free, not this read.

## 5. DPC reporting and the flip-pending poll

`Bc250DpcRoutine` (`pnp.c`) calls the new `WddmDcnVsync` (`wddm.c:903-929`) unconditionally, the same shape as
the existing `WddmGpuFence`/`WddmGpuFencePaging` calls next to it: completion is decided by polling state, not
by which vector woke the DPC. `WddmDcnVsync` is a no-op unless `Device->DcnVsyncAcked` (set by the ISR,
`InterlockedExchange`d back to 0 here so each ack is consumed once) is nonzero, `wddm->Stopping` is false and
`wddm->VSyncEnabled` - `ControlInterrupt`'s own flag - is set (`wddm.c:2542-2552`: `ControlInterrupt` sets
`VSyncEnabled` unconditionally and, on enable, calls `WddmVSyncArm(device, TRUE)`, which arms the hardware
source when the gate is open and the software timer otherwise - so the DDI's contract, "ControlInterrupt turns
CRTC_VSYNC on", is honoured identically by both paths).

Before reporting, `WddmDcnVsync` polls `DcnFlipPending` (`dcn.c:522-529`) - a single, non-blocking read of
`HUBPREQ0_DCSURF_FLIP_CONTROL`'s `SURFACE_FLIP_PENDING` bit, `FALSE` (treated as "not pending", never blocking)
on any register-read failure. This mirrors amdgpu's own `dm_vupdate_high_irq`, which completes a flip only once
`dc_get_flip_pending_on_otg` says none is pending - the double-buffered HUBP hardware can still show the
previous surface for the instant right after VUPDATE if the CRTC caught the update mid-scan. Facts M94 measured
the bit clearing within one frame, so a still-pending read defers the report to the next tick
(`Device->DcnVsyncDeferred`, counted) rather than reporting early; in the ordinary case this should not defer
more than once. Once clear, the DPC reports `DXGK_INTERRUPT_CRTC_VSYNC` with `wddm->PrimaryAddress` - the same
address `SetVidPnSourceAddress` last stored, which is also the address `DcnFlipSourceAddress` already wrote to
the hardware - through the existing `WddmReport` (`wddm.c:159-161`'s own comment: nothing else retires a queued
flip). **DECISION**: report `CRTC_VSYNC`, not a second, separate `DISPLAYONLY_VSYNC` - this table is the full
WDDM one; `DISPLAYONLY_VSYNC` belongs to the display-only DDI table this build does not use once
`EnableFullWddm` is open, and `MaxQueuedFlipOnVSync = 1` (`wddm.c:1391`, unchanged) means one report is exactly
what retires the one flip that can be outstanding.

## 6. Interaction with display-only mode

With `EnableFullWddm` closed, `Device->Wddm` is NULL and every new function that dereferences it
(`WddmVSyncArm`, `WddmDcnVsync`) returns at the top before touching anything - `EnableVidPnFlip` is simply
inert in that configuration, gate value or not, the same way `EnableDcnWrite`'s escape path already is. With
`EnableFullWddm` open but `EnableVidPnFlip` closed, `Device->VidPnFlipEnabled` is `FALSE` (chained through
`DcnWriteEnabled`, `mmio.c:134`) and every branch this note adds falls through to the code that existed before
it: `WddmVSyncArm` runs the unchanged software-timer body, `Bc250WddmSetVidPnSourceAddress` records the address
and counts a flip exactly as 0.7.23 did, and the firmware keeps scanning out its own framebuffer address -
dxgkrnl is told a flip retired (by the software timer's `CRTC_VSYNC` report) even though nothing moved on
screen, which is the documented, pre-existing behaviour of display-only-table-closed-but-WDDM-open
(`wddm.c:2492-2497`'s own comment).

## 7. FlipCaps: no change

`DXGK_DRIVERCAPS.FlipCaps.FlipOnVSyncMmIo = 1`, `FlipIndependent = 1`, `MaxQueuedFlipOnVSync = 1`
(`wddm.c:1389-1391`) were already set for the full WDDM table before this step and are left exactly as they
were. **DECISION**: no new cap is declared. `FlipOnVSyncMmIo` already promises "SetVidPnSourceAddress performs
an MMIO flip, synchronized to VSync" - which is now literally true instead of aspirational - and
`FlipIndependent` already promises the driver accepts any address it is given, which the validation in section
2 still honours (refusal is a defensive floor, not a policy narrower than what the cap promises).
`FlipImmediateMmIo`/`FlipOnVSyncWithNoWait` are not declared: nothing in this step changes the synchronous,
one-flip-in-flight model `MaxQueuedFlipOnVSync = 1` already describes, and HUBP's own double-buffered latch
(writes take effect at the *next* vsync automatically) is inherent `FlipOnNextVSync` semantics without needing
a "flip immediately, off cycle" cap this driver does not implement.

## 8. StopDevice ordering

`Bc250StopDevice` (`pnp.c:98-124`) already calls `WddmStop(device)` first - "nothing below it is allowed to
have run" - then, in order, `IhStop`, `GfxStop`, `PspStop`, `GartStop`, `VramStop`, `DcnStop`, `MmioStop`.
`WddmStop`'s own critical section (`wddm.c:1146-1159`) now disarms the hardware vsync source next to the
existing software-timer cancel: `if (Device->DcnVsyncArmed != 0) { InterlockedExchange(&Device->DcnVsyncArmed,
0); DcnVsyncEnable(Device, FALSE); }`, under the same spin lock and for the same reason the timer cancel is
there - so that nothing can re-arm behind `wddm->Stopping` being set. Because this runs inside `WddmStop`,
which runs before `DcnStop`, the VUPDATE_NO_LOCK interrupt is disabled and acked (`DcnVsyncEnable(..., FALSE)`
always ORs in `EVENT_CLEAR`, `dcn.c:547`) before `DcnStop` restores the firmware's own surface address
(`dcn.c:436-459`, unchanged) and before `MmioStop` unmaps BAR5 - an ISR reading or writing DCN registers after
the mapping is gone is exactly what this ordering prevents. `Device->VidPnFlipEnabled` itself is cleared in
`MmioStop` (`mmio.c:150`), after every one of the above has already used its old value for the last time this
start.

## 9. Counters and the gate-closed regression bar

New counters (`Device->DcnFlipsHardware`, `DcnFlipRefused`, `DcnVsyncArmed`, `DcnVsyncTicks`, `DcnVsyncRefused`,
`DcnVsyncDeferred`, `bc250kmd.h`) are reset to 0 in `MmioStart` (`mmio.c`) and reported by `WddmSummaryOf`
(`wddm.c`) next to the existing vsync/flip lines, following the same "0 with the gate closed" convention as
node 1's own counters (`docs/design/paging-node.md` section 6). **DECISION**: these do not go through
`BC250_ESCAPE`'s `Reserved[]` wire array - `Reserved[2]` is already fully used by `Blits`/`Flips`
(`bc250kmd_escape.h:64`'s own comment: reusing those two words is why the struct did not need
`BC250_KMD_VERSION` to move) - so exposing the new counts through `GET_INFO` would grow the struct and bump the
version, which this task explicitly rules out. They are visible through `GuardLog`/`bc250kmd_cli log` only,
same as every stage-behind-a-gate counter that came before a wire-struct change was budgeted for it.

**What never runs with `EnableVidPnFlip` closed**: `DcnFlipSourceAddress`, `DcnVsyncEnable`, `DcnVsyncInterrupt`
(all gate-checked at their own top), `WddmDcnVsync` (`Device->VidPnFlipEnabled` checked first), and
`WddmVSyncArm`'s hardware branch. Every counter above stays 0, `Bc250InterruptRoutine` and `Bc250DpcRoutine`
still call the new functions every time (so the *call* is not gated, only its effect is) - the same "new
branches, not edited ones" bar `paging-node.md` section 6 sets, verified here by the unchanged EXACT MATCH
verdict of every `driver/shim/test/*.ps1` replay (none of which touch `dcn.c`/`wddm.c`/`mmio.c`/`pnp.c` at all)
and by `tools/wddm_contract_check/check.py` staying 34 OK / 4 n/a.

## 10. What remains open

1. **Not measured on hardware by this note**: whether `SURFACE_FLIP_PENDING` truly never stays set across two
   consecutive `WddmDcnVsync` ticks under a real present cadence - facts M94 measured "clears within one frame"
   from the escape path's own poll, not from back-to-back hardware-driven vsyncs. E22 step 3's H7 (README) is
   this question.
2. **VUPDATE_NO_LOCK's exact trigger phase relative to the CRTC's active/blank boundary** is amdgpu's own
   documented behaviour (`dm_vupdate_high_irq`), not something this driver has measured on this ASIC yet; if it
   fires later in the frame than expected, `DcnFlipPending`'s defer-and-recheck already covers that case rather
   than assuming it away.
3. **Whether a second `SetVidPnSourceAddress` can arrive before the first flip's VUPDATE_NO_LOCK has been
   serviced** is unmeasured; `DcnFlipWriteSequence` always writes the newest address (last write wins, matching
   `MaxQueuedFlipOnVSync = 1`'s promise of at most one outstanding flip), so this is believed safe by
   construction rather than proven by a lab run.
