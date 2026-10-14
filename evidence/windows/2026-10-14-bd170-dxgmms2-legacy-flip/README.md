# dxgmms2 on the legacy MMIO flip path (2026-10-14, BD-170)

A static reading of how Windows' video scheduler (`dxgmms2.sys`) completes a flip on the legacy
`DxgkDdiSetVidPnSourceAddress` path, and what it does when the DDI returns a status other than success. The
driver's immediate flip (BD-170) needs this rule, because the rule says when the surface that the flip replaced
is free again. The fact is M898 in [`docs/facts/display.md`](../../../docs/facts/display.md). The design that uses
it is [`docs/design/vidpn-flip.md`](../../../docs/design/vidpn-flip.md), section 7.1.

## Why a reading of the binary

The DDI page documents the return value only as `STATUS_SUCCESS` on success and otherwise "one of the error codes
defined in Ntstatus.h" (local `ref/ddi-display/d3dkmddi.md:12791`, WDK 10.0.26100, and the public page
<https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_setvidpnsourceaddress>).
It gives no completion rule for an immediate flip on this path, and no meaning for `STATUS_DEVICE_BUSY`
(0x80000011). Everything below comes from the binary, not from the documentation.

## What was read

| Build | Where it applies |
|---|---|
| `dxgmms2.sys` 10.0.22621.6199 | the lab's Windows 11 23H2 (22631) |
| `dxgmms2.sys` 10.0.26100.9549 | Windows 11 24H2 and 25H2 builds that carry this image |

The reading is by function name, with Microsoft's public symbols. The two builds agree on every point below.
Offsets, instruction listings and the disassembly stay outside this repository, because they are derived from
Microsoft binaries.

Four tester minidumps from Windows 11 26200 systems (BD-135) list the 26100.9549 image: its `TimeDateStamp` and
`SizeOfImage` are in each of them, and the 22621.6199 stamp is in none. The lab's own 26200 system can carry
another update. Its HLK run is the check for that build, and the lab arm a03 is the check on 22631.

## What the OS does

1. Without the MPO DDIs, `VidSchiExecuteMmIoFlip` calls `DxgkDdiSetVidPnSourceAddress` through
   `DpSynchronizeExecution` (`VidSchiExecuteMmIoFlipAtISR`), so the calls are serialized with the interrupt.
2. An immediate flip's entry is set to `FlipWaitComplete` (11) before the call. On a success status the OS
   dereferences the allocation that was on screen, references the new one and completes the entry
   (`VidSchiCompleteFlipEntry`), all before it continues. It does not wait for a `CRTC_VSYNC` report.
3. A vsync flip's entry waits in `FlipWaitVSync` (5). A `CRTC_VSYNC` report completes it only when its
   `PhysicalAddress` is the flip's address (`VidSchiIsExpectedVSyncCookie`).
4. `VidSchiCheckHeadTimeout` watches two entry states only: `FlipWaitVSync` (5) and the MPO3 Hsync wait (15).
   An entry that stays in one of them too long goes to `VidSchiReportHwHang`, which is a TDR.
5. A status other than success leaves the entry not completed at the head of its flip queue, with the previous
   primary still referenced:
   - For `STATUS_DEVICE_BUSY` the OS calls `VidSchiKeepInterruptTargetEnabledForFlip`. That function sets the
     source's interrupt target only for an entry with an MPO plane. For a legacy primary flip it does nothing.
   - A failure status is recorded as a failure (`STATUS_INVALID_PARAMETER` with a triage event).
   - In both cases the VSync DPC (`VidSchDdiNotifyDpc`, `VidSchiProcessDpcVSyncCookie`,
     `VidSchiExecuteNextFlipQueueEntry`) executes the head entry again, once a VSync. For a busy entry it goes
     through `VidSchiCheckPendingFlipsForThisEntry` first.
   - Nothing counts the repeats or limits their time. No state of such an entry is in the head timeout's two
     states, so no TDR comes from it, and the device is not marked as in error.
   - Later flips of the same queue wait behind it.
   - The idle count in `VidSchiProcessDpcVSyncCookie`, which lets the OS switch the VSync interrupt off, does not
     look at such an entry. It looks at the VSync waiters and at new submits. So with no waiter and no new
     submit, the OS can switch the VSync off while the entry is at the head, and then the repeats stop until
     something enables the VSync again. Whether the lab's flips meet that condition is not known from the reading.
6. Without a success, only the OS ends such an entry, by completing it as canceled:
   - a newer flip that the OS marks `FlipRestart` (`VidSchiRestartQueuedFlip`), which completes the busy head with
     entry status 6
   - a change of the flip device (`VidSchiSetFlipDevice`, `VidSchiCompletePendingFlip`)
   - a device or adapter teardown, and a suspend (`VidSchFlushDevice`, `VidSchFlushAdapter`)
   - a TDR (`VidSchiReportHwHang`) and `VidSchiCancelIndependentFlips`
   - a device that is in error, or an adapter in recovery, both checked before the DDI

   A mode change does not cancel it. The OS releases the previous primary only at a success status, that is at a
   real completion of the flip.
7. The legacy path has no deferred completion and reads no driver capability for one. The MPO3 output flag
   `HsyncInterruptCompletion` (`d3dkmddi.md:25271-25273`) puts an immediate flip into the Hsync wait (15), which
   the head timeout of item 4 watches. The legacy path writes the ETW event `MMIOFlip`.

## What the driver does with it

- An immediate flip returns success only when the hardware has taken it (item 2): the full flip-pending predicate
  of `hubp1_is_flip_pending` holds within 50 us of the programming.
- Otherwise the DDI returns `STATUS_DEVICE_BUSY`, and the OS asks again once a VSync (item 5). Each call again
  costs one sample of the predicate.
- Because the OS sets no bound (items 4 and 5), the driver marks a request that is not complete 250 ms after its
  programming as stuck. It counts it and keeps the answer `STATUS_DEVICE_BUSY`: success would release a surface
  that the hardware can still read, and a failure status repeats in the same way. The cancel events of item 6 end
  the stall.
- The OS can switch the VSync off with a busy entry at the head (item 5). So the driver takes the mark at its own
  vblank too, which runs whether the OS's VSync is on or off. The bound is 250 ms plus one refresh, with no call
  from the OS needed. While a request is outstanding, the driver reports each vblank as `CRTC_VSYNC`, with the VSync
  off too. A busy answer needs that vblank to be live. Without it the driver answers success, as before BD-170.
- The driver counts each time the OS switches the VSync off while a busy request is outstanding, and it keeps the
  last 16 calls that switch the VSync on or off. The lab arm a03 runs a control with an idle DWM and records them.
- The MPO3 route of item 7 is the option for a later train. It completes the flip at the next interrupt after the
  latch, with no busy retry. Its timeout is a TDR, and on this part a TDR is bugcheck 0x116 (facts M53).

## An assertion behind an independent flip

`VidSchiSubmitMmIoFlipCommand` has an internal check that writes a watchdog entry with code 0x119
(`VIDEO_SCHEDULER_INTERNAL_ERROR`) and then executes a breakpoint, which is a bugcheck on a system without a
kernel debugger. It fires when a new flip entry that is not the next one to execute, and that does not carry the
OS's restart marker, is queued behind an entry with `IndependentFlipExclusive`. The check reads neither the
DDI's status nor the state of the head entry. A busy head makes a queue of more than one entry last longer, so
the reading cannot exclude that a busy immediate independent flip with a vsync flip behind it reaches this check.
The restart marker is `FlipRestart`. The OS takes it from `D3DKMT_PRESENTFLAGS.FlipRestart` in
`DXGCONTEXT::Present`, or for an independent flip from its own restart choice (`PrepareIndependentFlipToken`).
A queued flip with the marker cancels the entries before it with entry status 6 (`VidSchiRestartQueuedFlip`, then
`VidSchiCompleteFlipEntry`). That cancel does not release the previous primary: only the completion paths
(`VidSchiProcessVsyncCompletedFlipEntry`, the immediate completion in `VidSchiExecuteMmIoFlip`) change which
allocations are referenced as on screen. A queued flip without the marker waits behind a busy head with no bound.
`VidSchiSubmitMmIoFlipCommand` does not wait for the head, and only the 64-entry ring of the flip queue limits it.

The flags of the call: `VidSchiExecuteMmIoFlip` sets `FlipImmediate` only for an entry that the OS marks
true-immediate (from `PrepareIndependentFlipToken` or an MPO3 present) or for one present mode of the source. Every
other legacy flip gets `FlipOnNextVSync`, at SyncInterval 0 too. A search of `DXGCONTEXT::Present` found no writer of
the true-immediate mark.

These flips arrive at this driver: it declares `FlipIndependent`, and a lab session of 2026-10-11 counted 85862
immediate flips, each with `IndependentFlipExclusive`. So the driver gives an immediate request with
`IndependentFlipExclusive` no answer other than success, also when another writer holds the primary generation and
when the scan-out checks or the programming refuse it. Then nothing is published, and the next flip replaces the
request. A vsync request with the flag keeps its busy or failure answer: success would put it into the vsync wait,
which the head timeout of item 4 watches. Whether the OS queues a flip behind such an entry is not read.

## What is not shown

- No lab run is in this reading. The lab arm a03 measures, on 22631, the outcome of each immediate flip, the
  retries of a busy one and their age.
- The reading does not tell how often a real game meets a busy answer. That is a03's outcome mix.
