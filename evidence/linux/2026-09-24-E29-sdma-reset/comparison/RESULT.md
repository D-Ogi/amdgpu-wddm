# M389 - Physical engine control and RLC policy mismatch

Unit A, 2026-09-24. The E29 timeout trace explicitly selects instance1 in
amdgpu_sdma_reset_engine. The unpinned client ring0 is an entity index, not
proof of physical SDMA0. amdgpu_ctx.c initializes that entity against a scheduler
list. M388 proves new work after reset but its first post-control did not alone
attribute that work to the reset engine.

## Physical engine positive control

The live mask was3 and fence info listed sdma0 and sdma1, without page queues.
A new positive control ran with source-derived amdgpu_sdma_sched_mask=2 and
restored mask3 in the same terminal SSH run. SDMA0 signaled/emitted stayed23/23;
SDMA1 advanced57/57 to71/71. Marker1373246004 matched, fence completed, native0.
This closes the physical-engine attribution gap for SDMA1; no second timeout,
reset, module reload or OS restart was performed. No repeated-reset claim.
Raw log and source wrapper are preserved; validation.json contains counters.

## Source and register comparison

PROVENANCE: Linux amdgpu v6.18.52, AMD MIT. Exact RLC/context/fence/nv sources
and retrieval URLs/hashes are preserved. Windows source snapshots are included.

The56 accesses between stop_queue entry and restore_queue return decode through
original AMD headers using tools/regcalc. decoded-reset-all-ips.csv resolves GC
and NBIO, including mmBIF_SDMA1_DOORBELL_RANGE; the initial decoded-reset.csv
left its two NBIO accesses unnamed. No register name was guessed.

Linux clears RB_ENABLE/IB_ENABLE, requests FREEZE and observes FROZEN, sets HALT,
clears UTC_L1_ENABLE, asserts/readbacks/releases GRBM_SOFT_RESET.SDMA1, then
unfreezes and restores pointers, ring base, doorbell and enable/cache state.
The reset readback stays asserted across approximately52us in this trace.

There are zero writes to mmRLC_SAFE_MODE in this window. Reads of mmRLC_CNTL
return enabled. Exact amdgpu_rlc.c gates safe-mode commands on GFX clock-gating
flags; nv.c sets cg_flags=0 and pg_flags=0 for GC10.1.3/10.1.4. This is consistent
with the observed command omission, not direct introspection of the live flags.

Windows bc250_gmc.c also sets cg_flags=0, but bc250_gfx_rlc_safe_enter explicitly
ignores that policy and sends a request. Its reset_for_reload caller uses two
such scopes, resets both engines and re-quiesces them before retiring backing;
Linux restores a retained live queue. Thus the successful Linux path does not
validate the extra Windows safe-mode commands or its firmware-reload lifecycle.

## Next change and acceptance

Honor the source-derived clock-gating condition in the Windows safe-mode helper.
Retain paired entry/exit and ACK checks when a request is actually needed. Adapt
host models to test the actual zero-gating positive path and preserve coverage
of requested scopes. Then build a new candidate and test at the next necessary
Windows transition. The mismatch is established; causation of the warm hang is
not. Do not remove resource-retirement requirements or declare warm recovery.

Linux remains active with mask3, trace off and USB loader on. Windows119 is
installed;120 remains undeployed. M9 remains open.
