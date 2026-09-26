# E31: Vulkan Win32 WSI on BC-250

Status: M10 first-picture criteria accepted in M476, 2026-09-25.

## Goal
GPU-render upstream vkcube using the BC-250 RADV ICD, present real Win32 swapchain
images, collect lab screenshots and measure cadence with an explicit synchronization
contract. Owner permits CPU presentation copies. DWM CPU rendering and Vulkan GPU
rendering are separate paths.

Initial baseline: signed KMD147, RADV DB886B8D..., desktop UMD FF864DB8....
First bounded control uses MESA_VK_WSI_DEBUG=sw to select existing CPU presentation
without rebuilding. This diagnostic is not a permanent fix or FIFO acceptance.

PROVENANCE: KhronosGroup/Vulkan-Tools, Apache-2.0, commit
6fe2055cf2fa921d52a4c6a31528cfc279a6977f (ref/Vulkan-Tools). Upstream C cube built
unchanged with MSVC and current Mesa Vulkan headers. Mesa is MIT.

## Initial procedure
Pinned SSH, intended ICD hash, health, 1000MHz/820mV, temperature below85C and STOP
clear. Launch only on the lab's existing interactive session:320x240,2frames,
30-second task deadline. Preserve native exit/stdout/stderr and health.
Timeout/device loss stops GPU testing until inspected. No unlock/login, firmware
write, driver replacement or executable window on the development PC.

## Final acceptance
- Correct support, formats, image count/extent and present modes.
- Render completion before CPU reads, acquire/present ownership and semaphore reuse.
- FIFO tied to compositor/display progress, not a Sleep-based FPS cap.
- Resize, minimize/restore, old/new swapchain lifetime and resource cleanup.
- Validation/synchronization checks and applicable WSI CTS with pinned versions and
  explicit coverage. Passing cube alone is not formal certification.
- Defined-image checks, color/orientation, multiple physical-lab screenshots.
- Measured FPS/frame timing and exact source/binary/ICD/device witnesses.
- ADR for CPU presentation and hardware VSync, refining ADR0011 under owner's
  explicit M10 CPU-copy permission. M9 and M13 remain separate unfinished work.

## Initial CPU WSI candidate

The candidate patch is wsi-cpu-fifo.patch (after the E27 RADV-main integration).
On the lab, stock cube2frames and instrumented120frames pass without the sw debug
flag. The short run measures52.42present returns/s, median16.83ms, and one DWM
refresh counter increment per noncapture interval. Independent frame correctness,
physical cadence and lifecycle/conformance controls remain pending. Runtime source,
hashes and private screenshots: scratch/m10/wsi-fifo/RESULT.md and sibling capture.

## M475 validation subset
19 selected Win32 WSI CTS cases and a120frame core/synchronization VVL control pass.
RGBA8 and BGRA8 each match all76719 pixels of an odd321x239 visible attachment-clear
oracle. Per-case loader witnesses prevent counting the earlier old-ICD runs.
Evidence: ../../evidence/windows/2026-09-25-E31-wsi-candidate/RESULT.md.
Longer timing, resize/minimize/restore and final architecture review remain open.

## Final acceptance: M476
All recorded M10 gates have scoped passing evidence in
[the final report](../../evidence/windows/2026-09-25-E31-m10-acceptance/RESULT.md).
Final ICD9C40083C includes the Win32 extent and CPU-status locking fixes. Earlier
candidate sections above are historical. Full conformance and long soak remain separate.
