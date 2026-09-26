# E34: native D3D UMD through Zink

PROVENANCE: Mesa (MIT), base 05e6c9622e1; retained local D3D frontend from the deployed CPU UMD source. This is not DXVK.

Hypothesis: the existing native D3D10 state tracker can execute shader draws through Zink/RADV on the BC250 with exact pixel output.

Procedure: build the isolated Zink-only UMD, explicitly select the sole PCI 1002:13FE adapter by its DXGI LUID, and load the DLL through D3D_DRIVER_TYPE_SOFTWARE (the runtime's custom-driver entry mechanism). No CPU Gallium renderer is linked. Use the registered Windows Vulkan ICD; do not change system UMD registration. Run native-control with a 45-second process timeout and stop on its first failure. Check 4096 RGBA pixels after a blue clear and after a red full-screen shader triangle. Record loaded module paths, artifact hashes, exit status and DWM state. Run only on the lab, never the development PC.

Expected: both reads match all pixels, device removal is S_OK and the process exits0. A create error, mismatch, device loss or timeout fails this stage. A passing result proves only bounded native offscreen rendering, not shared surfaces, presentation, GPU DWM or milestone acceptance.

Outstanding: primary allocations currently use the CPU import contract. Native GPU sharing and synchronization must be implemented before any DWM deployment. The explicit environment LUID is a prototype selection mechanism, not the final runtime adapter contract.

Result: run011 passes clear and shader-draw readback plus clean destruction. See [evidence](../../evidence/windows/2026-09-26-E34-native-d3d-zink/RESULT.md).
