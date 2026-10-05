# Retained GFX/SDMA integration - source result

2026-09-25. Source only; no lab actions, deployment, version change or power-cycle claim.

## Changes

Production: driver/kmd/gfx.c only. Added GfxSetPowerRetained(Device, Resume),
GfxPowerIsSuspended(Device), private PowerSuspended state, retained ring transport
reset and refusal of active execution/diagnostic RUN while suspended.

Suspend holds GfxPagingLock then GartLock, closes bounded CPU readers, requires
joined IH and no outstanding OS or diagnostic fence, and runs the existing SDMA
halt, GFX queue unmap/halt, SDMA reload reset and RLC stop. It does not call
HaltForMappingRetirement, GpuMemRetireGttMappings, TearDown, or free owners.
Only a successful halt/reset establishes PowerSuspended. Failed transitions keep
backing and close execution; they do not fabricate completion or readiness.

Resume requires retained setup, enabled GART, PSP loaded, IH active and the GFX
translation-bootstrap flag. Existing GTT allocations, OS handles and fence pages
remain. Transport writeback and ring positions are reset. FenceSeq, SubmitSeq,
PagingSubmitSeq and completed fence data stay intact. The VmidRoot cache is cleared;
normal nonzero-VMID submission already programs and flushes its root every job.

RunEngineStage runs stages 1-8 using existing imported sequences. RLC reconstructs
its VRAM clear-state buffer; the existing bootstrap barrier commits GFX translation
before CP. CP recreates VRAM MQDs from retained descriptors and maps/tests queues.
SDMA reuses its retained fence page and performs both ring tests. Admission opens
only after all stages and the bootstrap flag have completed successfully.

Core GFX ring/EOP/fence/writeback and optional diagnostic shader/destination buffers
are retained GTT. PagingCopyStaging is private VRAM scratch filled before each use.
The optional startup-only SdmaVaTables diagnostic reconstructs its tables when run;
it is not an OS page-table preservation mechanism.

## API and coordinator review

    NTSTATUS GfxSetPowerRetained(BC250_DEVICE* Device, BOOLEAN Resume);
    BOOLEAN GfxPowerIsSuspended(const BC250_DEVICE* Device);

Root owns declarations. GfxSetPowerRetained requires PASSIVE_LEVEL and external
adapter lifecycle exclusion. GfxPowerIsSuspended requires GartLock; it is a cached
successful suspend verdict. Consume it before PSP firmware resume. PSP may release
SDMA, so the cached flag is not a fresh hardware observation after firmware reload.

Read-only review of current power.c matches these dependencies:

- Down: WDDM drain -> IH join -> GFX halt -> PSP suspend -> GART suspend.
- Up: GART/private mappings/bootstrap -> PSP -> IH -> GFX/RLC barrier -> WDDM.

No source-order blocker found for that bounded sequence. Native SMU and display
preparation remain coordinator-owned. OS CPU_VIRTUAL page-table reconstruction is
separate; no stale OS PTE replay is introduced. A completed host sequence does not
prove real firmware reload or S4 resume succeeds on this hardware.

## Validation

- New actual-source extractor, fixture and runner:
  driver/kmd/test/generate_gfx_retained_test.py
  driver/kmd/test/gfx_retained_test.c
  driver/kmd/test/run_gfx_retained.ps1
- 600 checks, 0 failures. Extracts actual RunEngineStage and all added transition
  functions; mocks hardware stage effects. Checks order/lock ownership, retained
  identities/fences, bootstrap before CP, VMID invalidation, prerequisites,
  outstanding work refusal, failed halt/reset/stage and closed admission.
- Negative control omitting VmidRoot invalidation: 320 failing checks.
- Negative control resetting SubmitSeq: 1 failing check.
- Actual gfx.c compiles with WDK 10.0.26100, /kernel /W4 /WX.
- Test log test.log; compile.log; mutation logs mutant-vmid.log and mutant-fence.log.
  Binaries/generated fixtures: scratch/build/gfx-retained-integration.
- No assertion here tests actual MMIO execution, firmware reload or GPU content.
  Required hardware acceptance remains real retained suspend/resume followed by
  both engine content tests, models, OS fence progression and visible desktop.

## Local source references

- Microsoft local DDI: ref/windows-driver-docs-ddi/wdk-ddi-src/content/dispmprt/
  nc-dispmprt-dxgkddi_set_power_state.md: PASSIVE_LEVEL, context restore, D0 ActionType.
- Microsoft conceptual snapshot 110f60ea: ref/windows-driver-docs/windows-driver-docs-pr/
  display/threading-and-synchronization-third-level.md: idle/eviction, exception for
  concurrent QueryAdapterInfo. Private owners require their own joins.
- Linux reference 7d0a66e4bb9081d75c82ec4957c50034cb0ea449, read-only:
  gfx_v10_0.c:7130-7215 (retained queue reconstruction), 7529-7578 (hardware-only
  suspend/resume). No GPL code copied. Existing permissive AMD shim APIs reused.
- driver/shim/bc250_gfx.c:1205,1235,1316: rebuild MQDs and reset queue positions;
  :530 reconstructs CSB before programming RLC; :2256 establishes backing types.
- driver/shim/bc250_sdma.c:1201: retained fence-page allocation is idempotent.
- driver/shim/bc250_dispatch.c:131: diagnostic shader/data use GTT.
- driver/kmd/gfx.c:392: RLC/GFX-bootstrap/CP ordering; :1030 nonzero VMID flush;
  :3217 retained API; :3312 serialized entry point.
