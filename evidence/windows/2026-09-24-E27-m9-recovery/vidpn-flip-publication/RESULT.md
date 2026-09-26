# M431 - Publish only successfully programmed VidPn flips

2026-09-24. Source/host/build only; no lab operation or firmware access.
This extends M430; its evidence remains unchanged. M430 alone is not a deployment
candidate because the caller originally ignored the new timeout result.

## Implementation

Bc250WddmSetVidPnSourceAddress propagates DcnFlipSourceAddress's failure without
changing PrimaryAddress, PrimarySegment or Flips. A later submission of the same
address still reaches the hardware path. Successful programming publishes the
new address; hardware FLIP_PENDING still gates completion, so a successful MMIO
write is not itself called a completed scanout.

PrimarySequence uses a nonblocking compare/exchange to admit one programmer at
a time, odd while programming and even when published. A competing caller gets
STATUS_DEVICE_BUSY; there is no spin behind a preempted caller at DIRQL. The
vsync snapshot brackets its address/pending reads with the generation. A pending
flip, active programmer or changed generation defers the report to another tick.
Both software and hardware vsync readers use this helper. No automatic dxgkrnl
retry is claimed: the test proves a subsequent identical DDI request works.

DcnFlipEscape refuses diagnostics while VidPnFlipEnabled owns OTG0. The normal
quiesced stop still calls DcnFlipCore directly to restore the firmware address.
DcnLockTimeouts counts failed ACK waits and is exposed by the existing summary
log. No escape ABI or monitor schema change is required for that counter.

DCSURF_SURFACE_CONTROL now preserves fields other than PRIMARY_SURFACE_TMZ and
PRIMARY_META_SURFACE_TMZ; these two stay clear for our unprotected graphics
surfaces. AMD hubp2_program_surface_flip_and_addr does this even when meta_addr
is zero, so the claim that it writes this register only for DCC was too broad.
Offsets and masks remain from the vendored AMD/generated headers.

## Validation

- Updated actual DCN sequence + escape: 31764 checks, zero failures, exit 0.
  Covers delayed ACK, field preservation, address latch/order, timeout counter,
  and diagnostic flip/restore refusal while VidPn is enabled.
- Actual DDI, completed-address sampler and WddmDcnVsync: 61 checks, zero failures.
  Covers low/high IRQL paths, failed flip retaining old address, identical-address
  retry, duplicate successful address, pending flip, nested writer rejection,
  and a whole programming transaction during the pending-bit read.
- Discard hardware failure mutation: 18 failures, exit 1.
- Omit final generation comparison mutation: 4 failures, exit 1.
- Full WDK build, link, stack-budget check, catalog and signing pass, exit 0.
  Development SYS SHA256:
  `1C0AD04715955AD51950C550B42C842618FB4131E720E475437F0DBA145EDFF2`.
  Private path scratch/build/dcn-publication-dev/package, version 0.7.133.0.
  Never installed; installed 0.7.133.1 SHA remains in workspace STATE.md.

Run driver/kmd/test/run_dcn_flip.ps1 and run_vidpn_flip.ps1, then the normal
full driver build to reproduce. Mutated generated fixtures and raw logs remain
here. These are deterministic callback interleavings, not real multithread or
hardware timing tests. The MMIO backend, OS callback behavior and actual screen
remain hardware acceptance work. DWM survival/input/visual correctness are not
established by these tests.

## Source contract and remaining work

Local MS ref/ddi-display/d3dkmddi.md, DXGKDDI_SETVIDPNSOURCEADDRESS: return an
Ntstatus error on failure and report the effective scan address via CRTC_VSYNC;
IRQL ceiling PROFILE_LEVEL - 1. The document does not promise seamless recovery
or retry for an arbitrary failed presentation. Its special warning about failed
SharedPrimaryTransition is retained as a limitation, not hidden by success.

AMD Linux v6.18 revision and reference hashes are recorded in adjacent
../dcn-lock-ack-review/references.json. No proprietary code imported.

Still open: hardware ACK-frequency/timing measurement, live-screen acceptance,
OTG_GLOBAL_SYNC_STATUS enable-vs-ISR synchronization, explicit lock selection for
future modeset/ODM, and dynamic geometry across validation, CPU mapping and
reporting. DcnFlipPending's existing read-error behavior is unchanged. No M9
startup, memory-lifetime, preemption or performance gate is closed by this work.
