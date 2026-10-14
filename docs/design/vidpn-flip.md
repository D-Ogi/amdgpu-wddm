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

**Changed by BD-170 (2026-10-14):** `FlipImmediateMmIo = 1` is declared, and the immediate flip is
implemented. Section 7.1 gives the contract and how the driver keeps it.

### 7.1 BD-170: the immediate flip

**Why the cap.** `DXGK_FLIPCAPS` says that "the driver must support an immediate flip and a flip interval of
one" (local `ref/ddi-display/d3dkmddi.md:21231`). Without an MMIO immediate flip, dxgkrnl sends an immediate
flip down the DMA-buffer path. An exclusive-fullscreen D3D11 present at SyncInterval 0 is such a flip. The
driver wrote no DMA for a flip. The scheduler then marked the device as an error (DxgKrnl
`VidSchMarkDeviceAsError` reason 19, after its "DMABufferLength > 0" assertion). The application got
`DXGI_ERROR_DEVICE_HUNG` at its first Present, with no TDR and no System event (BD-170 arm a02).

**The promise.** `FlipImmediateMmIo` is a flip that "takes effect immediately following a call to
DxgkDdiSetVidPnSourceAddress without waiting for the next vertical sync" (`d3dkmddi.md:21235-21239`). It is not
`FlipOnVSyncWithNoWait`, which is a next-vsync flip that does not stall the pipeline (`d3dkmddi.md:21206-21212`).
`DXGK_SETVIDPNSOURCEADDRESS_FLAGS.FlipImmediate` tells the driver to start the scan-out from the new allocation
at once (`d3dkmddi.md:28495-28515`).

**Completion and reuse.** The legacy DDI documents no completion rule for an immediate flip. For the return
value its page says only that the DDI returns `STATUS_SUCCESS` on success and otherwise "one of the error codes
defined in Ntstatus.h" (`d3dkmddi.md:12791`). The MPO3 DDI has a rule: the flip is complete at the
return, unless the driver sets the output flag `HsyncInterruptCompletion` (`d3dkmddi.md:25263-25275`). This
driver does not implement the MPO3 DDI. What the OS does on the legacy path, and what `STATUS_DEVICE_BUSY`
(0x80000011) means to it, comes from the binary, not from the documentation: a static read of `dxgmms2.sys`
10.0.22621.6199 (the build that the lab's 22631 uses) and 10.0.26100.9549, which agree (BD-170, 2026-10-14,
facts M904). Windows 11 26200 tester minidumps (BD-135) carry the 26100.9549 image. The lab's own 26200 system is
checked by its HLK run. The lab arm a03 measured the busy behaviour on 22631 (facts M902).

1. Without the MPO DDIs, `VidSchiExecuteMmIoFlip` calls `SetVidPnSourceAddress` through
   `DpSynchronizeExecution` (`VidSchiExecuteMmIoFlipAtISR`). The calls are serialized.
2. For an immediate flip, the flip entry is set to `FlipWaitComplete` (11) before the call. On a success status,
   the OS releases the allocation that was on screen, references the new one and completes the flip entry, all
   before it continues. It does not wait for a `CRTC_VSYNC` report. So the replaced allocation can be written
   again as soon as the DDI returns success, and a later report cannot make up for a flip that had not taken
   effect.
3. For a vsync flip, the entry waits in `FlipWaitVSync` (5). A `CRTC_VSYNC` report completes it only when its
   `PhysicalAddress` is the flip's address. An entry that waits too long causes a TDR
   (`VidSchiCheckHeadTimeout`, which watches this state and the MPO3 Hsync wait, 15, and no other). On this part
   a TDR is bugcheck 0x116, because the GPU has no working reset (facts M53).
4. Any status other than success leaves the flip not completed at the head of its queue, with the previous
   primary still referenced. A failure status is also recorded as a failure. In both cases the VSync DPC
   (`VidSchiExecuteNextFlipQueueEntry`) executes the flip again, once a VSync, with no count, no time limit, no
   head timeout, no TDR and no device error. Later flips of the same queue wait behind it. Nothing keeps the
   VSync on for such an entry: `VidSchiKeepInterruptTargetEnabledForFlip` acts only for an entry with an MPO
   plane, and the idle count that lets the OS switch the VSync off looks only at VSync waiters and new submits.
   Without a success, only an OS cancel ends it: a change of the flip device, a device or adapter teardown, a
   suspend, a TDR, a device already in error, or a newer flip that the OS marks `FlipRestart`. That newer flip
   cancels the busy head (entry status 6). A mode change does not cancel it. The previous primary is released
   only at a success status, that is at a real completion.
5. The legacy path has no deferred completion and reads no driver capability for one.
   `HsyncInterruptCompletion` is on the MPO3 path only.

The legacy path writes the ETW event `MMIOFlip`, not `MMIOFlipMultiPlaneOverlay`, so a trace of it does not
have `FlipEntryStatusAfterFlip`.

**The flags of a legacy flip.** `VidSchiExecuteMmIoFlip` sets `FlipImmediate` only for a flip that the OS marks
true-immediate, or for one present mode of the source. The true-immediate mark comes from an independent flip
(`PrepareIndependentFlipToken`) or from an MPO3 present. The legacy `D3DKMTPresent` route does not set it. Every
other flip gets `FlipOnNextVSync`, at SyncInterval 0 too. So `FlipOnNextVSync` does not tell an immediate flip from
a vsync flip, and the driver reads only `FlipImmediate` for this decision. The record of each immediate flip keeps
the request's flags. In a03 every immediate flip of the DXGI client carried `0x002` (`FlipImmediate` only).

**The restart mark.** The OS marks a flip `FlipRestart` from `D3DKMT_PRESENTFLAGS.FlipRestart`, or for an
independent flip from its own restart choice. A queued flip with the mark cancels the entries before it
(`VidSchiRestartQueuedFlip`, entry status 6). That cancel does not release the previous primary. The OS releases it
when the flip with the mark completes. A queued flip without the mark waits behind a busy head as long as the head
stays busy. The OS admits it without a wait, and only the 64-entry ring of the flip queue limits the queue.

**The programming.** DCN 2.01 uses `hubp1_program_surface_flip_and_addr` (Linux v6.18
`dc/hubp/dcn201/dcn201_hubp.c:115`, `dc/hubp/dcn10/dcn10_hubp.c:349`). It writes `SURFACE_FLIP_TYPE =
flip_immediate`, then the address, high half first. DC does this inside the TG master update lock
(`dc/core/dc.c:4194` lock, `:4398` `update_plane_addr`, `:4406` unlock). `dcn201_update_plane_addr` records an
immediate address as current at once (`dc/hwss/dcn201/dcn201_hwseq.c:156-157`). `DcnFlipWriteSequence`
(`driver/kmd/dcn.c`) keeps that order in the acknowledged `OTG_MASTER_UPDATE_LOCK`. It writes
`SURFACE_FLIP_TYPE` 1 for a request with `Flags.FlipImmediate` and 0 for every other flip. A vsync flip writes
the same register values as 0.7.216.64. DCN 2.01 has no `set_flip_control_gsl` in its hardware sequencer, so
the driver does not touch `SURFACE_GSL_ENABLE`. The two stereo-sync fields keep the firmware's values, as since
M87. The record of each immediate flip shows the full `DCSURF_FLIP_CONTROL` value.

**When a flip has taken effect.** `hubp1_is_flip_pending` (`dcn10_hubp.c:751-777`) calls a flip done when
`SURFACE_FLIP_PENDING` is clear and `SURFACE_EARLIEST_INUSE` is the requested address. That is a vblank test:
`EARLIEST_INUSE` follows an immediate flip only at the next vblank (facts M902). amdgpu does not wait for it for an
immediate flip. It sends the flip event at the HUBP flip interrupt (`amdgpu_dm/amdgpu_dm.c:437-525`), and it
releases the old framebuffer of an async flip with no wait for `EARLIEST_INUSE`
(`amdgpu_dm.c:11075-11084,11136-11142`). This driver keeps `DcnFlipPending` for the vblank reports and for a
request for the address already published. For an immediate flip it uses P':

- P' holds at one sample when `SURFACE_FLIP_PENDING` is clear (first observed clear, not a measured edge),
  `DCSURF_PRIMARY_SURFACE_ADDRESS`, read high word first, reads back as the request in the field widths of that
  register, and the primary generation is unchanged.

The lab arm a04 compared P' with the flip event (`SURFACE_FLIP_OCCURRED`) and with `EARLIEST_INUSE` for 116 224
immediate flips. All three were first seen in the same sample, in the call, for every flip, the vertical blank
included (facts M903). For next-VSync controls the address readback matched in the call while the pending bit and
the event did not. So the address readback alone is no witness of a latch, and the pending bit is the term that
tells a latch from a programming.

`DcnImmediateObserve` (`dcn.c`) samples P' after the programming, inside the flip transaction (under L, section
7.2), with 1 us stalls between samples. The bound is 50 us (`BC250_DCN_IMMEDIATE_OBSERVE_US`) from the end of the
programming sequence. The reader checks it before every additional sample, including after the 1 us stall.
A step count of 50 also ends the loop. A sample that started inside the bound can end past it: the end time is taken after
the loop, and such an observation is counted (`DcnImmediateOverruns`). The function writes no register and is legal
up to `PROFILE_LEVEL - 1`. It checks at its start and at its end that the generation is still the one that the
transaction set. Its outcome is one of these:

| Outcome | Meaning | Section 7.2 |
| --- | --- | --- |
| `DONE` | P' at a sample that started inside the bound | success |
| `DONE_LATE` | P' at a first sample that started after the bound (a preempted caller): complete, and the time is an upper bound only | success |
| `PENDING` | `SURFACE_FLIP_PENDING` still set at the bound | U |
| `PRIMARY_OTHER` | the pending bit clear, `PRIMARY_SURFACE_ADDRESS` on another address at the bound | U |
| `READ_FAILED` | the last sample could not read `FLIP_CONTROL` or `PRIMARY_SURFACE_ADDRESS` | U |
| `SUPERSEDED` | the primary generation moved during the observation, which L does not let happen (facts M904) | U |

The record of each immediate flip keeps the request's `Flags.Value` as it came.

**The cost of the bound.** 50 us is 4.2 lines of the lab mode (174.5 MHz pixel clock, 2080 pixels a line,
11.9 us a line). In a04 the latest sample start of an immediate flip was 18 us after the start of the programming
(facts M903). That is not the duration of the DDI. The summary prints the time of the whole transaction inside L
(`FlipLockedMaxUs`: admission, programming, observation and E5) next to the observation times.

**Independent flips.** `VidSchiSubmitMmIoFlipCommand` has an internal check that can end in bugcheck 0x119.
It fires when a flip without `FlipRestart` is queued behind an `IndependentFlipExclusive` entry still at the head.
An error leaves that entry at the head (M904). This remains an OS risk after an admission or hardware failure.

r7 does not hide such failures with success. The old workaround let Windows release a primary that hardware still read.
Both immediate and next-VSync refusals now remain errors. A valid ordinary immediate request succeeds only with its hardware proof.
A request during an active hardware writer returns `STATUS_DEVICE_BUSY`, without publishing or completing it (section 7.2).

**Next train: the MPO3 route.** With `HsyncInterruptCompletion`, the driver completes an immediate flip after the
return, with a `CrtcVsyncWithMultiPlaneOverlay2` notification (`d3dkmddi.md:25271-25273`). That removes the
in-call observation: the flip completes at the next interrupt after it latches. dxgmms2 waits for it in state 15,
which its head timeout watches. So a flip that never completes ends in the OS's timeout, which is a TDR and, until
this part has a working GPU reset, bugcheck 0x116. The route needs the MPO3 DDIs
(`SetVidPnSourceAddressWithMultiPlaneOverlay3` and their caps). It is not in tester.31.

**Several requests in one refresh.** Each immediate request is programmed and observed before the DDI returns.
A second request in the same frame programs its own address, and the first may never be shown, which is what
an immediate flip does. `DcnImmediateSameFrame` counts the requests that share an OTG frame with the one before.

**Same-address promotion.** A next-vsync request for B is published at once and latches at the next VUPDATE.
An immediate request for the same B is then no change for the `changed` test of the flip transaction. While
`DcnFlipPending` still reports B pending, the driver programs B again with the immediate flip type, observes P' like
every immediate flip and counts it (`ImmediatePromotions`). It is not a new flip and does not move `Flips`. When B
is no longer pending by that vblank test, the request is complete and gets success with nothing programmed.

**Clone view.** dxgkrnl flips a secondary clone output with an MMIO immediate flip, so that it can treat the
surface as free (`d3dkmddi.md:12809-12819`). This driver has one video present source:
`SetVidPnSourceAddress` refuses any `VidPnSourceId` other than 0. The rule above, free on a success return,
covers it.

**DMA flips.** With both MMIO caps, `Present` gets `pDmaBuffer` NULL for a flip and the flip arrives through
`SetVidPnSourceAddress` (`d3dkmddi.md:9701`). A flip that still arrives with a DMA buffer needs commands that
flip from the GPU's own stream, and this driver has none. `WddmPresentDmaFlip` (`wddm.c`) refuses it with
`STATUS_ILLEGAL_INSTRUCTION`, the status of the DDI's table for "instructions that graphics hardware cannot
support" (`d3dkmddi.md:9640`). It writes nothing, does not move the buffer pointer and does not pretend a
completion. `DmaFlipsRefused` counts it.

**Known limits.** The driver counts these and does not assume them away:

1. P' is measured for address-only flips on a running, unblanked plane with one writer (facts M903). A format or
   pitch change and a flip that was pending when it was armed are not measured. They use the same predicate, and U
   (section 7.2) covers a miss.
2. U is E5, and on this part E5 ends in bugcheck 0x116 (facts M53). a04 had no U in 116 225 immediate flips. That
   does not prove that U never comes.
3. `FlipInterval` (intervals 2 to 4) is not declared.
4. The promotion test reads the hardware before the write. If the vsync flip latches between the two, the
   driver programs the same address again, which does no harm.
5. An independent-flip refusal can still reach the OS queue assertion described above. Writer contention returns BUSY without an unsafe success or E5.
6. Whether dxgkrnl cancels a pending flip at a modeset commit is not read (`OwedInvalidated`, section 7.2).
7. The dxgmms2 reading is static. F1 (section 7.2) counts the case in which a call above DISPATCH_LEVEL does not
   hold L.
8. A flip that the OS queues behind an `IndependentFlipExclusive` entry while E5 runs can still reach the 0x119
   check first (facts M904).

**Evidence.** `vidpn_flip_test.c` runs the actual source of the flip transaction, the writers, E5 and its DPC, the
destroy restore and the vblank report, at both entries (section 7.2, the checks). `dcn_flip_test.c` (`FlipTypes`,
`ImmediateObserve`) and `retained_power_test.c` run the actual source too. The dxgmms2 reading is recorded without
code in `evidence/windows/2026-10-14-bd170-dxgmms2-legacy-flip/README.md` (facts M904), the a03 busy head in
`evidence/windows/2026-10-14-bd170-a03-busy-head/README.md` (facts M902), and the a04 latch measurement in
`evidence/windows/2026-10-14-bd170-a04-latch/README.md` (facts M903). `tools/wddm_contract_check` rule R40 requires
the cap with `FlipOnVSyncMmIo`.

`bc250kmd_cli log summary` prints the counters: the edges and results of section 7.2, the outcomes of the
observation, promotions, the done and observation times, the overruns, the time inside L, DMA refusals, the
writers' counters and the E5 record. It prints the last 16 `CRTC_VSYNC` calls of `ControlInterrupt` with their QPC.
It also prints the last eight records of immediate flips, each as a whole copy under its sequence lock: outcome,
request, request flags, the last `PRIMARY_SURFACE_ADDRESS` sample and read status, samples, primary generation,
frame, scan position before and after, the first `DCSURF_FLIP_CONTROL` sample, and the programming, first-sample,
pending-clear, done and end times.

### 7.2 BD-170: primary ownership during a writer

r7 removes the r6 interval in which an immediate success could release a surface that hardware still read.
A writer first moves scan-out to a reserved buffer owned by the driver. It proves that transition before it starts its long operation.
Long writer operations use only reserved buffers. Only `End`, under L, can select an application surface again.

**The contract.** An immediate success lets the OS release the previous primary before another VSync (M904).
A next-VSync success leaves the entry waiting for its address in a real CRTC report.
Desired is a record of the latest accepted address. It holds no independent allocation or residency reference.
Safe retirement alone does not satisfy a flip request. The DDI must also program the requested primary on its required schedule.
The local contract requires the following VSync, or an immediate flip without waiting for that retrace (`d3dkmddi.md:12801,12819`).
No documented exception permits a long writer to return success and defer the requested primary until it finishes.

The reserved runtime surface is carved out before `QUERYSEGMENT` (`driver/kmd/connector_buffer.c:8`).
Its actual length bounds every bridge mode. The firmware surface is also excluded from the application segment.
A software pointer to an application allocation is never treated as ownership of its VRAM.

**The lock L.** L is the interrupt lock of this driver's single MSI message (M38).
The ISR owns it. The high entry of `SetVidPnSourceAddress` also owns it through dxgmms2's synchronized call (M904).
The low entry and writer sections acquire it through `DxgkCbSynchronizeExecution`, message 0.
The callback is called only at or below DISPATCH_LEVEL. Code inside L never recursively takes it.

Long link, ATOM, clock, scaler and audio work stays outside L. No writer waits at DIRQL for a PASSIVE thread.
Hardware observation inside L is bounded. Diagnostics from `DcnFlipSourceAddress` do not format or print under L.
The whole flip section has its own elapsed-time measurement, including lab witnesses.

**`Begin` and the first transition.** `WddmPrimaryExclusiveBegin` first serializes admission under L.
Another active writer causes a bounded PASSIVE retry or refusal, never a fault for normal contention.
Under L it sets `WriterActive`, the odd generation and the BUSY gate before it programs a driver-owned bridge.
Then it releases L and polls for retirement at PASSIVE_LEVEL, for at most 30 ms from the end of programming.
The old primary remains retained during this interval. Immediate and next-VSync calls return BUSY.
Destroy of the old primary also returns BUSY until retirement is proved.
Short stalls outside L avoid a coarse timer wake that could skip the next VSync and the entire observation window.
After the proof, a second section checks the owner, generation and fault state before it sets `WriterReady`.
Only then does `Begin` authorize the long operation. Destroy cannot free the old allocation before this step.
A pre-write refusal rolls back admission and leaves the writer unstarted, without E5.
A failed write or a retirement timeout retains ownership and enters explicit recovery.

Blanking alone is insufficient. An old address can remain latched while HUBP is drained.
Programming a new shadow address and resuming memory reads must not expose that old application address.
The bridge handoff requires the stable effective `EARLIEST_INUSE` address to match the reserved surface, in addition to the requested address and clear pending bit.
A requested-address readback with an old effective address is insufficient for this ownership transition.
Every subsequent long writer transition moves between driver-owned surfaces, or keeps the plane drained.

**Requests while the writer runs.** A flip enters through the same serialized transaction as an ordinary flip.
With hardware flips enabled, an otherwise admitted request returns `STATUS_DEVICE_BUSY` while `WriterActive` is set.
This covers immediate and next-VSync requests, before any register write, Desired publication or teardown identity change.
Normal writer contention does not enter E5 and never waits at DIRQL for the PASSIVE writer.
The previous OS primary remains retained. No unprogrammed request is reported as a completed hardware flip.
With the hardware gate closed, the existing software-report route retains its separate deposit behavior.

BUSY is a safety response, not a proved recovery mechanism for this legacy scheduler.
M904 shows that an exclusive queue head can remain and lead to the later 0x119 assertion.
M902 also records a BUSY head that stopped progress until a restart.
The exact r7b artifact still needs a lab overlap trial. These risks remain open until that trial or a different completion contract resolves them.

**Resume and rollback.** `WddmWriterResume` returns an explicit status and authorizes only a reserved-buffer operation.
It does not return Desired to a caller outside L.
`DcnRuntimeSurface` uses the reserved runtime buffer, with bounds from its allocation.
`DcnLinkSurfaceRestore` also uses that buffer, with the old mode's geometry and visibility.
Rollback metadata contains no application address. The old mode can therefore return after its former primary has been released.
A capture after `Begin` supplies only the hardware visibility bit. Its address, pitch and format are never replayed.
Hardware visibility and the OS logical `SourceVisible` are preserved separately, including an inherited cold start where they differ.
A failed Resume section stops the operation before its writes.

**End and the final transition.** `WddmPrimaryExclusiveEnd` selects the retained Desired under L.
Hardware writer contention has accepted no newer primary, so this is the request retained at `Begin`, unless destroy invalidated it.
The software-report route may have a newer deposit when the hardware gate is closed.
It checks that address against the mode that the writer actually committed or restored.
The final programming and ownership update occur in the same section, before the generation becomes even.
No writer can later replay a saved application address.

If the current Desired does not fit the final mode, the plane stays on the reserved bridge and the refusal is recorded.
A successful programming does not imply a later VSync report. The hardware report still controls completion of a next-VSync entry.
A detach, suspend or invalidating End retains the safe reserved or drained state when no valid primary must be resumed.
With the flip gate closed, the existing software-report route remains separate from hardware ownership.

**Every long writer follows the same protocol.** The references below identify the entry and final status handling.
`Begin` protects the entire interval before the first long operation, not just the eventual register write.

| Writer | Protected work outside L | Final ownership and error handling |
| --- | --- | --- |
| Link commit and rollback, `connector_link.c:781` | Clock session, audio stop, HDMI handoff, `DisableOwned` and `Apply`. Rollback uses the reserved buffer with the previous mode's dimensions | `connector_link.c:868`: End selects current Desired. Its failure becomes the returned status and prevents audio restart |
| Link disable, `connector_link.c:627` | AUX close, HUBP blank, stream and transmitter disable | `connector_link.c:637`: invalidating End, with its failure returned |
| Link suspend, `connector_link.c:896` | Firmware or runtime buffer, then output visibility off | `connector_link.c:901`: invalidating End, with its failure returned |
| Link resume, `connector_link.c:911` | Clock session, disable and mode replay, then visibility off | `connector_link.c:915` and `:930`: both early failure and normal exit call End and return its failure |
| HDMI start, `hdmi_link.c:141` | Prediction, ATOM programming and timing measurement while hardware uses the bridge | `hdmi_link.c:170`: failed End is logged and blocks audio start |
| HDMI stop, `hdmi_link.c:229` | Restore the inherited register journal while hardware uses a reserved buffer | `hdmi_link.c:233`: failed End makes the operation fail |
| Modeset commit, `modeset.c:477` | Scaler programming. Growth selects a sufficiently large firmware surface | `modeset.c:497`: End status is returned and stored in `LastStatus` |
| Modeset D3, `modeset.c:577` | Firmware surface and native shape, without proceeding after a failed surface change | `modeset.c:582`: End failure is stored in `LastStatus` and logged |
| Modeset D0, `modeset.c:623` | Check the inherited pipe and program the retained scaler plan | `modeset.c:639`: End failure is stored in `LastStatus` and logged |
| Allocation destroy, `wddm.c:8483` | Retire the affected primary and invalidate Desired under L before releasing its object | A failed section prevents the free. No fallback executes the protected callback without L |

**Section failures.** A failed `DxgkCbSynchronizeExecution` call grants no ownership.
`Begin`, `Resume`, `End` and destroy preserve conservative state and return an error.
If a `Begin` section failed before it acquired the writer or wrote hardware, it returns BEGIN_REFUSED without a recovery request.
An acquisition failure after ownership was established, including the retirement commit, keeps the writer owned and queues recovery.
They never call their protected function directly after the callback refused to synchronize.
An independent atomic recovery request can be queued from the legal caller context.
Recovery itself must acquire L, or be consumed by the next entry that already owns L.
A failed End synchronization section does not publish an even generation, release the writer, or permit an application allocation to be freed.

The callback can fail because the interrupt is not connected yet.
This documented case also needs an explicit error, not an assumed substitute for the missing lock.
Void power or audio wrappers keep a visible status and rely on the same recovery request.
Returning an error from a destroy operation does not establish physical retention by itself. The driver performs no later unsynchronized use.

**Ordinary flips.** With no writer, the transaction checks the request and programs the requested flip type.
Next-VSync uses `SURFACE_FLIP_TYPE=0`. Immediate uses type 1 and the bounded P' observation described in section 7.1.
Ordinary immediate flips use P': clear pending, requested PRIMARY readback and unchanged generation.
The bridge also requires stable EARLIEST_INUSE, pitch and format before it authorizes a long writer or destroy.
This extra retirement proof waits outside L. It is not part of the ordinary immediate DDI's 50 us observation.
A repeated pending address is promoted with an immediate write when required.
An immediate hardware write without a completion proof enters recovery instead of returning success or an indefinitely busy head.

Admission refusals remain errors, including `IndependentFlipExclusive`. r7 removes the earlier success-without-retirement workaround.
This preserves allocation ownership. It does not prove that Windows avoids its separate 0x119 queue assertion after such an error.
That OS risk remains explicit, as does the need to admit every valid primary promised by the caps.

**Fault recovery.** FAULTING prevents further normal programming.
The protected entry requests HUBP blank and queues deferred notification. It does not wait for a PASSIVE writer or notify recursively under L.
The DPC enters its own section before it reports `DXGK_INTERRUPT_DMA_PAGE_FAULTED`.
Its payload sets `DXGK_PAGE_FAULT_FENCE_INVALID` and `DXGK_PAGE_FAULT_ADAPTER_RESET_REQUIRED`.
The fence and virtual address are zero. Unknown API, stage and bind fields stay unknown.
The chosen node belongs to the first valid supplied context, or node 0 when no such context exists.

The bounded drain observation after notification is a record, not permission to release a still-used surface.
No fabricated CRTC report completes the failed request.
On this part the recorded recovery limitation can lead to bugcheck 0x116 (M53). A queued notification is not proof of successful recovery.

In FAULTING, destroy of the scanned allocation can be refused. The driver retains those objects until stop,
so memory can remain allocated during failed recovery. This is a known limit. A refusal is not permission to free a surface still in use.

**Lab witnesses.** `AMDGPU_WDDM_LAB_FLAVOUR` compiles the event and EARLIEST_INUSE witnesses only into the lab binary.
The lab-only `bc250kmd_cli connector replug` command requires test signing, `LabDiagnostics=1` and an effective administrator token.
Its escape command is 36. Command 35 remains the shared HIP journal upload.
It increments the connector generation, sets Pending and queues the existing DPC and worker.
The worker performs the real disconnect, discovery and reconnect sequence. The escape does not fabricate child notifications.
The release image omits this escape and the raw diagnostic handlers. A linked-image flavour gate checks both builds.
The summary prints one BUSY count for each `BC250_WRITER_KIND` value and the bridge program, retirement and failure counts.
An arm is fresh only when event clearing succeeds and no older flip is pending at arm time.
Excluded or failed arms remain counted. They cannot establish per-request agreement.
The production reader selects the result first. Later witness reads and comparisons never change that result.
The event clear happens before programming, so a release control remains necessary when comparing timing.

**Regression checks.** The tests execute extracted production functions, with synchronization assertions enabled.
They cover old-primary destruction during a writer and prevent a later request from replacing Desired while that writer is active.
The immediate and next-VSync conflict cases require BUSY, no publication and no contention fault.
Rollback checks reject application addresses even when the capture returned one.
They refuse `Begin`, `Resume`, `End` and destroy sections without exempting their protected operations from lock checks.
The DCN tests include the actual source-address helper, so the no-log rule does not depend on a substitute.
The observation boundary test admits no extra sample after the 1 us stall reaches the budget.
The result class uses the successful sample's own start time, while the record also keeps the actual end time.
Host fixtures establish these code properties. Lab results must still check the exact release and lab artifacts.
The replug test runs the actual CLI initializer, escape flags and complete miniport dispatcher with a substituted KMT call.
It checks both build flavours and preserves HIP dispatch. Restoring command 35 fails the test and the compile-time namespace check.

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

## 11. Where the present path writes once the flip is live (2026-09-22, fact M100)

M100 found the consequence this note's step 3 did not address: once `DxgkDdiSetVidPnSourceAddress` has flipped
HUBP0 away from the firmware's framebuffer, nothing scans that framebuffer out any more, and E20's CPU blit
(`WddmPresentBlit`, `wddm.c`) kept painting it anyway. The fix, bc250kmd 0.7.31, is in
`WddmPresentBlit` and `dcn.c`'s new `DcnScanoutMapping`/`DcnUnmapScanout`: once `Device->VidPnFlipEnabled &&
Device->DcnDiverged`, the blit's destination becomes a CPU mapping of `Device->DcnCurrentAddress` instead of
`Device->Framebuffer`, remapped only when that address changes (once a flip, matching M97's "one flip served 60
presents", never once a present) and bounded by the mapped length, never by anything user mode supplied. The
mapping is made and torn down at `PASSIVE_LEVEL` only, from `WddmPresentBlit` and from `WddmStop`/`DcnStop` -
never from `DcnFlipSourceAddress` itself, which `d3dkmddi.h`'s own `_IRQL_requires_max_(PROFILE_LEVEL - 1)` on
`DXGKDDI_SETVIDPNSOURCEADDRESS` allows to run above `DISPATCH_LEVEL`, where `MmMapIoSpaceEx`/`MmUnmapIoSpace`
would be illegal to call. A mapping failure falls back to `Device->Framebuffer` rather than dropping the
present, counted apart from the ordinary case (`WddmSummaryOf`'s new blit-destination line) so a run's evidence
says which of the two reasons a present landed on the POST framebuffer.

**The question this section actually answers: is copying at all the right shape, or should the scanout instead
flip straight to the CDD's own render target** - the allocation `Present` already names as its source, physical
`0x271398000` in the E22 runs (M100) - **so the copy becomes unnecessary?** Read against
`P:\BC-250\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\shared\d3dkmddi.h`, the WDDM2_0
shape this build compiles at (`bc250kmd.h:42-48`):

- `DXGKARG_SETVIDPNSOURCEADDRESS` (`d3dkmddi.h:6363-6381`) is address-only - `PrimarySegment`, `PrimaryAddress`,
  `hAllocation`, `ContextCount`/`Context[]`, `Flags`, `Duration`, and at WDDM2_0 also
  `PrimaryData[D3DDDI_MAX_BROADCAST_CONTEXT]` (`DXGK_PRIMARYDATA`: `hAllocation`/`SegmentId`/`SegmentAddress`,
  `d3dkmddi.h:6355-6360`). No pitch, no stride, no format. `Bc250WddmSetVidPnSourceAddress` (`wddm.c:2719`)
  already reads only `PrimaryAddress`/`PrimarySegment` and ignores the rest, which the struct's shape allows: it
  hands over an address, not a description of what is at it.
- The driver is not purely reactive about which allocation is primary, as it first looked: `DXGKARG_COMMITVIDPN`
  already carries `hPrimaryAllocation`, "driver assigned primary surface allocation handle" (`d3dkmddi.h:6913`),
  an `IN` parameter `Bc250CommitVidPn` (`display.c:587`) does not read today - it only inspects
  `hFunctionalVidPn`, `Flags.PathPowerTransition` and the pinned source mode's `PrimSurfSize`. So there are two
  DDI-level places the KMD could learn a primary allocation's identity ahead of a flip (`CommitVidPn`'s handle)
  or at one (`SetVidPnSourceAddress`'s `hAllocation`/`PrimaryData`), not zero - option B would read one of these
  rather than continue guessing a present's source by position the way `WddmPresentBlit`'s three-arm union
  lookup does today (facts M82).
- `DXGK_ALLOCATIONINFOFLAGS`/`_WDDM2_0` (`d3dkmddi.h:3750-3847`) mark no allocation "primary" at
  `CreateAllocation` time - the nearest thing, `D3DKMDT_SHAREDPRIMARYSURFACEDATA` (`d3dkmddi.h:4167`), is the
  legacy shared-desktop-primary path, and the E22 evidence shows the CDD's surfaces arriving as ordinary
  per-present allocations through `Present`'s own list instead. Either way, this driver only ever finds out
  which allocation is primary by being told, never by asking.
- **The pitch is the real gap, and it is this driver's own, not a documented WDDM one.** `DcnFlipWriteSequence`
  (`dcn.c:328`) never writes `HUBPREQ0_DCSURF_SURFACE_PITCH`, and no `SETVIDPNSOURCEADDRESS` field carries a
  pitch either. Today that is safe only because `OfferSourceMode` (`display.c:363`) offers exactly one source
  mode - `Stride = Device->Post.Pitch` (`display.c:381`) - and `Bc250CommitVidPn` refuses any pinned mode whose
  `PrimSurfSize` differs from `Device->Post.Width`/`Height`, so every allocation dxgkrnl could ever pin as this
  adapter's primary is constrained to 7680 by the mode set, not by anything DCN or the flip DDI enforces.
  Flipping to a CDD allocation on trust of that constraint, rather than reading the allocation's own pitch back
  out of `BC250_WDDM_ALLOCATION_PRIVATE` (`wddm.c:138-146`) the way `WddmPresentBlit` already does for its
  source, would be assuming an invariant instead of checking it.
- `DXGK_FLIPCAPS.FlipIndependent` (`d3dkmddi.h:1972-1980`, a cap this driver already declares at
  `wddm.c:1521`, unchanged since M7 stage A) documents an MMIO flip straight to a redirected surface with
  no compositor copy in between - option B, not option A. The CPU blit this section fixes is the improvised
  piece standing in for a capability the driver already advertises but does not yet fully implement.

**DECISION: ship the mapped-copy fix above now; treat flipping straight to the CDD's own allocation as the next
step, not this one.**

1. It answers the measured defect (M100) inside gates already exercised (`EnablePresentBlit`, `EnableVidPnFlip`)
   with no new register write - `DcnFlipWriteSequence` is untouched - and no new hardware risk: the only new
   operation is a CPU-side `MmMapIoSpaceEx` of memory the existing carve-out check already allows.
2. Option B has an open question this note does not resolve: which of `PrimaryData[]`'s (up to
   `D3DDDI_MAX_BROADCAST_CONTEXT`) entries, or `CommitVidPn`'s `hPrimaryAllocation`, is authoritative for a
   single-target adapter that has only ever measured one broadcast context - unmeasured, because this driver has
   only ever seen `NumSrcAllocations == 0` and read a present's source by position (M82), never by being handed
   a handle directly.
3. Option B needs the pitch question above actually answered from the allocation's own data, not assumed from
   the mode set, before any write sequence - this one or a new one that also programs `DCSURF_SURFACE_PITCH` -
   can flip to an allocation whose stride was never independently confirmed to be 7680.

The two are not in tension: the mapped-copy path is a strict improvement on 0.7.25 either way, and needs no
revisiting if a later step adds a real flip-to-CDD-allocation path - that step would just mean
`Device->DcnDiverged`'s present-time consumer sees fewer presents needing a copy at all, not that this one was
wrong.
