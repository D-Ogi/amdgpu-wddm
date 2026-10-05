# llvmpipe shared-surface CPU cache review

2026-09-25. Source/contract review; no lab actions. Parent observations (slow
post-S4 DWM CPU rendering/flush, normal general CPU and system-memory controls)
are inputs to a hypothesis, not proof of the cause. This review led to the
separately authorized KMD source change in scratch/m9/shared-cpu-cache.

## Finding

Cached=1 is the documented policy for a CPU-read, non-primary, system-aperture
surface. Current E26R v1 shared surfaces are allocated with Cached=0, and Mesa
imports the Lock2 pointer directly into llvmpipe. This is a concrete reason to
correct allocation intent and measure its effect. It does not establish that
the observed pointer is WC or that S4 changed its attributes.

The static v1 policy existed before S4 too. To attribute the regression to it,
compare the same surfaces/workload cold and resumed. If both map identically,
look for changed composition/read intensity, imported resources or another
post-resume effect. A faster heap-memory control does not test the locked
surface used by llvmpipe.

## Local Microsoft contract

DDI snapshot 7515063cea4c9e98db6a92986c5b4ddb0463fd16, declarations from WDK
10.0.26100. Conceptual snapshot staging 110f60eaf2ac5836e644d320c1e92c1011f2af5e.

- ref/windows-driver-docs-ddi/wdk-ddi-src/content/d3dkmddi/
  ns-d3dkmddi-_dxgk_allocationinfoflags_wddm2_0.md:69-75:
  Cached requests cached backing store; the default is write-combined backing.
  It is recommended for allocations read by the application/UMD and not for
  write-only allocations. CpuVisible must also be set. Primaries MUST NOT use
  Cached. VidMm flushes CPU caches as appropriate for noncoherent segments.
- ns-d3dkmddi-_dxgk_segmentflags.md:84-86: CacheCoherent describes an aperture's
  ability to maintain coherence with cacheable pages. It does not mean every
  CPU mapping is automatically cached and has no meaning on a memory segment.
- ns-d3dkmddi-_dxgk_gpummucaps.md:76-78: CacheCoherentMemorySupported means PTE
  CacheCoherent bits and I/O-coherent transfers to system memory are supported.
- ../d3dumddi/nc-d3dumddi-pfnd3dddi_lock2cb.md: WDDM2 puts CPU/GPU lock
  synchronization responsibility on the UMD. Changing Cached is not a substitute
  for fences, producer/consumer ordering or shared-resource synchronization.

Public corresponding source:
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_allocationinfoflags_wddm2_0
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_segmentflags
All were consulted locally; no online search was required.

## Actual path

- wddm.c:WddmCpuVisibleAllocationFlags requests CpuVisible=1/Cached=0 while
  preserving reserved input bits. E26R v1 sharedCpu selects only aperture;
  standard/local LB7A remains physical VRAM. BC2A already has a separate v2
  cached-GTT intent path and must remain unchanged.
- scratch/mesa-main-20260924/src/gallium/frontends/d3d10umd/DxgiFns.cpp:
  Bc250EnsureSurface:59 allocates using E26R and the unchanged 32-byte LB7A,
  waits residency, obtains pfnLock2Cb.pData:126 and imports that exact address as
  WINSYS_HANDLE_TYPE_USER_MEMORY. The mapping remains until destruction.
- Resource.cpp:256-257 sets primary from pPrimaryDesc and shared independently
  from MiscFlags. OpenResource:426-468 uses an existing allocation, then imports
  it through EnsureSurface. It cannot retroactively change allocation policy.
- src/gallium/winsys/sw/gdi/gdi_sw_winsys.c:220-241 assigns external user memory
  directly to gdt->data; displaytarget_map:115 returns it. lp_texture.c's display
  target map path therefore has no automatic conversion to a separate WB buffer.
  Software texture reads/blending can read this mapping. A llvmpipe flush may
  wait for CPU worker completion; its wall time does not identify a GPU wait.

## Exact non-primary discriminator / ABI

Do not use shared as a proxy for non-primary. Public WDDM2 allocation flags
contain no Primary input member; the remaining OS bits are reserved and must
not be interpreted or overwritten. D3DDDI_ALLOCATIONINFO2.Flags.Primary exists
on the UMD side, where the actual resource role is available.

Agreed E26R v2 is exactly four ULONGs (16 bytes):

    { magic=0x52363245, version=2, shared, cpuAccessFlags }
    PRIMARY=1; CPU_READ=2; shared is exactly 0 or 1.

UMD must derive PRIMARY from the same resource->primary that supplies the
runtime allocation Primary bit. CPU_READ declares the intended shared software
reader path, not an inferred physical mapping. Parent owns that producer change
and its minimum-KMD147 requirement. The parent also owns UMD primary/shared
negative controls; no UMD file was edited by this agent.

KMD caches only valid v2 + shared==1 + CPU_READ + !PRIMARY + aperture placement.
V1/standard LB7A remain Cached0. V2 shared primary (flags3) remains uncached.
Malformed recognized E26R is rejected before allocation side effects. The
allocation-private LB7A remains32bytes. Unrelated resource ABIs and BC2A retain
existing behavior. Opening an already-existing v1 allocation retains its v1
policy: restart/recreate the actual shared-resource producers for an A/B test.

The cached class must remain offscreen CPU-sharing. Do not later promote it to
primary/direct scanout without an explicit role/allocation transition. Existing
caps advertise DirectFlip, so a future promotion path must honor this distinction;
the current cache change is not permission for cached scanout.

## Coherency and M447 limits

The existing aperture is declared coherent. bc250_pte.c:bc250_pte_from_dxgk maps
requested CacheCoherent to AMDGPU_PTE_SNOOPED; VM leaf memory type is MTYPE_NC.
bc250_gart.c explains why the GPU's cache type and CPU cache snooping are
separate properties. GPU NC/UC does not mean CPU NC/WC. No new PTE type or global
local-memory/POST mapping change is justified by this CPU allocation request.

M447 evidence/windows/2026-09-24-E27-m9-recovery/delegated-memory-review:
- bd021:71 focused checks validate attempted PTE encoding/counters, not live
  backing cache attributes, actual committed leaf values or data visibility.
- bd022:2072 checks validate POST mapping selector behavior. Actual independent
  OS/VidMm versus driver mapping attributes remain unknown; old physical cache
  queries returned an error and cannot be cited as a cache observation.
Thus M447 supports retaining the existing mapping/coherency architecture while
measuring explicit shared-system allocation policy, not claiming its acceptance.

## Decisive controls

1. Identify the exact Lock2 surface: ABI/role, allocation handle, size/stride,
   CPU mapping, process, and primary/shared status. Record VirtualQuery's State,
   Type, Protect and AllocationProtect over the entire mapped extent. Explicit
   PAGE_WRITECOMBINE/PAGE_NOCACHE bits are useful OS mapping witnesses; absent
   bits alone are not a PAT/MTRR or same-physical-page alias proof. Do not create
   a second conflicting-cache physical alias to measure the first.
2. Standalone matched v1/v2 non-primary shared allocations, same size/data and
   callback/residency route. Time checksum-bearing sequential reads, dependent
   sparse reads and writes separately; repeat warm reads. Compare with a normal
   WB heap control. Preserve native return values and exact content. Allocation
   Cached intent, GPU SNOOPED and measured throughput are three separate results.
3. Where GPU accesses that allocation, run CPU-write -> actual GPU-copy/read
   -> GPU-write -> CPU-read controls with real fences and changing patterns,
   including stale-data negatives. No special CPU flush added just to hide an
   incorrectly coherent mapping. Re-run after S4 on the same retained allocation
   if it remains valid, then on a newly created allocation.
4. DWM A/B must recreate producing shared resources as well as DWM imports;
   compare equivalent composition/shader counts and input, same worker count,
   primary output, clocks and telemetry. A cached shared-source improvement does
   not remove CPU reads of an uncached primary render target; measure which role
   consumes worker time. Do not set Cached on primary to improve that number.
5. Only infer WC as the cause when the actual mapping observation and matched
   surface-read controls agree with the changed render cost. Policy-only speedup
   establishes that the change helped, not a raw hardware cache-type diagnosis.

No live cache/performance result is claimed here. The root is collecting those
controls independently. General borrowed-table/local-surface alias questions
remain separate from this bounded system-aperture shared-resource policy.
