# M746 - Window buffer DISPLAYABLE flag reaches the D3D11.1 UMD

Window005 sourceab85a60c, manifestCA2EBC894BA00188D8F6DA76F3421A39BED62B2779E0FE2482562FB1DD65C5AD.
Diagnostic shell compiled against frozen DXVK80352134 ABI1.3; the exact engine
and ICD pass the host module admission gate. GPU device creation succeeds.

The rejected request is TEXTURE2D BGRA8_UNORM64x64, bind0xA8 (PRESENT, render
target, shader resource), misc0x20002 (SHARED and DISPLAYABLE_SURFACE), DEFAULT,
no CPU map, one mip/array/sample, quality0, non-null runtime handle and no
primary descriptor. convert_runtime_resource strips SHARED but previously
left DISPLAYABLE for the ordinary COM converter, which rejects unknown flags.
GPU swap-chain returns887a0005/exit4, before any scene or successful Present.

CPU control passes. Independent CPU171 restoration, postflight and both Job
closures pass44.6187074s. Task Missing observed2026-09-28T14:28:43.0381133Z.
No KMD/DWM/OS restart. Raw logs: scratch/m14/window005-ops. Selected excerpt
omits module paths, addresses and process dump data.

The fix admits DISPLAYABLE on the runtime LB7A surface path (linear256-byte
pitch), strips the allocation intent from the ordinary engine texture flags,
and retains the distinction from an actual VidPn primary. The shared runtime
handle, allocation callback and image import stay in use. Unknown restriction
flags remain rejected. Windows header contract: WDK10.0.26100 d3d10umddi.h
D3DWDDM2_0DDI_RESOURCE_MISC_DISPLAYABLE_SURFACE; D3DDDI_ALLOCATIONINFO2.Primary
and VidPnSourceId are not inferred from displayability alone. Host regression
covers the measured descriptor, displayable-only resources and refusal of
RESTRICT_SHARED_RESOURCE_DRIVER. GPU admission with the fix remains to measure.
