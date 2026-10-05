# M476: M10 first visible Vulkan graphics accepted

2026-09-25, unit A. M10 meets its recorded first-picture criteria. This is
GPU Vulkan rendering with CPU presentation. It is not full Vulkan conformance,
a hardware-accelerated DWM milestone, or M9 recovery acceptance.

## Final identity
- KMD0.7.147.1:5FCB554AE77B04506AA80B4590EE33D7CAA4F8D5A6E720CA89666F736760EC31.
- Desktop UMD CB1480EC (M474), Mesa llvmpipe CPU rendering with LLVM23.1.2.
- Vulkan ICD:9C40083C08210C71A6F6DC7EA12F7C1205BBC6AA1DFE48E596C8495B6C2A9798.
  Mesa f333dd6d plus E27 WDDM2 integration and included WSI patch.
- Installed in C:/BC250/m10/wsi-final; launcher run-vkcube.cmd pins this ICD.
  Global ICD registration is unchanged. No diagnostic sw override.
- CTS93bca01861b0e3ef3c387027a9791e6d065f900c;
  Vulkan-Tools6fe2055cf2fa921d52a4c6a31528cfc279a6977f;
  VVLfd8605f99aa030a4693cf42362fedfe4ff73be78 (1.4.363).
  Binary sizes and SHA256:identities.json.

## Acceptance
| Criterion | Evidence and result |
|---|---|
| Visible GPU scene | timing/cube-1.png and cube-61.png show the upstream textured cube at different angles; exact client-region crops from bc250mon screen captures |
| Defined image | RGBA8 and BGRA8,321x239,76719 pixels each, zero mismatches in format-37/ and format-44/ |
| Win32 WSI | cts/:19 selected cases pass with candidate ICD path witnesses; surfaces, caps, formats, present modes, acquisition/timeouts, old-chain lifetime and basic rendering |
| Window lifecycle | lifecycle/:640x480 ->433x277 -> minimized -> restored ->640x480;120 successful presents plus one expected OUT_OF_DATE; native0 |
| Validation | Same lifecycle with core/synchronization VVL loaded at instance/device scope; no reported VUID or synchronization hazard |
| Frame rate | timing/:600 successful presents, native0;50.865 present returns/s excluding two documented screenshot holds; median16.8583ms, p9549.1101ms |
| Refresh synchronization |919 DxgKrnl VSync QPC samples during the run;59.94997Hz, median16.68025ms; GdiFlush then DwmFlush, no sleep-based frame cap |
| Architecture decision | ADR0015 refines ADR0011 for the owner-authorized CPU WSI path, retaining hardware DCN scanout and IH VSync |
| End state | final/post-health.log:health flags15,1000MHz/VID116,66.625C, same Windows boot, Notepad still running, zero remaining test processes |

## Fixes and review
GPU fence errors propagate before CPU reads. Per-call GDI objects are acquired and
released on the same thread. RGBA pixels are swizzled to the DIB's BGRA order.
Only opaque CPU composition is advertised. Retired CPU chains reject new acquisition.
CPU status reads and writes use the acquisition mutex; presentation returns its
local result rather than reading unlocked shared status.

Win32 minimum/current/maximum extents now all follow the client area, including0x0
while minimized. The first lifecycle run reached an internal zero-width assertion
with the old inconsistent caps. After that fix, a test-only WM_TIMER restore was
starved by vkcube's continuously invalid WM_PAINT region. The final harness posts
one restore message from a delayed helper thread instead. No queue/render/sync
algorithm in the upstream scene is replaced. All final controls use the same ICD.

## Measurement limits
FPS is successful present-return cadence, not a count of physically distinct images.
DwmFlush synchronizes the caller with compositor presentation; its contract is not
a per-image scanout timestamp. ETW independently witnesses hardware refresh, and
screenshots witness actual visible contents. Median cadence is close to one refresh,
but the p95 shows missed refresh opportunities. No stable60FPS or Linux-performance
parity claim. The CPU copies and llvmpipe DWM remain performance limits.

The two screenshot holds after frames1/61 are excluded only from cadence intervals;
all other intervals remain, including outliers. Timing source and parser are included.
ETL remains private; vsync-qpc.json contains the relevant extracted timestamps and
identities.json records the exact ETL hash. No zero-event-loss claim is made.

This19-case CTS selection is explicitly listed in cts/cases.txt. Full must-pass
conformance, broader applications and long mixed-load soak remain M12/M11.
M9 and M13 remain open. No global ICD promotion is implied.

## Provenance and privacy
Khronos Vulkan-Tools, VK-GL-CTS and VVL:Apache-2.0; Mesa:MIT.
PresentMon's local DxgKrnl schema (MIT) supplied the ETW keyword definition.
Driver contract:
https://docs.vulkan.org/spec/latest/chapters/VK_KHR_surface/wsi.html
Compositor synchronization:
https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmflush

Raw originals remain in scratch/m10. PCI/display instance suffixes are redacted
in archived text logs; hardware IDs, results and candidate library paths are retained.
Whole-desktop images and application dumps are not published. PNGs here are actual
client-region crops, with no generated replacement pixels.
