# ADR 0018: the Vulkan WSI presents through an engine, never through a CPU pixel copy

Date: 2026-09-27. Status: accepted (owner decision of 2026-09-27: the CPU transport of ADR 0015 was an M10-only
allowance and is not wanted for M12 or later). Supersedes ADR 0015. Restores ADR 0011 point 4 in full and makes
ADR 0011 point 1 due. Nothing in this ADR is a measured result unless it cites a `facts.md` row.

## Context

ADR 0011 point 4 (2026-09-22) decided that windowed presents that need a copy get one from an engine, never from the
CPU. ADR 0015 (2026-09-25) suspended that for M10, the first visible Vulkan milestone: the winsys selects Mesa's
CPU-image Win32 WSI path (`wsi->sw = true` on the BC250), which copies the rendered image into a DIB on the CPU,
BitBlts it into the window and waits for DwmFlush. ADR 0015 named the cost ("CPU copies and CPU DWM limit
throughput") and predicted that hardware presentation would replace the transport later.

M10 was accepted on 2026-09-25 with that transport. On 2026-09-27 E43 measured it on a real D3D12 game through
vkd3d-proton (facts M580): the present call takes a median 15.8 ms of a 33.4 ms frame on the presenting thread
(CPU copy 3.2 ms, BitBlt 1.3 ms, DwmFlush 11.3 ms) and the scene is quantised to 29.7 presents/s. The owner then
stated that the allowance belonged to the M9/M10 stage only and that the limitation is not wanted.

The other half of the path is in the kernel driver: `DxgkDdiPresent` (`Bc250WddmPresent`, `driver/kmd/wddm.c`)
has a copy-free Flip path for DWM's primary (`SetVidPnSourceAddress`, facts M545) and, for Blt presents, a
software packet whose pixels are copied by the CPU at `SubmitCommandVirtual`, behind the diagnostic gate
`EnablePresentBlit` (ADR 0011 point 1, "removed once point 2 works"). Point 2 works; the gate and the copy are
still there.

## Decision

1. **The Vulkan WSI presents without a steady-state CPU pixel copy, in user mode and in the kernel driver.** The
   rendered image reaches the compositor through WDDM allocations and `D3DKMTPresent`, ordered after the render
   fence by a monitored fence; any copy the present needs is done by an engine on the ring of the presenting
   context's node (CP `DMA_DATA` on the gfx node, SDMA when the context is on the COPY node of ADR 0013), and the
   present's fence is signalled by that same ring.
2. **The M10 CPU transport becomes a diagnostic fallback behind an explicit environment switch**, off by default.
   It is kept only as a control for pixel oracles and for a machine without the engine path, and it is named as
   such wherever it is used. ADR 0015's "without a diagnostic environment override" is withdrawn with it.
3. **DWM's Flip path is not touched.** It is copy-free today (facts M545, M576: constant CPU blit count during a
   GPU DWM run) and stays so; this ADR is about Blt presents.
4. **ADR 0011 point 1 is executed:** the software Blt packet and its CPU copy leave the kernel driver once the
   engine copy passes the acceptance below. Until then they stay behind the gate, closed by default.
5. **Acceptance** (also the M12 presentation requirement in `00-goal-and-roadmap.md`): the E43 present log shows
   no copy phase; the KMD's CPU blit counter does not advance during the application's presents and the
   CPU-copy exclusion instruments of the accelerated-desktop work show no full-frame CPU copy attributable to
   them; the presented pixels match the rendered image (E31/M475 style oracles) for at least two row pitches and
   for clipped and dirty-rectangle presents; present order holds under queued multi-frame work; allocations stay
   resident through completion of the copy; unsupported formats, layouts and out-of-bounds rectangles are
   rejected before any engine work; the Witcher 3 DX12 scene of E43 is measured again with the same save and
   settings. The matched Linux comparison of M12 stays a separate, unpassed performance gate.

## Consequences

- Two pieces of work, one contract between them: the WSI side (swapchain images as presentable allocations, linear
  layout first, `D3DKMTPresent` with the window's redirection target, the fallback switch) and the KMD side (the
  Blt path of `DxgkDdiPresent` as an engine copy honouring pitch, sub-rectangles and layout, with the residency
  and fence handling the paging path has, facts M114). The pitch, sub-rectangle and layout expectations are
  written down as a short design note before either side lands. Whether tiled swapchain memory needs a
  tiled-aware copy or a linear swapchain layout suffices is decided by measurement.
- The WSI side can be built and tested against the current KMD first: the software packet still copies on the
  CPU inside the kernel, so that control measures the WSI side only and the KMD blit counter must then advance
  once per present. The KMD side removes the last copy; acceptance needs both.
- `docs/00-goal-and-roadmap.md`: the M10 row's "a present path that copies through the CPU is acceptable" was
  true for M10 only and is annotated so; the M12 row gains the presentation requirement of point 5.
- PresentMon becomes usable for the port once presents go through dxgkrnl (facts M577 explains why it saw nothing
  on the GDI path); the in-stack present log of E43 stays as the finer instrument.

## References

- ADR 0011 (present is a flip), ADR 0013 (node layout), ADR 0015 (superseded).
- Facts M114, M475, M545, M576, M577, M580.
- A DXGI composition swap chain is a second candidate transport for point 1, and it survives the app-local
  DXVK and vkd3d-proton DLLs that our games ship: facts [M792](../facts/d3d.md#m792),
  [M793](../facts/d3d.md#m793) and [M794](../facts/d3d.md#m794), measured on the development PC
  (`evidence/windows/2026-09-28-E56-dxgi-shadowing-dev-pc/`). Nothing there measures our own driver.
- `evidence/windows/2026-09-27-E43-witcher3-dx12-present-log/RESULT.md`.
- [Vulkan WSI through DXGI](../design/vulkan-wsi-dxgi.md) (2026-10-07): the route on a D3D12 device of our
  adapter that replaces the kernel-thunk plan of point 1. It was the default until its first lab trial froze
  every client that used it before the first present (BD-105, 2026-10-09). It is opt-in now, `gdi` is the
  default and the switch back, and no frame has been presented through it.
- Mesa fork `amdgpu-wddm/radv-wddm2-kmt-enum` c34ab7cd: `src/amd/vulkan/winsys/wddm2/radv_wddm2_wsi.c`,
  `src/vulkan/wsi/wsi_common_win32.cpp`; `driver/kmd/wddm.c` `Bc250WddmPresent`.
