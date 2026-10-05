# M430 - DCN lock acknowledgement and flip-control preservation

2026-09-24. Source/host/build validation only. No lab access, installation, PnP,
DWM restart, OS restart or firmware access during this work.

The BIOS report prompted a review of the current KMD and AMD v6.18 sources.
DcnFlipWriteSequence now reads UPDATE_LOCK_STATUS before control/address writes,
with at most ten 1 us stalls and eleven status reads. A missing acknowledgement
returns STATUS_IO_TIMEOUT after unlocking, without address or trigger writes.
DCSURF_FLIP_CONTROL changes only SURFACE_FLIP_TYPE to synchronous; other bits
are preserved. HIGH precedes LOW, then unlock and the existing trigger.
Explicit stalls are bounded; total MMIO time is not measured by this host test.

AMD reference: optc1_lock (dcn10_optc.c) uses REG_WAIT with 1 us / 10 retries;
dcn201_tg_funcs selects it. hubp2_program_surface_flip_and_addr uses REG_UPDATE
for SURFACE_FLIP_TYPE. Our WDDM adaptation returns an error on timeout and uses
KeStallExecutionProcessor, never sleep/allocation at the high-IRQL flip DDI.
PROVENANCE: AMD display sources in torvalds/linux v6.18, MIT; reference hashes
in references.json. No proprietary BIOS code was imported.

The reported D3 absence from Linux is incorrect if applied to the full flip
path: dcn201_tg_funcs selects optc2_program_manual_trigger; core/dc.c invokes
it after unlock for an eligible address update. dc_hw_sequencer.c likewise
places it after the unlock in its fast sequence. This agrees with existing
unit A trace M87. The trigger remains; no hardware A/B necessity is claimed.
optc1_unlock does not poll for lock-status deassertion in this source version.

## Validation

- Actual DcnFlipWriteSequence body extracted verbatim into an MMIO model.
  Immediate ACK and every delay through ten stalls, both logging modes, all
  32 individual control bits, address latching and post-unlock trigger tested.
- Positive result: 30350 checks, zero failures, exit 0.
- Replace status-ACK test with request-bit test: 3858 failures, exit 1.
- Replace flip-control RMW with whole-register zero: 682 failures, exit 1.
- Missing ACK: no address/trigger write; lock released after ten stalls.
- Initial harness compilation failed because _In_ duplicated CRT SAL. Removed
  the redundant definition; initial log preserved. Not a KMD compile failure.
- Full WDK compile/link, stack budget, catalog and signing pass, exit 0.
  Development SYS SHA256:
  `F40856F09B221FE623C6EED33FF08A540B8810589BB3714532922CB56F5ADCBC`.
  Private build: scratch/build/dcn-ack-preserve-dev/package. Same development
  version 0.7.133.0; NOT the installed 0.7.133.1 artifact and never installed.

Reproduce: run driver/kmd/test/run_dcn_flip.ps1; build with driver/kmd/build.ps1
-Kits P:/bc-250/toolchain/nuget -Out <fresh workspace scratch directory>.
For negative controls pass the altered dcn.c through the runner's -Source;
the generated mutated fixtures are retained here. Raw logs are unedited.

## Capacity consequence

Current WddmMemoryLayout subtracts the firmware framebuffer, 32 MiB tail, and
1/32 of remaining space for page tables (64 KiB alignment). With the current
7680-byte pitch, 1200 lines, framebuffer at offset zero and an assumed 12 GiB
carve-out, application capacity projects to 12440829952 bytes = 11.58642578 GiB.
At 8 GiB the same formula gives 7.71142578 GiB, matching the established layout.
This is source arithmetic, NOT a changed carve-out or runtime measurement.
12 GiB of model allocations resident in the local segment cannot be accepted
merely because a firmware option called 12G exists. Reservations and other
GPU allocations also consume capacity. The requested 12 GiB remains open.

## Limits and next hardware work

No physical lock timing, visual improvement, uninterrupted DWM/input or
concurrent escape/DDI serialization is established here. DCSURF_SURFACE_CONTROL
still uses its existing whole-register zero; this patch covers FLIP_CONTROL.
OTG0's inherited lock selection is unchanged. A hardware test must preserve
DWM/content controls, measure ACK/flip behavior and check the existing display
profile before promoting a distinct candidate. D4-D7 remain separate work.
M429 native SMU owner/backend/startup integration and all M9 acceptance gates
remain open. Deployment stays as recorded in workspace STATE.md.
