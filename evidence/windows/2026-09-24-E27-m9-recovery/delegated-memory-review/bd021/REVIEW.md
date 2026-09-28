# BD-021 review: SNOOPED on local VRAM

2026-09-24. Read-only review; no production source or lab state changed.
Recommended backlog status: NEEDS-LAB, not FIXED or NOT-A-BUG.

## Decision

The reported source behavior exists: bc250_pte_vm_flags() propagates the caller's
snooped argument independently of SYSTEM, and bc250_pte_from_dxgk() propagates
DXGK_PTE.CacheCoherent for both leaf and directory entries. That alone does not
establish the claimed undefined/slower behavior. Do not silently clear an OS
coherency request on the strength of the report's incorrect universal Linux claim.

## Reference findings

- Linux v6.18 commit 7d0a66e4bb9081d75c82ec4957c50034cb0ea449,
  ref/linux-src/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c:1362-1384:
  amdgpu_ttm_tt_pde_flags sets SNOOPED for cached system-backed resources AND
  TTM_PL_VRAM with mem->bus.caching == ttm_cached. amdgpu_ttm_tt_pte_flags at
  1394-1407 inherits those flags. Thus 'Linux only sets it for system pages' is false.
- amdgpu_vram_mgr.c:607-610 chooses cached VRAM when xgmi.connected_to_cpu,
  otherwise write-combined. The exception is not proof of coherent VRAM on BC-250.
  Ordinary non-XGMI placement follows the WC/non-snooped policy in this source.
- gmc_v10_0.c:492-536 does not generally clear SNOOPED on local entries. It does
  select memory type from VM flags and BO flags; these are distinct from SNOOPED.
- Microsoft local DDI snapshot ref/windows-driver-docs-ddi commit
  7515063cea4c9e98db6a92986c5b4ddb0463fd16; consolidated ref/ddi-display/d3dukmdt.md:4046:
  CacheCoherent indicates a CPU/GPU coherent page. It does not specify that a local
  PTE must ignore the bit. WDK/SDK 10.0.26100.0 d3dukmdt.h:323 supplies its layout.
- ref/ddi-display/d3dkmddi.md:22240: CacheCoherentMemorySupported means the driver
  supports these PTE bits and coherent I/O to system memory.
- Same file:27933: segment CacheCoherent has meaning ONLY for aperture segments.
  A local segment leaving this flag zero is not a separate prohibition on the PTE bit.
- Same file:18223: Cached controls the allocation's backing-store cache mode; VidMm
  flushes CPU caches as needed for use in a non-coherent segment. Allocation Cached
  is not a promise of cached local VRAM mapping.

PROVENANCE: Linux amdgpu reference files above carry MIT notices in the GPL-2.0
kernel repository; no upstream code was imported by this review. Microsoft local
DDI text and WDK declarations used only for contract checking.

## Current KMD advertisement and reachability

- wddm.c WddmQuerySegment4:1750-1784 publishes application local and table local
  segments with CpuVisible and LocalBudgetGroup, no CacheCoherent or cached CPU
  host aperture. Aperture segment 2 alone sets CacheCoherent (line 1770).
- WddmGpuMmuCaps at 1923-1925 advertises CacheCoherentMemorySupported=1 for system
  pages and cached GTT backing. This is a global capability, not a local-memory cap.
- WddmCpuVisibleAllocationFlags at 2338 sets Cached=0. The UMD allocation branch
  changes it using UmdBlobAllocCpuCached at 2428. umd_blob.c:120-124 returns true
  only for GTT, only with a cache policy and without NO_CPU_ACCESS/GTT_USWC.
  Current application local VRAM therefore does not request cached backing.
- This makes local coherent PTEs unproven on the current advertised positive path.
  The local public documentation does not guarantee the OS will never supply them,
  especially for OS-owned page tables. No actual local CacheCoherent=1 callback was
  captured by this review. Existing vidmm.c:425-435 diagnostics count system PTEs
  only, so they cannot establish local absence or prevalence.

## Validation

Command (TEMP and TMP set to P:/bc-250/scratch/tmp):

    pwsh -NoProfile -File bc250-win/driver/shim/test/run_pte.ps1 -Out P:/bc-250/scratch/build/bd021 -Verbose250

Log: scratch/m9/bd021/pte-control.log. Exit 0, PASS (0 failures), clean /W4 /WX
user-mode build and WDK kernel-flag compile. Existing positive controls exercise
real SDK DXGK_PTE layout, coherent system/local leaves and directories,
non-coherent local entries, GART flags, and 2048 table-segment address/unit/kind
combinations. No test was added; no hardware coherence or performance claim.

## Narrow next measurement

During an already planned normal local/GTT allocation and paging control, capture
or count incoming valid PTEs by (segment, page-table level, CacheCoherent), retaining
the requested and encoded flags. This needs no forced coherent local mapping.
If local coherent requests occur, correlate with actual CPU mapping attributes
before selecting enforcement or refusal; system-memory coherence must remain intact.
Do not add a standalone OS restart or Linux trip just for this source review.

## Proposed append-only DEFECTS comment

- 2026-09-24 review: Source review confirms propagation but not the reported failure.
  Linux v6.18 amdgpu_ttm.c:1379-1381 also sets SNOOPED for cached VRAM;
  amdgpu_vram_mgr.c:607-610 limits that cache policy to CPU-connected XGMI.
  Current KMD requests Cached only for GTT and advertises segment CacheCoherent
  only on the aperture; local CacheCoherent=1 callbacks have not been observed.
  MS DXGK_PTE does not authorize silently discarding a coherency request. Existing
  PTE host controls and kernel-flag compile pass with 0 failures. Leave NEEDS-LAB
  for bounded local/system PTE flag observation; no runtime patch or HW conclusion.
  Review/log: scratch/m9/bd021/REVIEW.md and pte-control.log.
