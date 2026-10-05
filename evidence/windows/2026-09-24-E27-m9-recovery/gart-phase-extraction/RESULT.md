# M363 - GART configuration extracted; early bind dependencies reviewed

PROVENANCE: Linux amdgpu v6.18 commit7d0a66e4bb9081d75c82ec4957c50034cb0ea449, AMD MIT; existing imported hub routines unchanged.

bc250_gmc_gart_configure_observed now contains the existing GFXHUB/MMHUB enable and fault-default sequence. The established enable wrapper calls configuration followed by its original MMHUB/GFXHUB flushes and observer callbacks. No KMD call uses the new configuration-only API. This is preparation for a staged visibility contract, not a deployed reordering or warm-start fix.

Actual-shim host comparison starts both paths from reset backend state and owns its own amdgpu_device/GARTbo. Ordinaryenable and configure+MMflush+GFXflush compare all285writes by offset/value/order. Configuration alone issues no invalidation request,ACKpoll orsemaphore access and has exactly the three expected observer boundaries. Full paths each issue two requests and four observedreads (MM semaphore+ACK; GFX dummyREQ+ACK). All checks pass. Initial test incorrectly expected five reads; corrected from actual flush source. Preserve failed and corrected logs. Existing354+35write oracle,24address exceptions,22reset/8flush cases pass;fournegativecontrols discriminate;run_gfx WDK shim compilation passes. No package/version or lab change.

Dependency review of current code (hashes attached):
- startup.c orders GART,PSP,IH,GFX. psp.c requires GartEnabled. PSP ring/commands/staging/TMR are VRAMMC, but this alone does not establish hardware independence from GFXtranslation.
- gpumem.c bc250_shim_mem_alloc binds each GTT allocation then FlushTlb invalidates both hubs. Deferring only the initial GART flush would still issue early GFXrequests.
- IH allocates its ring and writeback in GTT before GFXsetup. Its MC-space path needs maintained translation visibility; retain MMHUB invalidations. Hardware routing remains a validation requirement, not inferred solely from allocation type.
- GFXSetUp allocates writeback/rings/EOP plus SDMA buffers before stage1. The submit fence is also allocated before stages. These binds must participate in any explicitly scoped pending-GFX phase.
- Current RLC CSB allocation is VRAM. bc250_init_csb fills it and programs RLC_CSIB_ADDR/LENGTH. Stage5 resumes RLC; CP is stage6, SDMA stage7. This identifies a candidate commit point afterstage5/beforestage6, but is not hardware proof that all RLC accesses are independent of pending GFXtranslation.
- Gart.Enabled currently means hardware was touched and snapshot restoration is owed, set before initial writes. It must not be overloaded as proof that a deferred GFXflush completed.
- gpumem TlbDirty and GpuMemRelease protect retirement. An MM-only bootstrap result must never clear pendingGFX state or permit backing release under the current both-hub assumption.

Next implementation requirements: device-owned bootstrap state distinct from GART restoration ownership; only automatic fullstartup may enter it; retain all MM visibility; accumulate every early GFX invalidation; afterRLCstart perform and verify the pending GFX flush before CP,ready/publication or ordinary submission. Retain storage on unresolved pending translations during unwind. Diagnostic/full-enable behavior remains complete. Test the successful staged path plus actual consumer/publication barrier; then build a new version and perform first/warm hardware content controls. If RLCcannot safely reachstage5 with deferred GFXvisibility, this ordering proposal is rejected rather than weakening readiness.

Current installed lab remains114display-only fromM362,boot06:44:52. M9 warm/cache/DMA/alias/lifetime/performance acceptance remains open.
