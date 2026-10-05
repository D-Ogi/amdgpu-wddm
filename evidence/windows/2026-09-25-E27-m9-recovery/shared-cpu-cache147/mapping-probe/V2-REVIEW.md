# E26R v2 independent source review

2026-09-25. Read-only review, no lab operation or production edit.

Conclusion: no new source blocker found for the paired KMD147 + v2 Mesa deployment.
This conclusion covers the current CPU-rendered shared-surface path; it is not a
claim that GPU/CPU concurrent access or every resource lifetime is validated.

## Primary exclusion and compatibility

`DxgiFns.cpp:89,97-101` uses the same immutable `resource->primary` for
ALLOCATIONINFO2.Primary and E26R PRIMARY. `Resource.cpp:256` derives that property
from pPrimaryDesc. Shared resources carry CPU_READ; a primary/shared resource
therefore carries both bits and is excluded by KMD, not accidentally cached.
`wddm.c:2536-2555` accepts exact v1/v2 lengths, shared0/1 and only the two defined
v2 bits. Its cached predicate is shared && CPU_READ && !PRIMARY. The allocation
path additionally requires the aperture segment (`wddm.c:2677`).
Standard LB7A and v1 retain Cached0; the separate BC2A path keeps its own policy.
CpuVisible remains set, as required by the MS Cached contract. No physical VRAM
mapping attributes or primary scanout layout are changed.

Deployment ordering is material: KMD146 recognizes only a 12-byte v1 descriptor.
The v2 UMD must not be activated against it: the old code would fail to recognize
the 16-byte descriptor as shared and choose its normal local VRAM allocation path.
New KMD + old UMD is compatible but remains WC. Existing allocations keep the
policy with which they were created; replacing DLL bytes alone cannot retrofit them.

## Sharing, alias and lifetime

`Resource.cpp:426-468` opens the existing allocation handle and then calls
Bc250EnsureSurface. Although its reconstructed Resource.shared is false, allocation
is already nonzero, so `DxgiFns.cpp:86-109` skips Allocate entirely. It does not create
a second allocation with a conflicting cache policy. Lock2 maps the same OS-owned
allocation. The unchanged 32-byte LB7A blob remains the per-allocation open contract;
E26R is resource-private creation policy, not a new surface layout.

KMD OpenAllocation retains its existing device-specific descriptor path and does not
change backing or cache attributes. The local DXGKARG_OPENALLOCATION contract also
says its resource-private data is the same immutable data supplied at creation.

The imported winsys USER_MEMORY handle references Lock2's pointer directly. GDI
winsys marks external_memory=true and does not free that pointer when destroying
the display target (`gdi_sw_winsys.c:134,225-240`). DestroyResource's existing
Unlock2/Deallocate2 lifetime is unchanged. Thus v2 introduces no new allocation,
alias or ownership transition. The existing assumption that pending llvmpipe work
has stopped using a resource before final runtime destruction is not strengthened
or proved by the cache change; no new lifetime redesign is needed for this patch.

## Coherence contract

Local MS `DXGK_ALLOCATIONINFOFLAGS_WDDM2_0::Cached` explicitly describes WC as the
default backing and recommends Cached for allocations read by the application/UMD,
requiring CpuVisible and excluding primary/write-only allocations. VidMm handles
CPU-cache flushing for noncoherent segments at the appropriate transitions.
`DXGK_SEGMENTFLAGS::CacheCoherent` describes an aperture that maintains coherence
with mapped cached pages. The current KMD already declares such an aperture
(`wddm.c:1940`) and consumes VidMm CacheCoherent requests in the VM PTE encoder.
`driver/shim/bc250_pte.c` maps coherence to AMDGPU_PTE_SNOOPED with SYSTEM and the
existing VM memory type; this patch does not add a different PTE encoding.

For the tested desktop path llvmpipe reads/writes these pages on CPU. Present waits
the llvmpipe render fence, then executes MemoryBarrier before the runtime callback
(`DxgiFns.cpp:180-198`). Cached ordinary CPU aliases can use normal CPU coherence;
this is not a promise that a barrier alone would synchronize a future GPU renderer.
No new explicit cache flush should be guessed into this patch. GPU access to these
surfaces would need its own fence/coherence acceptance evidence.

## Acceptance still needed

1. On KMD147 run the same v1 then v2 probe: v1 remains WC, v2 requests cached;
   all full-content oracles and normal cleanup must pass. VirtualQuery metadata
   and throughput characterize this owned allocation only, not effective PAT/MTRR.
2. Activate paired v2 UMD and recreate its consumers. A successful DWM/app sharing
   path and known-color content check exercise OpenResource more meaningfully than
   the single-process probe. Confirm visible primary and no mode/flip regression.
3. Repeat the bounded motion/ETW control to establish whether the former llvmpipe
   stall improves. The 146 4 MiB WC read ~156 ms versus cached control ~0.28 ms is
   strong motivation, not by itself proof of DWM's exact resource or S4 causality.

Sources reviewed: current `driver/kmd/wddm.c`, `driver/shim/bc250_pte.c`;
`scratch/mesa-main-20260924/src/gallium/frontends/d3d10umd/{DxgiFns,Resource}.cpp`;
`src/gallium/winsys/sw/gdi/gdi_sw_winsys.c`; patch
`experiments/E26-wddm-desktop/mesa-shared-cpu-cache-v2.patch`.
Local contracts under `ref/windows-driver-docs-ddi/wdk-ddi-src/content/d3dkmddi/`:
`ns-d3dkmddi-_dxgk_allocationinfoflags_wddm2_0.md`,
`ns-d3dkmddi-_dxgk_segmentflags.md`, `ns-d3dkmddi-_dxgkarg_openallocation.md`.
