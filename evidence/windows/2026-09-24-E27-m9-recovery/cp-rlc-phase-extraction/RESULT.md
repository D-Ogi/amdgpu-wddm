# M357 - CP retirement separated from RLC stop, KMD not integrated

Local source/model change only. Installed112 and M356 hardware state unchanged.
A shared implementation now parameterizes the final RLCstop. Existing
bc250_gfx_hw_fini calls it with stop enabled, preserving its contract;
bc250_gfx_hw_fini_keep_rlc omits only that final operation. Queue unmap,
KIQdequeue, first-error propagation and CP halt are shared. The new helper
is not called by KMD and is not a sufficient GPUquiet predicate.

The existing successful teardown/reallocation/restart replay now exercises
keep-RLC, verifies CP/MEC/SDMA halted, RLCenable unchanged and CSB pointer/MC
ownership retained, then explicitly stops RLC before the old storage teardown.
That phase check has0failures. Other existing fini callers still exercise the
original wrapper. Ordinary354+35write replay remains exact; four negative
controls rejected;22reset/8TLBobserver scenarios pass. Same shim compiles with
WDKflags in run_gfx. No new full KMD candidate/package or hardware acceptance.

Ownership finding: the current shim explicitly allocates clear_state_mem in
BC250_MEM_VRAM and publishes it via RLC_CSIB_ADDR/length. The upstream
amdgpu_gfx_rlc_init_csb permits VRAM|GTT placement, so VRAM-only is a Windows
choice, not a universal upstream invariant. The GFX10rlc_init path initializes
CSB; do not generalize to every ASIC's RLCsave/restore buffers. Firmware/TMR
ownership stays with PSP and must survive until RLC is explicitly stopped.

Proposed integration, still absent: after admitted work drains and CP/SDMA
halts succeed, invalidate only this GFXowner's GTT PTEs while retaining every
physical backing allocation and the VRAM CSB. Flush both hubs, then stop RLC,
then existing PSP/GART retirement, then storage destruction. Preserve unbind
and flush errors; no physical free or owner detach in the new pre-retirement
operation. Later teardown must distinguish successfully unbound entries from
still-bound entries to avoid reissuing the problematic post-RLCstop flush.
Partial-start and diagnosticFini behavior need separate preservation tests.
Do not apply this to arbitrary WDDM allocations or another owner's pages.

This is a hypothesis-driven sequencing change. CSB placement and halted CPs do
not prove all undocumented RLC access behavior. Before hardware use, verify
owner selection, original PTE ranges, all mappings removed, successful flush,
retained storage until the final phase and failure retention. The next hardware
trial must compare control/workload/retirement/warm reentry, not only a busy bit.
No TLB invalidation is waived; general DMA/cache/alias acceptance stays open.
