# Design note: an engine present for the Vulkan WSI (ADR 0018)

Date: 2026-09-27 (second revision, replaces the D3D11 composition draft of the same day). Status: design,
nothing measured beyond the `facts.md` rows cited. Owner of the WSI side: the Vulkan/ICD work; owner of the
KMD side: the kernel driver work (ADR 0018 consequences).

## What the port does today

`radv_wddm2_wsi_init` (fork `amdgpu-wddm/radv-wddm2-kmt-enum` c34ab7cd) sets `wsi->sw = true` on the BC250, so
`wsi_common_win32.cpp` takes its CPU-image path: the swapchain image lives in host-visible memory, every
`vkQueuePresentKHR` copies it on the CPU into a DIB section (with an R/B swizzle for R8G8B8A8 chains), `BitBlt`s
into the window DC, `GdiFlush`, `DwmFlush`. Measured on the Witcher 3 DX12 scene: 15.8 ms of a 33.4 ms frame on
the presenting thread (facts M580). The same file has a second path, taken when the winsys provides
`win32.get_d3d12_command_queue`: a DXGI flip-model swapchain created for composition, the Vulkan image copied
into the DXGI back buffer by a D3D12 command list, ordered by shared D3D12 fences exported from Vulkan timeline
semaphores, then `IDXGISwapChain3::Present1`. That path needs a D3D12 device on the same adapter (ADR 0017, M15).

## Two constraints that decide the design

- **A composition swapchain needs a D3D device of the adapter, and the adapter has none on the GPU.** Mesa's
  DXGI path (and a D3D11 variant of it) runs the per-frame copy in the adapter's D3D user-mode driver. Today
  that driver is the llvmpipe build of `d3d10umd` (UMD 8279AC7F, a CPU copy) without D3D11.3 fences, and the
  hosted RADV D3D UMD is not the system UMD (M13, the KMD/DWM work). Loading the system DXGI inside DXVK and
  vkd3d-proton processes is not the obstacle: with the full System32 path it loads and works next to the
  application's own `dxgi.dll` (facts M496; E37 run 006 confirms it for vkd3d-proton). The earlier claim that
  the loader returns the application's module for a full-path load (agent note of 2026-09-27) was wrong and is
  withdrawn here. The path therefore waits for a GPU D3D UMD and for fences in it; it is the wrong first step
  for ADR 0018 item 2, whose target is the DXVK and vkd3d-proton titles of M12 on the current stack.
- **A window present without a D3D device is documented.** The OpenGL ICD contract
  (`ref\windows-driver-docs\...\display\providing-kernel-mode-support-to-the-opengl-installable-client-driver.md`,
  `ref\ddi-display\d3dkmthk.md`, WDK 10.0.26100 `D3DKMT_PRESENT`) has the ICD call `D3DKMTPresent` itself with
  `hWindow`, `hSource` (the allocation being presented), `Flags.Blt`, `SrcRect`/`DstRect`, `SubRectCnt` and
  `pSrcSubRects`, on its own `hContext`. dxgkrnl decides the route: with DWM composing it is the GDI redirection
  surface (PresentMon reports such presents as "Composed: Copy with GPU GDI"), full-screen it is the primary,
  and in the hybrid case the cross-adapter allocation. In every route the copy itself arrives at the KMD as
  `DxgkDdiPresent(Flags.Blt)` with dxgkrnl's destination allocation. This is exactly ADR 0018's KMD item, and
  it is the route the rest of this note is built on.

## Options considered

| option | what | copies in steady state | verdict |
| --- | --- | --- | --- |
| A | Keep `wsi->sw`, replace the CPU copy by an SDMA copy into the DIB memory | one engine copy into host memory, then `BitBlt` (a CPU copy by GDI) | Rejected: the CPU copy moves into GDI and stays |
| B | WSI calls `D3DKMTPresent(hWindow, Blt)` on the swapchain allocation; the KMD performs the copy on an engine into the destination dxgkrnl names | one engine copy (KMD) | **The path.** Documented (OpenGL ICD contract), no D3D device, works inside DXVK and vkd3d-proton processes |
| C | Mesa's DXGI composition path with a D3D11 device on the adapter (shared texture, `CopyResource`, `Present1`) | one copy by the D3D UMD (CPU on the current UMD) | Deferred: needs a GPU D3D11 UMD with fences (M13) and a linear-pitch match between the D3D UMD's allocation and RADV's GFX10 layout. Worth revisiting after M13 because its end state (the Vulkan image bound to the DXGI back buffer through a private UMD query) removes the last copy |
| D | KMD engine Blt honouring dxgkrnl's destination | one engine copy | Required by B; it is ADR 0018's KMD item |

## Path B in detail

### Swapchain images

- Created by the common WSI code as ordinary device-local RADV images with `VK_IMAGE_TILING_LINEAR`, one
  dedicated `VkDeviceMemory` each (the model is `wsi_configure_cpu_image` / `wsi_create_cpu_linear_image_mem`,
  minus the `MapMemory`). RADV's linear layout gives a pitch that is a multiple of 256 bytes
  (`surf_pitch * bpe`), which the KMD contract below requires; no pitch override is attempted (GFX10 refuses
  one, `ac_surface_override_offset_stride`).
- The allocation carries the geometry dxgkrnl and the KMD need. The winsys writes the existing 32-byte
  `LB7A` surface description (`{Magic, Version 1, Width, Height, Pitch, Format, Size}`, the block the CPU UMD
  and the KMD's `WddmPresentBlit` already share) as the allocation's private driver data instead of the
  `BC2A` buffer blob, when the memory is allocated for a WSI presentable image. The trigger is a new field in
  `wsi_memory_allocate_info` (`present_surface`) that `radv_alloc_memory` forwards to `radv_bo_create_for_image`
  as a new `RADEON_FLAG`, and the winsys fills the block from `image->planes[0].surface` and the image format
  (B8G8R8A8 -> `D3DDDIFMT_A8R8G8B8`, R8G8B8A8 -> `D3DDDIFMT_A8B8G8R8`, sRGB variants the same).
- Memory placement stays VRAM (`AccessedPhysically`, the KMD's LB7A default); the E26R blob is not sent, so the
  allocation is not a primary and not CPU-visible. Nothing in the WSI maps it.

### Presenting

- The winsys owns one extra `D3DKMT_CREATECONTEXTVIRTUAL` per device for presents: node 0, no private driver
  data (the KMD treats an empty blob as a non-UMD context, `driver/kmd/wddm.c` `Bc250WddmCreateContext`), so it
  meets the current `Bc250WddmPresent` gate `!context->UmdContext` and later the engine Blt on the same terms.
  It is created lazily on the first present and destroyed with the winsys.
- The common blit flow of `wsi_common.c` is reused unchanged: the first submit signals the per-image blit
  timeline value N; `wsi->blit(chain, i)` runs the present; the second submit waits for N+1 and signals the
  present fences/semaphores. The Win32 layer's `blit` for this path (`wsi_kmt_present`, hook
  `win32.kmt_present` implemented by the winsys) does:
  1. `D3DKMTWaitForSynchronizationObjectFromGpu(present context, blit semaphore handle, N)` so the copy is
     ordered after rendering by the scheduler, not by a CPU wait;
  2. `D3DKMTPresent{hContext = present context, hWindow, hSource = image bo handle, Flags.Blt = 1,
     Flags.SrcRectValid = 1, Flags.DstRectValid = 1, SrcRect = DstRect = (0,0,w,h) of the image, SubRectCnt =
     damage rectangles or 1, pSrcSubRects}`; on the GDI-redirected route dxgkrnl takes the client rectangle
     from `hWindow`;
  3. `D3DKMTSignalSynchronizationObjectFromGpu(present context, blit semaphore handle, N+1)` queued behind the
     present on the same context.
  The blit semaphore is the monitored fence the common code already exports (`vk_semaphore_get_active_sync`
  gives the `vk_wddm2_monitored_fence` handle); no CPU wait in the steady state.
- Present modes: FIFO only (the route has no queueing beyond dxgkrnl's); `vkAcquireNextImageKHR` waits for the
  per-image present fence as the GDI path does. `VK_PRESENT_MODE_IMMEDIATE_KHR` is not advertised until a
  measurement shows what `D3DKMTPresent` does without `DwmFlush`.
- The E43 present log gains the path name `kmt`: `copy_us` is 0 by construction, `blit_us` covers the three KMT
  calls, `dwmflush_us` stays for the optional `DwmFlush` (off by default; a switch for measurement).
- Fallback: `BC250_WSI_CPU_PRESENT=1` selects the old `wsi->sw` path (ADR 0018 point 2, diagnostic use).

### Dispatch and hosting

- `D3DKMTPresent` is added to `vk_wddm2_dispatch_table_gen.py` (`gdi32` export) and to the hosted call table of
  `bc250_host_bootstrap.h`, so the hosted ICD (`BC250_WDDM_CALL`) can present through the host process on the
  same terms.

## Contract between the WSI and the KMD Blt

- Source: the swapchain allocation, linear, 4 bytes per pixel, `D3DDDIFMT_A8R8G8B8`/`X8R8G8B8` or
  `A8B8G8R8`/`X8B8G8R8`, pitch a multiple of 256 bytes, page-aligned in VRAM, described by the `LB7A` private
  data block (format, width, height, pitch, size). Tiled sources are rejected at `DxgkDdiPresent` until a
  tiled-aware copy is measured.
- Destination: the allocation dxgkrnl names in `DxgkDdiPresent` (entry 2 of the allocation list: the
  redirection surface, the primary or the cross-adapter surface). The software packet already copies into it
  when that entry names an opened LB7A object (`WddmPresentBlit`, geometry, translation and overlap checks
  before the copy) and falls back to the POST framebuffer or the current scanout surface only when the entry is
  empty or unknown; the engine copy keeps the first behaviour and drops the fallback. Pitch and format come
  from that allocation's private data, or from the standard allocation description for surfaces dxgkrnl
  created; a destination pitch different from the source pitch is honoured row by row.
- Rectangles: `SrcRect` and `DstRect` of equal size (no scaling, no colour conversion), `pDstSubRects` clipped to
  `DstRect` and to both allocations' extents; out-of-bounds rectangles are rejected before any engine work.
- Context: the present arrives on the WSI's present context (non-UMD, node 0) today; the KMD Blt must also
  accept it when the present arrives on a UMD context, which `Bc250WddmPresent` currently refuses.
- Ordering: the copy is emitted on the ring of the presenting context's node and completes the present's
  fence on that ring; the source is protected by the monitored-fence wait the WSI queues ahead of the present;
  residency of both allocations is held by dxgkrnl for the length of the present and by the fence for the
  length of the copy.
- Gate: the current software packet (`EnablePresentBlit`, INF default 0) is the plumbing control for this path:
  with the gate on, the WSI's present must reach `WddmPresentBlit` with an LB7A-described source and produce the
  frame on screen with `copy_us = 0` in the WSI and no CPU work on the presenting thread. That run proves the
  route (hypothesis H1 of `facts.md`: a DWM-composed `D3DKMTPresent(Blt)` reaches `DxgkDdiPresent`) before the
  engine copy exists. The software packet goes once the engine copy passes ADR 0018 point 5.

## Plan

1. WSI `kmt` path in the RADV fork (branch `amdgpu-wddm/radv-wddm2-wsi-kmt`): dispatch entry, hook,
   LB7A surface allocation in the winsys, present context, present log path, CPU fallback switch. Built as a
   candidate ICD, not promoted.
2. Plumbing control in the lab, under a window agreed with the KMD side: `EnablePresentBlit=1`, `vkcube` and one
   DXVK title, PresentMon capture, the E43 log, the KMD guard log for `WddmPresentBlit` entries, screenshot
   oracle. Result recorded as a `facts.md` row on H1 and on the `kmt` path's per-frame cost.
3. KMD engine Blt (ADR 0018 KMD item) per the contract above, measured against the same log.
4. The composition-swapchain flavour (C) after M13/M15, if a measurement shows a benefit over B (its end state
   is the zero-copy binding of the Vulkan image to the DXGI back buffer).

## Open questions

- Does dxgkrnl accept a `D3DKMTPresent(Blt)` with `hWindow` from a context whose device has no D3D runtime
  device, in a DWM-composed session, and route it to the redirection surface (H1)? The OpenGL ICD contract says
  yes; the control run decides.
- What does `bOptimizeForComposition` report on that route, and does dxgkrnl require a
  `D3DKMT_PRESENTHISTORYTOKEN` on it (the documented ICD example sets none)?
- Which allocation does dxgkrnl name as the destination on the composed route, and what private data does it
  carry (a standard allocation, or none, which the KMD Blt must handle by the standard allocation description)?
