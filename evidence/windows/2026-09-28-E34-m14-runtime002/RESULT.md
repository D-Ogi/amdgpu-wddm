# M14 runtime002: registry writes succeed, native runtime still selects CPU UMD directly

Unit A, 2026-09-28. Source31dbe13f with literal trial identity runtime001/Runtime001 changed to runtime002/Runtime002 in a preserved generated source directory. Router rebuilt for the exact new process path; local selection, debug-child, registration and supervisor tests pass. Stage manifest119625B1FF9C30748FDB1F54C8A1974F87A477FECFD779095A9CA331D3693E63. Rendering binaries are byte-identical to M728: shell4EA6D1CE, engine253A, ICDC0CE, configEAB1, clientECA8. Debugger unchanged.

Hypothesis: fixing the writable RegistryKey lets the system runtime choose the temporary router for the fresh client. Independent SYSTEM task180s; bounded child phases Capture/Install/Cpu/Gpu/Restore/Verify. Same tiny offscreen CPU/GPU arguments, no app-local D3D or DXGI DLLs. The CPU route must witness the router before GPU admission.

Install passed. Its readback has exactly the original first slot followed by two runtime002 router paths. Cpu client then exited0 and wrote a measured result: system D3D11, PCI1002:13fe, FL10_0,64x64,two frames,one draw,checksum21f6950fc86132f4, no API failures. Debug output identifies llvmpipe. Loaded modules contain system d3d11/dxgi/compiler and the original bc250d3d.dll. No router or Vulkan ICD was reported. Cpu admission correctly failed at System runtime/router not witnessed, so Gpu never ran.

This shows that changing UserModeDriverName did not change library selection in this live-adapter trial. Cached OS/runtime driver-name state is a candidate explanation, not established by this measurement. It does not establish that a fresh adapter start would behave the same way.

Supervisor completed28.8522287s, failed-restored. Restore readback exactly matches the initial multi-string. Independent postflight and tree closure pass; task subsequently removed. CPU171/SYS65172CA1, health15 generation305821861289/epoch5,67.0C, unchanged boot and DWM. No KMD or DWM restart. Lab free.

The next diagnostic can use the earlier measured file-path router mechanism, with a separate baseline DLL copy for forwarding and independent durable restoration. No GPU or M14 completion claim follows from this CPU result. Debugged timings are excluded from performance comparisons.

result.json, Cpu.err, Cpu-debug.txt and registration readbacks are unedited. Cpu-selected.json excludes environment and timing fields; postflight-selected.json excludes the raw driver ring and device instance data. Full originals, generated sources, package and recipes stay under scratch/m14/runtime002-*.
