# M14 same-GPU per-app control (not yet run)

Hypothesis: application-local DXVK produces the same three image checksums as
native runtime011 on the same BC-250 GPU, ICD C0CE and client 84C328CD.
A mismatch remains a measured failure and triggers shader/resource analysis;
CPU/GPU cross-implementation equality is not substituted for this comparison.

Use the m14-runtime001 dispatcher, supervisor, job wrapper, durable copy,
file-routing, preflight171, scene gate and stage verifier, with literal identity
runtime001/Runtime001 changed to perapp001/Perapp001 in the staged scripts.
This directory supplies phase.ps1 instead of the native-route phase script.
The exact derived scripts and all artifacts must be hashed in stage-manifest.json.
Use a new one-shot directory C:\BC250\m14\perapp001 and an independent SYSTEM
supervisor with the existing 180-second task bound and restoration budget.
No staging or lab execution has happened yet.

The elevated Vulkan loader previously ignored override environment variables.
This control therefore temporarily routes the registered ICD file through the
existing durable, hash-checked file replacement mechanism. Preserve CF39 as
icd-baseline.dll, install exact C0CE from bc250radv.dll and restore CF39 before
postflight. CPU UMD registration and file, KMD and DWM remain unchanged.
Foreign active file contents must be preserved and reported, not overwritten.

Stage the frozen runtime011 client, debug-child and bounded-child helpers,
perapp-d3d11.dll and perapp-dxgi.dll, CPU and GPU selected runtime011 JSON references named
cpu-reference.json/native-reference.json, and the exact ICD. Capture builds an
app subdirectory from verified copies before install. CPU uses the root client;
GPU uses that subdirectory and must witness app-local DXGI/D3D11 plus exact ICD.
Both controls retain the original workload, save PAMs and compare strictly with
the corresponding recorded reference. Verify unchanged OS/driver generation,
DWM modules, registration and baseline hashes after restoration.

DXVK source bf14ecca differs from native engine source80352134 only in
src/ddi/engine_test.cpp and src/ddi/meson.build, not rendering implementation.
A fresh build receipt must establish the per-app artifact hashes. This control
uses a debugger and is not evidence for the performance bound or window Present.
