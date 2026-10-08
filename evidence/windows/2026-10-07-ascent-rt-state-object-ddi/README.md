# The Ascent: the state object description of a collection link (development PC, 2026-10-07)

Date: 2026-10-07. The development PC, Windows 11 Pro build 26200, D3D12Core.dll 10.0.26100.9278 from System32, the
WARP driver, and an NVIDIA GeForce RTX 4090 for the engine-ddi harness. Not unit A. No step of this record ran on
unit A. Every program ran without a window and stopped by itself.

## Question

Lab trial 465 (2026-10-07) started The Ascent (Unreal Engine 4.26) on the native D3D12 path of unit A. The game
stopped with a GPU crash report: `CreateStateObject` at D3D12RayTracing.cpp line 368 returned
`DXGI_ERROR_DEVICE_REMOVED`. The kernel driver log of the trial holds no fault, no timeout and no reset. What did
our driver refuse, and why did the refusal remove the device?

## Method

1. `trial465-stack.txt`: the game thread (0x1638) of the trial's `UE4Minidump.dmp`, read with the Python
   `minidump` package. Module versions, then the stack qwords around the failing call.
2. `tools/win/d3d12ddicap` (this commit's source, `d3d12ddicap.exe` SHA-256 A9307198, first line of the file)
   on WARP. It prints the DDI description that the runtime gives the driver for each state object.
   `ddicap-warp.txt` is the output of all four cases. `ddicap-warp-ue426-5.txt` is the output of a later build
   (SHA-256 C27C5B81, the source of the commit that adds this file) for the cases `ue426` and `ue426-5`. The
   `ue426-5` link has five imports and 8 subobjects, as the failing link of trial 465.
3. The engine-ddi harness, round trip 9 (`driver/umd/d3d12/engine-ddi/tests/test-raytracing.cpp`), with the new
   cases 7 and 7b. Three runs: the fixed `state-objects.cpp` with the pinned engine, the same with the lab's engine
   DLL 348117F1, and a control build with the `state-objects.cpp` of bc250-win 3eca5f98. `harness-raytracing.txt`
   holds the ray tracing lines of the three runs.

## Files

| File | Content |
|---|---|
| `trial465-stack.txt` | Modules and stack qwords of the trial 465 minidump |
| `ddicap-warp.txt` | The DDI descriptions on WARP, cases `ue426`, `ue426-hitnames`, `ue426-exportlist`, `client-collection` |
| `ddicap-warp-ue426-5.txt` | The DDI descriptions on WARP, cases `ue426` and `ue426-5` |
| `harness-raytracing.txt` | Harness round trip 9, fixed (pinned engine, lab engine) and control |
| `sha256.txt` | SHA-256 of every file above |

## Result

1. The minidump. Unit A runs D3D12Core.dll 10.0.22621.5415 from System32 (OS build 22631). At 0x2c74b7df90 the
   failing call has a `D3D12_STATE_OBJECT_DESC` of type 3 (RAYTRACING_PIPELINE) with 8 subobjects. At 0x2c74b7df50
   the HRESULT is 0x887A0005. At 0x2c74b7e020 a shader configuration of 24 and 8 bytes is on the stack. The
   subobject array is on the heap, which the minidump does not hold.
2. The runtime on WARP. A collection's summary points into the collection's own array. The summary of a pipeline
   that imports collections lists every export of the collections, and each association points into the array of
   the collection that made it (`S1#4 LOCAL_ROOT_SIGNATURE` and so on). The application's link had six subobjects:
   the shader configuration, the pipeline configuration, the global root signature and three imports. The driver
   gets five: the three imports, the pipeline configuration and the summary. The runtime removes the shader
   configuration and the global root signature, which no export of the link uses. The project's own client shape
   (`client-collection`) gets the same form. With an export list per import, the summary lists the imported
   shaders only (not the hit group's), with the same pointers.
3. Our driver before the fix. `count_associations` (`state-objects.cpp:278` at bc250-win 3eca5f98) finds no
   subobject of the create at those pointers and refuses with E_INVALIDARG. The control run shows the refusal for
   the measured form: `summary association outside the description (hr 80070057)`. `create_state_object` returns
   that code to the runtime unchanged. CreateStateObject is a creation function, and the runtime removes the device
   for every failure other than E_OUTOFMEMORY and D3DDDIERR_DEVICEREMOVED (BD-075). The application then sees
   0x887A0005, as in item 1.
4. Our driver after the fix. The harness passes 564 checks with the pinned engine and 565 with the lab engine.
   Case 7 (one collection, measured form) and case 7b (three collections in the inferred UE 4.26 shape, renamed
   exports, a local root signature in each) create, give identifiers, and write the 64 expected words through the
   link after the collections' DDI objects are destroyed. A summary association into no known array is still
   refused, now as E_OUTOFMEMORY with E_INVALIDARG on one refusal line.

## Limits

- WARP and the development PC's runtime 10.0.26100.9278 are measured. That unit A's runtime 10.0.22621.5415
  describes a collection link in the same form is an INFERENCE.
- The UE 4.26 shape of case 7b and of `ue426` is an INFERENCE from the 8 subobjects and the 24-byte payload. The
  engine source was not available for this record.
- No run on unit A. The lab check that closes the question is a rerun of The Ascent with the fixed shell.
