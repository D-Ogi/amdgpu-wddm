# M475: WSI candidate controls

2026-09-25, unit A. Measured acceptance subset; M10 remains active.

- KMD147, desktop UMD CB1480EC (M474), unchanged Windows boot.
- Test ICD SHA256 3A03A1729F678A492D22220702E37A5BB4F3F9C2D23DE747075A139E20875F72.
  Mesa f333dd6d plus E27 integration and the included CPU WSI patch.
- GPU Vulkan rendering, CPU staging and GDI presentation, llvmpipe DWM.
  No MESA_VK_WSI_DEBUG override and no global ICD change.
- 19 selected Win32 WSI CTS cases pass, including acquire exhaustion/timeouts,
  retired swapchain lifetimes and 600-frame basic rendering.
  Each process has a loader path witness for the candidate, checked before PASS.
- CTS 93bca01861b0e3ef3c387027a9791e6d065f900c. Executable identity in cts/binary.json.
- Core/synchronization VVL 1.4.363 control: 120 stock cube frames, exit 0,
  layer insertion witnessed, no reported validation errors/hazards.
- Exact screen-region oracle: RGBA8_UNORM (37) and BGRA8_UNORM (44),
  321x239 each, 76719 pixels each, zero mismatches. GPU attachment clears make
  red/green/blue/white quadrants; client.png is a crop of the real lab screenshot.
  No synthetic replacement or image enhancement. Whole desktops stay private.
- Pixel capture waits 750 ms for the Windows opening animation and requests
  square corners on this test window. Initial attempts caught animation and then
  36 rounded-corner pixels. Those private captures were retained, not overwritten.
- Post-controls health flags 15; native 1000 MHz, VID116, temperature below 68 C.
  No reboot, PnP transition or DWM restart during these controls.

## Harness correction and limits

Earlier elevated CTS attempts selected the globally registered M8 ICD despite
environment overrides. Their passes and AV are not candidate results. Run04 uses
the normal interactive token, an app-local loader and per-case ICD path checks.
The test binaries are unchanged upstream CTS. Privileged health/clock preflight
runs separately via the existing SSH controller.

The image oracle proves opaque color, orientation and odd extents through the
screen copy path. It does not prove shader arithmetic or tear-free physical scanout.
CTS is a selected regression subset, not Khronos certification.
Longer ETW cadence, resize/minimize/restore and final architecture review remain open.

PROVENANCE: KhronosGroup/VK-GL-CTS, KhronosGroup/Vulkan-Tools and
KhronosGroup/Vulkan-ValidationLayers: Apache-2.0; Mesa: MIT.
