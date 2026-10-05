# Primary CPU shadow: bounded design review

2026-09-25, read-only. No source edits, build or lab actions.

A CPU shadow can remove llvmpipe destination reads from primary WC/uncached memory
without violating Microsoft's prohibition on Cached primary allocations. Retain the
ordinary pipe_resource created by CreateResource, keep the KMT primary allocation
unchanged and CPU-lockable, and publish the rendered image into that allocation only
after the render fence completes. This adds a CPU copy; it does not accelerate DWM
on GPU. At1920x1200 one image is8.79MiB;60 full copies/s write about527MiB/s, plus
source reads. Measure the copy separately before claiming60Hz.

## Minimal positive implementation

1. Eligibility must describe owned storage, not just the logical runtime handle:
   newly created, nonshared, single-level/single-layer/single-sample32-bit color
   surface used as a flip source. Exclude OpenResource/imported allocations even
   though their reconstructed Resource.shared is currently false. A created primary
   flag provides the initial policy, but is not safe to re-evaluate after rotation.
2. Bc250EnsureSurface still Allocate/maps/makes-resident/Lock2 exactly as now. For
   eligible storage retain its original pipe_resource instead of replacing it with
   resource_from_handle(USER_MEMORY). Store a shadow flag and actual destination
   pitch/extent alongside allocation/gpuVa/gpuBytes/cpuMapping. presentReady means
   both sides prepared, not that llvmpipe necessarily aliases Lock2.
3. In _Present wait the existing renderFence, map the original pipe_resource for
   PIPE_MAP_READ using pipe->texture_map, validate transfer stride and source extent,
   copy width*4 bytes per row into cpuMapping with the allocation's real pitch,
   unmap, issue MemoryBarrier, then invoke PresentCb. No read-modify-write on the WC
   destination. Do not use source stride as destination pitch or copy its padding.
4. Full-frame publication first. Partial rendering modifies only the shadow; unchanged
   pixels remain with that same storage identity. Copy the entire current shadow on
   every publication, avoiding speculative dirty-rectangle semantics. InitialDataUP
   is already applied after EnsureSurface and therefore fills the retained shadow.
5. Measure render_wait_ms and shadow_copy_ms independently. More speed is plausible,
   but the additional full copy and any conversion are not free.

## Rotation is part of the patch, not optional

Current _RotateResourceIdentities (DxgiFns.cpp:337-402) rotates pipe_resource,
allocation, gpuVa, gpuBytes, cpuMapping, presentReady and repairs RTV/SRV/bound views.
It preserves hRTResource and logical primary/shared descriptors. Add shadow flag,
physical pitch and any shadow-valid/dirty state to the rotated storage bundle.
Do not derive publication behavior from resource->primary after the rotation.
The Microsoft DXGI_BASE_FUNCTIONS contract requires Y/Z/X kernel identities and
stable runtime handles; current2/3-buffer controls already exercise that rule.

Mixed shadow/direct rotations are possible if only one created resource was marked
primary. They can be correct when the entire bundle rotates, but unshadowed buffers
still render to WC and can preserve periodic stalls. For a whole-chain optimization,
owned nonshared BIND_PRESENT backbuffers also need the same shadow policy, not just
pPrimaryDesc!=NULL. This is a scope decision requiring observed creation/rotation
flags, not a reason to silently shadow imported/shared resources.

## Concrete blockers to a naive EnsureSurface-only change

- Shared primary: an independent cached shadow would hide writes from another
  process/device, and importing an existing handle could return stale content.
  Keep the current direct mapping for all shared or opened allocations. The147
  shared-nonprimary cached path remains as-is.
- _Present with hDstResource: current KMD WddmPresentBlit can write the destination
  allocation, bypassing its pipe_resource. If that destination has a shadow, the
  next CPU draw/sample/readback can use stale pixels. A source shadow can be copied
  out before this call; a shadow destination cannot simply be ignored.
  A bounded initial path must establish that eligible flip surfaces are never used
  as such destinations, or explicitly synchronize and read the completed destination
  back into the shadow. The latter reintroduces WC reads and requires a real
  completion contract; do not presume PresentCb return always means GPU completion.
  No blanket copy-back/fence protocol is justified by this review.
- _Blt is a separate UMD entry point. It writes CastPipeResource(dst), flushes with
  no fence, then returns. With shadows that updates only CPU shadow storage. If its
  result is consumed as a kernel allocation before _Present, it must be published
  after a completed fence. Identify that use and add the same publish helper there
  when required; never copy while an asynchronous pipe->blit is still executing.
- External OS/GDI writes into a nonshared primary are also outside the shadow.
  No ownership rule in this patch may assume that 'nonshared' means 'no kernel
  writer'. The supported flip path must be established by existing callback traces
  or source contract, or these writers need an explicit synchronization/import path.

## Format, pitch and lifetime

Current EnsureSurface writes LB7A pitch=width*4 and imports the same format. Its
three accepted formats map directly to32-bit byte layouts; preserve them without
channel swapping. The ordinary GDI winsys resource has aligned stride, so obtain
actual stride through transfer mapping. KMD primary pitch uses64-pixel alignment;
1920 is aligned, arbitrary widths are not. A general implementation must request
matching aligned destination pitch/size in LB7A and use it consistently; otherwise
limit the experiment to already compatible geometry. Do not regress shared/opened
LB7A consumers which currently require pitch==width*4.

One ordinary shadow replaces the existing ordinary allocation that EnsureSurface
currently frees, so extra storage is one CPU image per shadowed KMT image. Runtime
views reference the shadow resource; the normal pipe refcount owns it. cpuMapping
is separately owned by Lock2. DestroyResource must finish CPU work before unlocking
or destroying its kernel allocation; keep the original pipe resource alive through
any map/copy and repair all rotated view bindings as today. Cached shadow requires
no special PAT mapping API and changes no GPU PTE/cache type.

## Meaningful acceptance before promotion

- Actual2/3-buffer rotation test extended with distinct shadow flags, pitches and
  content; verify the entire bundle rotates and retained RTV/SRV point at the shadow.
- Source readback and published full content for at least two incremental frames;
  a changed small rectangle must preserve other pixels and survive repeated rotation.
- Shared open/red-blue test remains direct and passes unchanged; shadow destinations
  either supported with a proven synchronization path or excluded by supported scope.
- Measured render-fence + copy timings and visible animated desktop, without heavy
  observer escapes. Test successful image content as well as Present return codes.

Sources: current scratch/mesa-main-20260924/src/gallium/frontends/d3d10umd/
DxgiFns.cpp (EnsureSurface,_Present,_RotateResourceIdentities,_Blt), Resource.cpp
(CreateResource,OpenResource,DestroyResource), State.h:165; ordinary allocation in
src/gallium/winsys/sw/gdi/gdi_sw_winsys.c:gdi_sw_displaytarget_create (align_malloc).
Local Microsoft references: d3d10umddi D3D10DDIARG_CREATERESOURCE pPrimaryDesc
(nonnull flip-style primary; null copy-style), dxgiddi DXGI_DDI_BASE_FUNCTIONS
pfnRotateResourceIdentities, d3dkmddi DXGK_ALLOCATIONINFOFLAGS_WDDM2_0 Cached.
