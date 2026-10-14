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
programming sequence. It is held after the witness reads of each sample, so no sample starts past it, and a step
count of 50 ends the loop too. A sample that started inside the bound can end past it: the end time is taken after
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

**Independent flips.** `VidSchiSubmitMmIoFlipCommand` has an internal check that ends in a breakpoint, which is
bugcheck 0x119 without a kernel debugger. It fires when a flip without `FlipRestart` is queued behind an
`IndependentFlipExclusive` entry that is still at the head. Any answer other than success keeps the entry at the
head (facts M904). These flips do arrive: the driver declares `FlipIndependent` and `SupportDirectFlip`, and a lab
session of 2026-10-11 counted 85862 immediate flips, each with `IndependentFlipExclusive`. So an immediate request
with the flag that is refused before anything is written (a scan-out refusal, or a refusal of the programming
before its write sequence ran) gets success with nothing programmed or published, counted `ExclusiveRefused`. The
plane keeps the surface it has, and the next flip replaces it. The OS then releases the surface that was on screen,
which the plane still reads until that next flip. The teardown record (`ScanoutObject`) still names that surface,
so a destroy of it restores the firmware surface before the memory goes. A write into the released memory can show
on screen until the next flip. This is a known hazard of these refusals only, kept from r5b. A programmed immediate
request with the flag has no other route to success than P' (section 7.2).

A vsync request with `IndependentFlipExclusive` keeps its failure answer, counted `ExclusiveFailed`. Success for it
would be worse. Its entry would wait in `FlipWaitVSync` (5) for an address that no `CRTC_VSYNC` report carries, and
the head timeout watches that state, so the end would be a TDR, which is bugcheck 0x116 here.

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
5. The `ExclusiveRefused` hazard (independent flips, above).
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

### 7.2 BD-170: the flip state machine

r6 makes the flip DDI, the writers of the primary and the fault path one state machine. The decisions are in
`driver/kmd/flip_state.h`, which reads no register and keeps no state. `wddm.c` reads the facts and acts on the
answer. This section is the design. Two things decide it: a busy answer stops the source until the client goes away
(facts M902), and P' holds in the call for every immediate flip measured (facts M903).

**The lock L.** L is the interrupt lock of this driver's one MSI message: one message is asked for in the INF and
one is granted (facts M38). Three paths hold L, and no other path does:

1. The kernel's dispatch of the ISR (`Bc250InterruptRoutine`, `pnp.c`).
2. dxgmms2's call of `SetVidPnSourceAddress` above DISPATCH_LEVEL. `VidSchiExecuteMmIoFlip` makes the call through
   `DpSynchronizeExecution` (`VidSchiExecuteMmIoFlipAtISR`, facts M904).
3. This driver's `DxgkCbSynchronizeExecution` sections (`WddmSection`), which run only from callers at or below
   DISPATCH_LEVEL (`dispmprt.h`, `_IRQL_requires_max_(DISPATCH_LEVEL)`).

Code inside L never calls `DxgkCbSynchronizeExecution`: `WddmSection` refuses above DISPATCH_LEVEL
(`STATUS_INVALID_DEVICE_STATE`) and with the callback missing (`STATUS_DEVICE_NOT_READY`), and the caller counts
the refusal. Code inside L does not log, does not arm the VSync timer and does not wait for another path. Every
field of the state machine is written inside L only.

**The two entries of the flip DDI.**

- The high entry is a call above DISPATCH_LEVEL (1616 of the 1641 calls of a03). dxgmms2 already holds L, so the
  transaction runs directly (`FlipsAboveDispatch`). It writes no log line and does not arm the VSync.
- The low entry is a call at or below DISPATCH_LEVEL. It runs the same transaction inside one section. When the
  section fails, nothing runs: the DDI returns the status of the section, counted `LowSyncFailed`, with no arm. The
  log lines and `WddmVSyncArm` come after the section.
- `WddmFlipLocked` wraps the transaction in both entries and keeps the longest time inside L (`FlipLockedMaxUs`).

**The generation.** `PrimarySequence` is odd while a writer is between `Begin` and `End`, and while a flip transaction
runs, so that the vblank DPC's lock-free reads (`WddmReadCompletedPrimary`) never cross either one.

**What is proven about the pipe.** Each fact is a hardware readback inside L at the time that a rule uses it. A
failed read proves nothing.

| Fact | Readback | Used by |
| --- | --- | --- |
| DRAINED | `HUBP0_DCHUBP_CNTL`: `HUBP_BLANK_EN`, `HUBP_TTU_DISABLE` and `HUBP_NO_OUTSTANDING_REQ` all 1, the end state of AMD's `hubp1_set_blank` | edge W-i, only while the writer has not resumed the plane |
| OTG running | `OTG_CONTROL.OTG_MASTER_EN` (`DcnOtgRunning`: running, stopped, or the read failed) | `End` |
| FAULTING | software, set by E5 only | every entry (F0), `Begin`, `End` |

Visibility off is not a quiet state on this part. The driver blanks with `OTG_BLANK_DATA_EN` and the OTG running,
so the HUBP reads and latches as with visibility on. Flips take E2 and E3 as usual, and a real VSync report
carries the latched address, which is "the address that the pixel pipeline would read from if visibility were on"
(`d3dkmddi.md:37739-37741`). A stopped OTG is not used as proof that nothing reads.

**The edges of a flip request**, inside L, in this order (`Bc250FlipEdge`):

- **F0, FAULTING.** E5 was entered. Nothing is programmed, and the DDI answers `STATUS_DEVICE_HARDWARE_ERROR`. The
  OS keeps the previous primary referenced (facts M904) and recovers the adapter.
- **F1, OVERLAP.** The generation is odd with no writer active, or a writer is active with an even generation. L did
  not serialize this call, which facts M904 says cannot happen. E5 with code `0x170004`. It is a check in both
  directions.
- **A, REFUSED.** The admission (`scanout_admit.h`, the pitch and format rules) refused the request. Nothing is
  published. It is a UMD or admission defect, not a pipe state, and not E5. The answer is the refusal status, or
  success for an immediate `IndependentFlipExclusive` request (`ExclusiveRefused`, section 7.1).
- **W, a writer is active.** Nothing is programmed now. The request is published as Desired (a deposit), and the
  teardown record follows it:
  - W-v, a next-VSync request: success. `End` or the writer's unblank programs it, and the first real VSync report
    after that carries it.
  - W-i, an immediate request while the plane is DRAINED and the writer has not resumed it: success. Nothing
    reads the plane, and the writer's unblank or its `End` programs Desired before any read.
  - W-u, any other immediate request: success with the deposit, as W-v and W-i. A writer conflict is a normal
    state and not a fault, so W-u does not enter E5. It has its own count and a bounded record (below, "W-u: the
    deposit and its hazard").
- **E, PROGRAM.** No writer, an even generation. With the flip gate closed (`EnableVidPnFlip` 0) the request is
  published only and gets success: the software timer reports Desired. A request for the address already published
  and not pending gets success with nothing programmed. Every other request is programmed: E2 with
  `SURFACE_FLIP_TYPE` 0 for a next-VSync request, E3 with type 1 for an immediate one, then P' is observed (section
  7.1).

The result of a programming (`Bc250FlipResult`):

| Result | When | Answer |
| --- | --- | --- |
| REFUSED | nothing was written (`DcnFlipSourceAddress`'s own checks) | A: the teardown record goes back to the previous object, nothing is published |
| FAULT_PROGRAM | the write sequence ran and failed (an MMIO failure, or no acknowledge of the update lock) | E5, code `0x170001`: a failure is unknown, never proof that nothing was written |
| UNPROVEN | an immediate request without P' | U: E5, code `0x170002`, Desired published, because the hardware has it |
| SUCCESS | a next-VSync request, or an immediate request with P' | success |

**U is E5.** U is an immediate request that edge E programmed and whose completion the call cannot show: E3 without
P'. It is never
`STATUS_DEVICE_BUSY`, because a busy answer stops every flip of the source until the client goes away (facts M902),
and it is never a longer poll at DIRQL. Success would let the OS release a surface that the plane may still read.
P' held in the call for every immediate flip measured, the vertical blank included (facts M903), so U is a fault
and not a state that the OS meets in normal operation.

**W-u: the deposit and its hazard.** An observation that expired, an MMIO failure, a writer conflict and a stopped
pipe stay distinct, and only a hardware fault gets the recovery notification. W-u is a writer conflict. The request
is deposited (Desired), the answer is success, and the writer's unblank or its `End` programs it. The transaction
counts the edge (`FlipEdges`) and writes a record into a ring of 8 (`WddmWuRecord`): the code `0x170005`, the
request (card address and flags), the generation, the writer (`BC250_WRITER_*` in `flip_state.h`, given to `Begin`),
the deposit sequence and the time of the deposit. At the writer's `End`, `WddmWuClose` gives each open record of
that writer its time to `End` (`ToEndUs`, the longest in `WuToEndMaxUs`) and how the deposit left the writer
(`Bc250WuEnd`): programmed by the unblank, programmed by `End`, refused by `End` (Desired no longer fits), `End`
failed, replaced by a newer deposit, or nothing to program. The summary prints the count, the records and the
longest time. `0x170005` is the code of the record only. It is not an E5 code.

After a success the OS releases the surface that was on screen at once (section 7.1, point 2), and the client can
write it again. The plane reads that surface until the writer blanks the plane, until the writer puts a surface of
its own on the plane, or until `End` programs the deposit (with the next-VSync type, so one more frame). The
writers do not all blank for their whole duration. In the source of this revision:

| Writer | The plane reads the previous surface | Bound |
| --- | --- | --- |
| Link commit, `ConnectorLinkCommit` | From `Begin` (`connector_link.c:779`) to the plane blank in `DisableOwned` (:599, visibility off :601). Between them run the surface capture (:784), the PSP clock session (:788), the audio stop (:789) and the HDMI hand-off (:795). From the blank to the surface step of `Apply` (:724-725) the plane is drained (W-i). After it the plane reads the driver's own buffer (`DcnRuntimeSurface`), or the captured surface or the deposit (`DcnLinkSurfaceRestore`). The output unblanks at :729-730. | `ClockSessionEnter` waits at most for the rest of `LINK_DEADLINE_US`, 2 s (:22), counted from `Begin(c)` at :759 (:82, :86-92). A failure before the blank adds the cleanup's `LINK_CLEANUP_US`, 300 ms (:23). About 2.3 s. |
| Link resume, `ConnectorLinkResume` | From `Begin` (:893) through `ClockSessionEnter` (:895) to `DisableOwned` (:898). | 2 s, as above. |
| Link disable, `ConnectorLinkDisable` | From `Begin` (:626) through `CloseAux` (:598) to the plane blank (:599). | `CloseAux` runs on `LINK_CLEANUP_US`, 300 ms (:215-224). |
| Link suspend, `ConnectorLinkSuspend` | From `Begin` (:879) to `DcnRuntimeSurface(FALSE)` (:880), which first blanks the output and the plane (`dcn.c:1202-1205`). An inherited link goes to the firmware surface (`DcnRestorePostDisplay`). | The 50 ms poll, `BC250_DCNFLIP_POLL_MAX_US` (`dcn.c:321`). |
| HDMI start, `HdmiLinkStart` | The whole writer (`hdmi_link.c:141-169`): no blank. The prediction runs on a shadow with a simulated clock (`SHADOW_MAX_US`, `bc250_hdmi_link.c:586`). The timing check after the ATOM run (`hdmi_link.c:150`) measures TMDS over 8 frames. | The ATOM run, 100 ms of elapsed time (`bc250_hdmi_link.c:511`, :566-567), and the TMDS measurement, 500 ms (`display_tmds.h:82`). About 0.6 s. |
| HDMI stop, `HdmiLinkStop` | The whole writer (`hdmi_link.c:221-225`): no blank. | A restore of the saved registers with a readback each, and no wait (`bc250_hdmi_link.c:366-386`). |
| Modeset commit, D3 and D0 (`modeset.c:477`, :573, :614) | The whole writer: no blank. When the viewport grows (`modeset.c:407-411`) and at D3 (:575), the plane goes to the firmware surface first. | The firmware-surface poll, 50 ms (`dcn.c:1011`), and the pipe update, `MODESET_PIPE_BUDGET_US` 1 ms (`modeset.c:18`). |
| Destroy restore, `WddmDestroyRestore` | From `Begin` (`wddm.c:8482`) to the firmware or runtime surface (`DcnRestorePostDisplay`, :8492). | The 50 ms polls (`dcn.c:321`). |

So a W-u success can leave the previous surface on screen for at most about 2.3 s (a link commit) plus one frame
after `End`. Every scan-out surface is in VRAM (admission) and the plane only reads, so the result is a wrong
picture for that time: a frame that the client writes again, or memory that a destroy gave back. It is not a fault
and not a write. `ToEndUs` and `WuToEndMaxUs` give the time on the lab.

**The lab witnesses.** The lab flavour (`AMDGPU_WDDM_LAB_FLAVOUR`, the BD-167 switch) compares P' with two other
witnesses for every immediate request that edge E programmed. The release flavour compiles none of it. Before the
programming, `DcnWitnessArm` clears the event bits of `SURFACE_FLIP_INTERRUPT` with the sequence of a04, and marks
the arm fresh when the readback shows them clear. Then the shipping reader decides (P', `Bc250FlipResult`, the
result count), and that decision is recorded as such (Q). After the decision only, `DcnWitnessRead` reads the flip
event (E), then `SURFACE_EARLIEST_INUSE` low word first and high word first (R1 and R2, each masked to its address
field). `WddmWitness` counts per request: a failed read, a stale arm (not fresh), Q against E (fresh arms only), Q
against R1, E against R1 (fresh arms only), and R1 against R2. The first 16 requests with a disagreement are kept
whole (both reads, the request's card and normalized address, the generation, the raw words, the flags, Q, fresh,
E), later ones are counted as lost, and the last request is always kept. The witnesses never change the answer.

**The writers.** A writer changes the plane outside the flip DDI: a modeset commit, D3 and D0, the HDMI link start
and stop, the connector link disable, commit, rollback, suspend and resume, and the destroy restore. It runs at
PASSIVE_LEVEL and takes L for short sections only. Its long work runs outside L.

- **`Begin`** (`WddmPrimaryExclusiveBegin`). In FAULTING it refuses at once (`WriterFaulting`), and the writer
  reports busy as before. While another writer holds an odd generation it tries again, 200 times 10 us apart, with
  the stall outside L, then refuses. A failed section refuses and is counted (`WriterHigh`, `WriterSyncFailed`).
  Otherwise it makes the generation odd, sets `WriterActive` and clears the resume mark and the deposit.
- **`Resume`** (`WddmWriterResume`), before every unblank of a writer (`DcnRuntimeSurface`,
  `DcnLinkSurfaceRestore`). It sets the resume mark, so that no DRAINED readback counts for this writer from now
  on, and hands a deposit, if there is one, to the unblank site. It does nothing with no writer active (the stop
  path) and nothing above DISPATCH_LEVEL (the bugcheck callback restores the firmware surface through the same
  site). The site programs the deposit in place of its own surface when the deposit fits the surface checks of the
  mode that it restores. When it does not fit, the site answers `STATUS_DEVICE_NOT_READY`. After the poll, the site
  reports the deposit as programmed (`WddmWriterDepositProgrammed`), and `End` does not program it again unless a
  newer deposit came meanwhile.
- **`End`** (`WddmPrimaryExclusiveEnd`). It owes a flip when a deposit was not programmed by an unblank, or when
  Desired is a next-VSync success that no report has carried yet (`DesiredOwed`). With the gate open and not in
  FAULTING, `Bc250WriterEnd` decides:
  - Nothing owed: nothing.
  - The OTG running: Desired is programmed with the next-VSync type, so that the next real report carries it. A
    programming refusal (Desired no longer fits the source) drops it, counted `OwedInvalidated`. A failed write is
    E5 with code `0x170001`.
  - The OTG stopped, or its read failed: E5 with code `0x170003`. With the gate open, no timer makes a `CRTC_VSYNC`
    (`WddmVSyncArm`), so nothing would complete the owed flip.

  In FAULTING `End` programs nothing. With the gate closed `End` programs nothing, and the timer reports Desired as
  published. Then the writer's invalidation applies (the plane is back on the firmware surface), except when `End`
  programmed Desired over it, or when Desired is owed with the gate closed. `End` clears the writer marks and makes
  the generation even. An `End` whose section fails runs without L, as every End did before r6, and is counted: a
  generation left odd would turn every later flip into a deposit.

**Desired and its lifetime.** Desired is the newest request that this driver answered success for, programmed or
deposited. It holds no reference of its own. The OS holds the allocation while its flip entry waits, and then as
the primary on screen until a newer flip completes. Desired ends:

- At a newer success, inside L.
- At the report that carries it. `WddmNotifyRoutine` clears `DesiredOwed` when the reported address is Desired
  (`OwedReported`).
- At `DestroyAllocation` of its object. The destroy restore is a writer: `Begin`, then inside L the teardown record
  is taken and Desired is dropped, then the firmware surface goes back outside L, then `End` programs a deposit that
  came meanwhile. When `Begin` is refused, the restore runs as before r6, without L, counted
  `DestroyUnsynchronized`.
- At D3, stop or teardown. dxgmms2 flushes the flip queue first (`VidSchFlushAdapter`, `VidSchFlushDevice`, facts
  M904). At the start of D3, before the power writers, `WddmFlipFlushed` clears `DesiredOwed` (`FlipFlushes`), so
  that their `End` does not program a flip into a pipe that goes down.
- At an OS cancel of a waiting entry (`FlipRestart`). Desired can then name a cancelled allocation until the newer
  flip programs. Admission puts every scan-out surface in VRAM, and the plane only reads it.

**The vblank report.** With the gate open, the only `CRTC_VSYNC` source is the DCN `VUPDATE_NO_LOCK` interrupt
(`WddmDcnVsync`). A report carries Desired when the hardware has latched it (`WddmReadCompletedPrimary`: an even
generation, and `DcnFlipPending` clear for Desired). Otherwise it carries the address that the plane still reads,
and only when that is not Desired. A vblank while the generation is odd is not reported. r5b's busy machinery is
removed: no outstanding request, no 250 ms stuck mark, no report of the driver's own vblank with the VSync off.

**E5.** E5 is entered inside L, from a flip (F1, U, a failed write) or from `End` (OTG stopped, a failed write):

1. `WddmFlipFault` sets FAULTING. It stays set until stop or reset. A second fault is counted only (`FlipFaults`).
2. It requests a blank: one read-modify-write of `HUBP0_DCHUBP_CNTL` that sets `HUBP_BLANK_EN` and
   `HUBP_TTU_DISABLE` (`DcnFaultBlank`), with no wait.
3. It does not notify inside L. It marks the notification and calls `DxgkCbQueueDpc`. A missing callback is
   counted (`FaultNotifyFailed`). The DPC (`WddmFlipFaultDpc`, from `Bc250DpcRoutine` at DISPATCH_LEVEL) makes the
   notification once per FAULTING, through `WddmReport`'s own section, which also queues the DPC that pairs it
   (`d3dkmddi.md:2467-2481`). So no `DxgkCbNotifyInterrupt` call runs inside dxgmms2's own DIRQL flip call.
4. The notification is `DXGK_INTERRUPT_DMA_PAGE_FAULTED`, a GPU error that needs OS recovery
   (`d3dkmddi.md:46003-46007`). The payload:
   - `PageFaultFlags`: `DXGK_PAGE_FAULT_FENCE_INVALID | DXGK_PAGE_FAULT_ADAPTER_RESET_REQUIRED`.
   - `FaultedFenceId` 0 and `FaultedVirtualAddress` `D3DGPU_NULL`, because the fault has another cause.
   - `FaultErrorCode.IsDeviceSpecificCode` 1, with the code of the fault (table below).
   - The API sequence number, the pipeline stage and the bind table entry: `UNKNOWN`.
   - `PageTableLevel` 0 and `FaultedProcessHandle` NULL.
   - `NodeOrdinal`: the node of the first context that the flip call carries when it is a context of this driver,
     else node 0 (the 3D node, also for a fault from `End`). `EngineOrdinal` 0, because each node has one engine.
5. The DDI answers `STATUS_DEVICE_HARDWARE_ERROR` for the call that entered E5 and for every later call (F0).
   `Begin` refuses in FAULTING, and `End` programs nothing.
6. After the notification the DPC reads `HUBP0_DCHUBP_CNTL` at most 50 times, 1 us apart, and records DRAINED, or
   UNKNOWN after the 50th read or at a failed read (`FaultDrain`, `FaultDrainReads`). The driver releases nothing
   in either case: the OS still holds every surface that it did not see complete.

| Code | Cause |
| --- | --- |
| `0x170001` | a write sequence ran and failed (a flip, or `End`) |
| `0x170002` | U: an immediate request without P' |
| `0x170003` | `End` owes a flip and the OTG is stopped, or its read failed |
| `0x170004` | F1: the generation and the writer mark disagree |

`0x170005` is not an E5 code: it is the code of a W-u record (above).

On this part the recovery is bugcheck 0x116 (facts M53). That is the terminal outcome here, not evidence that the
path recovers elsewhere.

**The checks.** `driver/kmd/test/run_flip_state.ps1` drives every rule of `flip_state.h` and breaks each one in a
copy (14 mutations). `driver/kmd/test/run_vidpn_flip.ps1` extracts the actual flip transaction, writers, E5, its
DPC, the destroy restore and the vblank report from `wddm.c`, runs each case at both entries with a model of L that
fails a check for a log line or a nested section inside L, and breaks each rule in a copy (29 mutations, among
them the high entry taking a section, W-u entering E5, a W-u without its record or its End, End ignoring the OTG,
Invalidate over a programmed Desired, and a fault that notifies twice). With `-Lab` it compiles the lab flavour,
checks the witnesses, and breaks them in 4 more copies: a witness read before the decision, a witness that changes
the answer, a stale event compared, and a disagreement past the bound that is not counted.
`driver/kmd/test/run_dcn_flip.ps1` breaks each term of P', the bound held after the reads, and the overrun count (4
mutations). `tools/quality/quick.ps1` runs all of them, and every mutation must fail.

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
