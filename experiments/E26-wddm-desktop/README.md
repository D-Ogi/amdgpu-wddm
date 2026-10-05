# E26: full-WDDM desktop

## Current desktop build (M412, 2026-09-24)

The active desktop uses Mesa main f9a2d34a19c496e552b6cd603e7807f1025a2e98
(26.3.0-devel) with LLVM23.1.2. Apply mesa-main-bc250-gallium.patch to a clean
checkout at that revision; it is consolidated and includes the local tests.
Use build-mesa-main-llvm23.cmd with the workspace toolchains described in
[the current baseline](../../docs/research/m13-present-baseline.md).
The old build-d3d.cmd and incremental prototype patches below document earlier
softpipe experiments. They do not select the active llvmpipe/LLVM23 build.
[Current evidence](../../evidence/windows/2026-09-24-E26-desktop-resume/mesa-main/RESULT.md)
records live controls and the remaining limits. The Vulkan ICD is separate.

## Original experiment history

The M8 run shows repeated DWM failures 0x8898008d. The installed SDK names
this MILERR_NO_HARDWARE_DEVICE. The D3D UMD is a stub returning E_NOTIMPL.
This contradicts the earlier unmeasured attribution to alpha handling.

H1: registering the installed Microsoft WARP D3D11 UMD on the lab adapter
allows device creation and breaks the DWM restart loop. This is a diagnostic,
not a claim that the WARP/KMD ABI is supported or that GPU acceleration exists.
Keep the stub registration in a backup, test full WDDM without GPU bring-up,
record D3D11 creation and DWM events, then restore registration and display-only
in a finally block. No firmware, OS binary, or disk layout changes.

Expected: S_OK for D3D11 and a stable DWM if sufficient; otherwise retain the
failure HRESULT and pursue a real UMD/presentation path. H1 failed; see the evidence summary below.

H2: Mesa's D3D10 UMD with softpipe can create a D3D device on the BC-250
adapter. Diagnostic only: CPU shader execution is not hardware acceleration.
Build Mesa 801c9763c6 with d3d10umd, softpipe, static CRT and a per-process
entrypoint trace. Register the absolute DLL path for D3D10/11, enable full
WDDM without engines, run the same hardware/WARP positive-control probe,
collect DWM events and restore registration/display-only in finally.
Device creation success alone does not prove shared resources or composition.
H3: restarting DWM after UMD registration distinguishes a newly created composition device from the process surviving the adapter restart. Repeat H2, terminate only dwm on the lab, wait 45 seconds, capture scanout and events, restore in finally.
H4: an explicit R8G8B8A8 green clear, staging readback of all 640x480 pixels, and Present distinguish the rendering and display paths. Run render_probe.exe in the lab interactive session via a temporary scheduled task while H2 is active. Capture scanout during its 300-frame loop and retain HRESULTs and pixel mismatch count. No new GPU engine initialization.
H5: a runtime-created virtual context and CPU-visible LB7A upload allocation can pass the software-rendered frame through DXGI PresentCb. Use the callback build, log each HRESULT, run H4 with a 60-second process deadline, and restore in finally. A callback failure is not converted to success or hidden behind GDI.
H6: copying in DxgkDdiPresent races context selection and residency. Serialize the allocation list and bounded rectangles into driver-private data, consume one DMA buffer per packet, and execute the CPU diagnostic copy in SubmitCommandVirtual before fence completion. Preserve multipass progress for more than 128 rectangles. This is still the gated CPU diagnostic path, not the final GPU blit/flip implementation.
H7: per-resource kernel allocations associated with hRTResource, a real SetDisplayModeCb, and destination allocation propagation remove PresentCb E_INVALIDARG. Compare callback2 and resource builds on KMD 0.7.49.1; software rendering remains the baseline.
H8: allocate present resources during CreateResource, map GPUVA and make them resident using the WDDM2 paging queue, wait for its monitored fence (5-second limit), and deallocate through Deallocate2Cb. Expect valid source addresses and no device-hung error in the color control; otherwise retain the exact callback failure.

H9: respecting Present destination allocations prevents offscreen window updates from overwriting scanout. KMD 0.7.50 adds native RGBA/BGRA conversion and rejects software present packets from GPU dispatch. Mesa imports persistent Lock2 mappings as linear softpipe display targets, implements OpenResource and DXGI Blt, and returns S_OK for redirection. Validate a shared texture on a second D3D device with a distinct red/blue pattern, then capture DWM composition. Unsupported formats or rotations return failure. This remains a CPU diagnostic, with automatic display-only restoration.

H10: clearing FlagsWddm2.Value destroys reserved input fields belonging to dxgkrnl. Preserve those bits while initializing only KMD-owned output fields; repeat H9. The WDK marks FlagsWddm2 as output except reserved fields. The 64x32 control reached KMD in 0.7.52 yet the syscall returned STATUS_INVALID_PARAMETER; offline analysis and user-mode tracing retained under scratch/dwm, no live KDNET used.

H10 result: preserving reserved bits did not fix sharing. The 64x32 CreateAllocation input FlagsWddm2 was zero; the hypothesis as the cause of this failure is refuted. The field-preservation change is retained as contract hygiene. Capture a bounded 64 MB Microsoft-Windows-DxgKrnl ETW session during H9 to obtain the system validation diagnostic (H11).

H11 result: ETW explicitly reports that Shareable plus CpuVisible requires exclusively aperture segments, and warns about zero initial allocation priority. H12: use an E26R resource descriptor to select the aperture only for CPU-shared Mesa allocations, retain the LB7A allocation ABI, set a normal priority, and repeat the two-device control. Nonshared linear VRAM surfaces request physical contiguity because the diagnostic blit maps them physically.

## Current results

See [captured evidence and limitations](../../evidence/windows/2026-09-22-E26-d3d-desktop/README.md)
and facts M140-M142. The desktop objective is still open. The two patches are
diagnostic prototypes against Mesa 801c9763c6: mesa-diagnostic.patch adds tracing
and fixes TEMP-relative softpipe indexing; mesa-callback-prototype.patch adds
unfinished resource/presentation support. Neither is a production D3D driver.
The scripts are exact lab probes and require the pre-existing E19 helper,
C:\BC250\m8 tools, and the DLL in the corresponding experiment directory.

H13: the persistent DXGI_ERROR_MODE_CHANGE_IN_PROGRESS in DWM can be reproduced by a fullscreen primary outside DWM. QueryDisplayConfig already succeeds in both display-only and full WDDM. Run the existing successful windowed probe with an optional fullscreen entry/ResizeBuffers to native 1920x1200 BGRA, clear and one Present; record every HRESULT, leave fullscreen, restore display-only in finally. If it reproduces, trace the failing DXGI branch in this test process; otherwise investigate DWM-specific swapchain flags. No GPU engine bring-up.

H13 fullscreen entry returned DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, so it did not reproduce the DWM failure. H14: attach a bounded user-mode CDB to DWM on the lab, break at the first DXGI journal record of 0x887A0025, capture the call stack and immediately detach. Exact function RVA comes from the matching local PE/PDB. No kernel debugger. Restore display-only and terminate diagnostic debugger in finally. This identifies the emitting runtime branch without guessing a driver fix.

H15: SetDisplayModeCB records a failing kernel status while returning S_OK to the UMD; offline analysis of the matching d3d11.dll shows this for several NTSTATUS values. On the recovered 00:53:51 boot, repeat the successful windowed/aperture control with a bounded 64 MB DxgKrnl ETW trace, without CDB attachment. Inspect SetDisplayMode events and triage diagnostics. Existing allocation ETW is the positive control for the capture method. Restore display-only and registration in finally.

H15 result: DxgKrnl reports "0 != ShadowSurfaceData.Pitch" and "0 != StagingSurfaceData.Pitch". H16: fill the public output Pitch fields for standard shadow/staging/GDI surfaces with the same linear pitch as their allocation blob. Microsoft D3DKMDT_SHADOWSURFACEDATA specifies Pitch as a required output. KMD 0.7.55; repeat H15, expect both validation messages to disappear and assess DWM independently. Do not infer desktop success from validation alone.

H16 result: both public pitch validation messages disappeared in the 0.7.55 ETW trace; render/shared pixel controls and task exit passed. Desktop stayed black and recovered after display-only restoration/DWM restart. H17 (not run): register the UMD virtual context during CreateDevice, before primary setup. ETW reports 163 empty broadcast-context signals from fresh DWM PID1508; its UMD trace never reaches Present, where context creation currently happens. Build only while the lab stays display-only. This is a hypothesis, not a proven fix. The separate WDDM1.x-DDI warning maps offline to the diagnostic-only DxgkCbGetHandleData call in OpenAllocation; do not conflate it with the broadcast-context error.

H18: Microsoft-Windows-Direct3D11 RecordJournal ETW exposes the failing D3DKMTSetDisplayMode NTSTATUS hidden by SetDisplayModeCB. Offline matching d3d11.dll/PDB shows CallAndLogImpl calls RecordJournal on failure and RecordJournal emits ETW. Repeat H17 with this provider (32 MB bound), same KMD55 and early-context UMD, no debugger attachment. Expect an explicit kernel status or refute this capture route. Restore display-only and restart DWM in finally.

H18 result: Direct3D11 ETW captures239 SetDisplayMode failures STATUS_GRAPHICS_PRESENT_MODE_CHANGED (0xC01E0005). H19: DescribeAllocation must report the same nominal60000/1000 refresh as the full VidPn mode; previously it returned NOTSPECIFIED. Offline DXGDEVICE::SetDisplayMode compares allocation/current-mode frequencies and returns the observed status when they differ. Change only that report (KMD56), retain early-context UMD, repeat H18. Success requires no steady mode-change errors and actual desktop presentation, not just callback S_OK.

H20: after H19, DWM blocks inside DrawIndexed (possibly a hidden assertion). Capture a bounded user-mode MiniDumpWriteDump snapshot with thread/stack memory, no DebugActiveProcess or kernel attachment. First verify capture/analysis on a sleeping lab PowerShell process. Then capture fresh DWM10s into the same H19 test, analyze offline and restore display-only. Raw dumps remain outside the repo.

H21: D3D10 translator leaves UIF/ELSE targets unset, so softpipe jumps to instruction0 and overflows loop stacks. H20 dump confirms hidden assertion LoopStackTop <32 at tgsi_exec.c:5380. A local bounded interpreter test reproduces stack growth in three shaders before correction; seven cases terminate with balanced stacks after assigning forward targets for explicit/generated conditionals. Run the same KMD56 probe with only this UMD change, capture a mini dump and scanout, restore Full0 and restart DWM in finally. Success requires compositor progress beyond the old assertion, with desktop visibility assessed separately.


## KMD147 shared CPU backing and diagnostic logging

Apply `mesa-shared-cpu-cache-v2.patch` after `mesa-main-bc250-gallium.patch`,
then `mesa-bounded-diagnostics.patch`. These are incremental patches for the
recorded Mesa f9a2d34a frontend. E26R v2 requires KMD0.7.147.1 or later;
never deploy this UMD on an older KMD. The new KMD still accepts the old v1 UMD.
Shared non-primary CPU-read resources request Cached system backing. Primary
allocations remain uncached; this does not introduce a primary shadow buffer.

Normal diagnostics retain renderer identity, errors/assertions and sampled
frame timing. `BC250_UMD_VERBOSE=1` at process creation enables full entrypoint
tracing (first10000 file records, plus retained categories); it is diagnostic
and materially distorts CPU composition timing. OutputDebugString is used only
with an attached debugger. Do not benchmark with verbose tracing enabled.
M470 records matched memory/content controls and remaining animation stutter;
see `docs/research/m9-acceptance-status.md` for current acceptance limits.

## Shared surface raster padding (M474)

Apply mesa-surface-padding.patch after the main/cache/diagnostic patches. Backing
pitch and height cover complete llvmpipe raster blocks; imports and rotation retain
real allocation geometry. [Validation](../../evidence/windows/2026-09-25-E31-surface-padding/RESULT.md)
includes Notepad startup and five shared surface dimensions. KMD147 unchanged.
