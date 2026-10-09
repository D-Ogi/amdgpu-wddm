# The Ascent: the ray tracing state objects of Unreal Engine 4.26 with additions (2026-10-07)

Date: 2026-10-07. The development PC, Windows 11 Pro build 26200, D3D12Core.dll 10.0.26100.9278 from System32, the
WARP driver, and an NVIDIA GeForce RTX 4090 for the engine-ddi harness. One file (`lab-ddicap-client.txt`) is from
unit A. Every program ran without a window and stopped by itself.

## Question

Fact M837 gives the cause of the stop of The Ascent in lab trial 465 and a fix. The UE 4.26 shape of that record was
an inference. The D3D12RHI source of Unreal Engine 4.26 is now available, read for facts only. Does the engine use a
state object form that the fix does not cover? Two engine features were not in the earlier cases:

- The state object configuration with `ALLOW_STATE_OBJECT_ADDITIONS` in each collection and link.
- `ID3D12Device7::AddToStateObject` onto a base pipeline. The engine stops on any failure of this call and of
  `CreateStateObject` (`VERIFYD3D12RESULT`).

## Engine facts (D3D12RHI of 4.26, read for facts only)

- `D3D12Adapter.cpp:742-748`: the engine sets `GRHISupportsRayTracingPSOAdditions` when the device reports ray
  tracing tier 1.1 and gives `ID3D12Device7`. The driver of this project reports tier 1.1.
- `D3D12RayTracing.cpp:302-304`: with that flag, each state object that the engine makes gets a state object
  configuration with `ALLOW_STATE_OBJECT_ADDITIONS`.
- A collection has these subobjects, in this order: the library with one renamed export, the hit group (hit groups
  only), the shader configuration (payload 24 bytes, attributes 8 bytes), an association of the shader
  configuration, the pipeline configuration (recursion depth 1), the state object configuration, the global root
  signature, the local root signature and its association. A link of three collections has 8 subobjects: the
  three collections, the shader configuration, its association, the pipeline configuration, the state object
  configuration and the global root signature.
- `D3D12RayTracing.cpp:1758-1765`: a pipeline gets a base pipeline from the pipeline cache only with the flag.
  `D3D12RayTracing.cpp:1938-1972`: with a base, the engine makes collections for the new shaders and calls
  `AddToStateObject` with the state object configuration and the new collections, in `VERIFYD3D12RESULT`.
- `D3D12RayTracing.cpp:359` holds the `VERIFYD3D12RESULT` of `CreateStateObject` in 4.26 (commit 1598cf21) and in
  4.26.1-release. The crash report of trial 465 gives line 368.

## Method

1. `tools/win/d3d12ddicap` (this commit's source, `d3d12ddicap.exe` SHA-256 A028221E, first line of the file) on
   WARP. The cases make the engine shapes above: `ue426` (no flags), `ue426-additions`, `ue426-basic` (the two
   start pipelines), and `ue426-add` (a base, two new collections and their addition). `ddicap-warp.txt` is the
   output of all cases.
2. The engine-ddi harness, round trip 9 (`driver/umd/d3d12/engine-ddi/tests/test-raytracing.cpp`). Case 7b is now
   the UE link with the state object configuration that allows additions, in the form of item 1. The new case 7c is
   the UE addition onto that link in the form of item 1, then a dispatch through the grown object. Three runs: the
   pinned engine, the pinned engine with deferred replay, and the lab's engine DLL 348117F1 (vkd3d-proton fork
   4e9a98e9). `harness-raytracing.txt` holds the summary and the UE lines of each run.
3. `lab-ddicap-client.txt`: the lab operator's run on unit A of the previous `d3d12ddicap.exe` build (cases `ue426`
   and `ue426-5` of fact M837) with `--hardware`. Three runs: the shell DLL installed at that time (189E9FA3), the
   shell with the fix of fact M837 (3ED15929), and the same with `--idle-seconds=120`.

## Files

| File | Content |
|---|---|
| `ddicap-warp.txt` | The DDI descriptions on WARP, cases `ue426`, `ue426-additions`, `ue426-basic`, `ue426-add`, `client-collection` |
| `harness-raytracing.txt` | Harness round trip 9: SHA-256 of the harness and both engines, the UE lines of three runs |
| `lab-ddicap-client.txt` | The API results of the earlier client on unit A, three shell states |
| `sha256.txt` | SHA-256 of every file above |

## Result

1. The runtime on WARP. The driver gets no association subobject. The summary of a collection associates five
   subobjects with its export, in this order: the shader configuration, the local root signature, the pipeline
   configuration, the state object configuration and the global root signature. The 8 subobjects of a UE link reach
   the driver as six: the three collections, the state object configuration (flags 0x4, or 0x0 for `ue426`), the
   pipeline configuration and the summary. The summary points into the arrays of the collections (`S16#3
   STATE_OBJECT_CONFIG` and so on). The runtime removes the shader configuration and the global root signature of
   the link.
2. The addition on WARP. The 3 subobjects of the application reach the driver as `AddToStateObject` with 5: the two
   new collections, the state object configuration, a pipeline configuration and the summary. The application gave
   no pipeline configuration. The summary lists the exports of the new collections only (the closest hit and any hit
   shaders, not the hit group), with pointers into their arrays. The grown object gives all five identifiers, also
   after the release of the base. The base gives its own three and no identifier of the new collections.
3. Our driver. engine-ddi accepts both forms with no change to `state-objects.cpp`. The harness passes 566 checks
   on the pinned engine (also with deferred replay) and 568 on the lab engine 348117F1. In case 7c, the description
   handed to the engine grows from the link's engine object and has the two imports with 0 exports and no
   association. A dispatch through a table of the base's ray generation shader and the new miss and hit group gives
   the 64 expected words after the DDI objects of the new collections are destroyed. The `RGS_00000001` identifier
   of the grown object is equal to that of the base. The restricted import path of the engine
   (`raytracing_pipeline.c:714-720`, E_NOTIMPL for an import with an export list) does not apply: the engine gives
   each import with 0 exports.
4. Unit A, earlier client. With the installed shell 189E9FA3, the link of `ue426` fails with 887a0005 and the device
   removed reason is 887a0020. The next case then fails at its first collection. With the shell 3ED15929 (the fix of
   fact M837), every create returns 00000000 and the device removed reason is 00000000, also after a 120-second
   idle time before the link.

## Limits

- The addition is not measured on unit A. Unit A's runtime 10.0.22621.5415 and its RADV pipeline library path for
  an addition (a library and a link of the parent, group handles that do not change) are not shown.
- The previous client on unit A had no state object configuration in its cases. The current client
  (`ue426-additions`, `ue426-basic`, `ue426-add`) has not run on unit A.
- The game's engine is not stock 4.26: its crash report gives line 368, and the 4.26 source has the call at
  line 359. That its ray tracing calls have the shapes above is an INFERENCE.
- The renderer source of the engine was not read. Which pipelines the game makes with ray tracing set off in its
  options is not known.
