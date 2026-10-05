# M392 - Stage5 combines RLC resume with pending GFX visibility

PROVENANCE: Linux amdgpu v6.18.52, AMD MIT. Source review only; no lab action.

## PSP lifetime comparison

Windows Bc250StopDevice performs GfxPrepareStop, PspStop, GartPrepareStop,
GfxStop and GartStop in that order. PspStop invokes Unload only after GfxStopQuiet.
Unload sends DESTROY_TMR before ring stop, then unmaps the CPU PSP pages only
when both are down. The TMR and PSP pages occupy reserved VRAM ranges; removing
the CPU mapping is not itself freeing arbitrary Windows system-page backing.
The TMR destroy/ring stop command order agrees with psp_hw_fini's
psp_tmr_terminate followed by psp_ring_destroy. Linux suspend also terminates
TMR and stops the ring. This review does not establish an early-free defect.
M391 separately measured both command returns0; hardware firmware lifetime
correctness is not proved by those returns.

## Unresolved boundary in the current trace

RunEngineStage calls g_Stages[Stage].Run first. For Stage==CP-1 (RLC stage5),
if GfxTlbBootstrap is pending and the stage/backend succeeded, it next calls
GpuMemCompleteGfxBootstrap before returning. GfxExecute logs the completed
stage only after RunEngineStage returns. Therefore a persisted stage5 entry
with no completed-stage log cannot distinguish:

1. A failure inside bc250_gfx_rlc_resume (RLC stop, CG/PG disable, CSB, SPM,
   RLC start).
2. A failure in GpuMemCompleteGfxBootstrap after RLC resume returned.

M391's all11PSP success and SDMA0-associated RLC_BUSY remain measured. They do
not identify the later hanging instruction. The previous broad label "stage5"
was accurate; equating it with failure inside RLC resume would be too strong.

## Next diagnostic implementation

Add persisted checkpoints at RLC resume entry/return and immediately before/
after deferred GFX bootstrap completion, with substep labels only as needed.
Reuse the CP1 checkpoint locking arrangement: PASSIVE_LEVEL and special APCs
enabled, same GfxPagingLock -> GartLock ownership, paired unsafe-fast-mutex
release. Current ordinary GfxExecute takes a normal fast mutex and therefore
cannot simply insert Zw file writes at that point. Keep diagnostic PLAN and
unrelated calls unchanged. Preserve identical MMIO ordering in a host control
and verify the source-derived stage/commit contract before hardware use.

No change to candidate121 and no lab test in M392. Unit A remains Windows121
from M391 recovery, last observed display-only and SSH healthy. Full goal open.
