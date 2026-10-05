# ADR 0015: M10 GPU Vulkan with CPU window presentation

Date: 2026-09-25. Status: **superseded by [ADR 0018](0018-engine-present-for-vulkan-wsi.md) on 2026-09-27**
(owner decision: the CPU transport was an M10-only allowance and is not wanted for M12 or later). The CPU
path survives only as an explicit diagnostic fallback, off by default. Scoped runtime acceptance for M10 was
M476/E31.

## Context
The owner explicitly permits CPU presentation copies for the first visible Vulkan
milestone. BC-250 has working GPU rendering through RADV/WDDM2 and a CPU-rendered
DWM desktop. The D3D12/DXGI WSI bridge is not available for this backend.

## Decision
For BC-250, select Mesa's CPU-image WSI path in the winsys, without a diagnostic
environment override. Keep rendering and render-to-staging GPU work on RADV.
Wait for successful GPU completion before the CPU reads the image. Copy or swizzle
opaque pixels into a DIB, submit BitBlt, flush GDI and then wait for DwmFlush.
Release the image for acquisition after that sequence.

Use thread-local GDI object lifetimes. Retired CPU swapchains reject new acquisition;
already acquired old images retain their normal presentation lifetime. Win32
minimum, current and maximum surface extents follow the client window, including
zero while minimized. Apps resume swapchain creation when the window has area again.

DWM remains the presentation engine. Its primary reaches DCN through the existing
hardware flip and IH VSync path. CPU Vulkan presentation does not imply CPU Vulkan
rendering; DWM's llvmpipe rendering remains a separate cost.

## Scope and consequences
This explicitly refines ADR0011 point4 for M10 Win32 Vulkan. It does not reinstate
the old KMD firmware-framebuffer diagnostic copy as the display architecture.
Future hardware presentation can replace this transport without changing the
Vulkan-facing ownership and synchronization contract.

Present-return FPS, compositor counters and hardware VSync are reported separately.
DwmFlush is compositor synchronization, not a timestamp proving physical display
of each particular application frame. CPU copies and CPU DWM limit throughput.
No formal Khronos certification is inferred from a selected CTS subset.

## References
- E31 and M475 record implementation and initial correctness controls.
- Khronos Win32 surface rules:
  https://docs.vulkan.org/spec/latest/chapters/VK_KHR_surface/wsi.html
- Local source: Mesa src/vulkan/wsi/wsi_common_win32.cpp, RADV WDDM2 winsys.

- Microsoft compositor contract:
  https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmflush
