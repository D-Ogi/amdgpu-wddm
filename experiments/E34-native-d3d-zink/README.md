# E34: native D3D UMD through Zink

PROVENANCE: Mesa (MIT), base 05e6c9622e1; retained local D3D frontend from the deployed CPU UMD source. This is not DXVK.

Hypothesis: the existing native D3D10 state tracker can execute shader draws through Zink/RADV on the BC250 with exact pixel output.

Procedure: build the isolated Zink-only UMD, explicitly select the sole PCI 1002:13FE adapter by its DXGI LUID, and load the DLL through D3D_DRIVER_TYPE_SOFTWARE (the runtime's custom-driver entry mechanism). No CPU Gallium renderer is linked. Use the registered Windows Vulkan ICD; do not change system UMD registration. Run native-control with a 45-second process timeout and stop on its first failure. Check 4096 RGBA pixels after a blue clear and after a red full-screen shader triangle. Record loaded module paths, artifact hashes, exit status and DWM state. Run only on the lab, never the development PC.

Expected: both reads match all pixels, device removal is S_OK and the process exits0. A create error, mismatch, device loss or timeout fails this stage. A passing result proves only bounded native offscreen rendering, not shared surfaces, presentation, GPU DWM or milestone acceptance.

Outstanding: primary allocations currently use the CPU import contract. Native GPU sharing and synchronization must be implemented before any DWM deployment. The explicit environment LUID is a prototype selection mechanism, not the final runtime adapter contract.

Result: run011 passes clear and shader-draw readback plus clean destruction. See [evidence](../../evidence/windows/2026-09-26-E34-native-d3d-zink/RESULT.md).

## Native shared allocation probe

Hypothesis: a shared linear LB7A allocation created on one WDDM device can be
imported by the candidate RADV on its own device, cleared by the GPU and read
through the original allocation with 4096 exact blue pixels.

Procedure: use scratch/m13/shared-import/probe.cpp and its bounded runner on the
existing Windows boot. Create one 64x64 RGBA allocation with E26R shared policy,
CreateShared and NtSecuritySharing; export through D3DKMTShareObjects using
OBJECT_ATTRIBUTES and SHARED_ALLOCATION_ALL_ACCESS. Load only the app-local
candidate ICD, match the adapter LUID, import OPAQUE_WIN32 with LB7A metadata,
clear, wait up to five seconds for the fence and read through D3DKMTLock2.
The process timeout is 45 seconds. Do not change system registration.

Expected: allocation, export, import, submission and destruction succeed; all
pixels match and the caller retains ownership of its NT handle. Any failing
stage terminates this probe. This does not establish runtime-owned back-buffer
sharing, asynchronous inter-device synchronization or DWM presentation.

Contract references: local ref/ddi-display/d3dkmthk.md, D3DKMTShareObjects and
D3DKMTOpenResourceFromNtHandle; ref/Vulkan-Docs/chapters/memory.adoc, Win32 handle
ownership. Architecture review: ref/m13-notes/zink-behind-d3d10umd.md, section 6.4.

Shared probe result: run006 passes all stages and 4096 pixels; see [M532 evidence](../../evidence/windows/2026-09-26-E34-shared-import/RESULT.md).

## Native D3D shader into imported memory

Hypothesis: the same shared LB7A surface can be a Zink memory-object render target
for native D3D, retaining exact clear and shader output. The isolated probe passes
an app-owned NT handle to the custom UMD; this is not runtime back-buffer sharing.
Run the M531 D3D clear/triangle checks, then Lock2 the original KMT allocation and
require 4096 exact red pixels. Use the M532 ICD during the bounded process, restore
the previous ICD in finally, and preserve logs. Failure stops this experiment.

Native shared result: run002 passes both D3D and original-allocation pixel checks; see [M533 evidence](../../evidence/windows/2026-09-26-E34-native-shared/RESULT.md).

## Runtime allocation inventory

Hypothesis: a hardware D3D device can create the native context and allocations
through runtime callbacks, while flag-dependent sharing can be measured before
choosing hosted integration. Temporarily register the diagnostic UMD for one
bounded hardware-device process, without restarting DWM; restore registration in
finally. Create normal, legacy-shared and NT/keyed-mutex 64x64 textures. Record
DDI MiscFlags, AllocateCb results, ShareObjects status and deallocation. This mode
intentionally stops before GPU import/presentation and must never serve DWM.
Expected: native context and AllocateCb succeed; record rather than presume which
resources accept NT export. No image correctness claim follows from allocation.

Runtime result: run002 passes context/allocation callbacks, with hKMResource zero; [M534 evidence](../../evidence/windows/2026-09-26-E34-runtime-callbacks/RESULT.md). Continue with the [hosted plan](hosted-plan.md).

## Per-device screen lifetime

Hypothesis: native D3D devices can each own a Zink screen; destroying one does not
invalidate GPU rendering on the other. Create two hardware devices through the
M534 process router. Clear/read 4096 exact red pixels on the first and blue on the
second, destroy the first, then clear/read green on the second. Exit0 and baseline
restoration required. This establishes frontend ownership only: standalone RADV
still caches its winsys, and callback dispatch into RADV is not implemented yet.

## hKMResource controls

Hypothesis: the observed zero hKMResource can be localized by recording nonzero
input runtime handles and actual compiled ALLOCATE layout, then checking whether
the shipping CPU UMD produces a usable legacy shared handle. Build a WDK26100
layout utility with header provenance; log identical offsets from the actual UMD.
Run GetSharedHandle and OpenSharedResource on two native hardware D3D devices,
first on the unchanged CPU baseline, then the instrumented candidate through the
bounded process router. Record every HRESULT and restore original libraries.
Do not infer the cause of a zero callback field merely from a successful API call.

### Per-device ownership control

Before hosted dispatch integration, create two native hardware D3D devices with
separate Zink screens; verify 4096 exact red and blue pixels, destroy the first
device, then verify 4096 green pixels on the second. Use the same bounded router
procedure; keep DWM on the CPU UMD. A pass proves screen lifetime isolation, not
separate kernel devices, hosted callbacks or GPU desktop composition.

Controls measured in [M535](../../evidence/windows/2026-09-26-E34-runtime-controls/RESULT.md). Shared handle retrieval works; hosted runtime rendering remains open.
