# D3D12 state object DDI capture

`d3d12ddicap.exe` prints the DDI description that the D3D12 runtime gives a user-mode driver for each ray tracing
state object. It runs on the development PC with the WARP driver. It does not use a driver of this project and it
does not touch unit A.

The program loads `d3d10warp.dll` before the runtime loads it. It points the `OpenAdapter12` export of WARP at a
wrapper. The wrapper replaces `pfnCreateStateObject` and `pfnAddToStateObject` in the device table that WARP fills.
Each replacement prints the description (`D3D12DDIARG_CREATE_STATE_OBJECT_0054`, d3d12umddi.h 10.0.26100) and then
calls the slot of WARP. A subobject pointer in a summary is printed as `#i` when it points into the array of the same
create, and as `S<n>#i` when it points into the array of an earlier state object `S<n>`.

The program has no window and no resident part. It stops when the cases are done.

## Build and run

```
$env:BC250_ROOT = '<workspace>'
powershell -NoProfile -File tools\win\d3d12ddicap\build.ps1 [-NoRun] [-Cases ue426,client-collection]
```

`build.ps1` compiles `ue426.hlsl` three times with the dxc of the SDK (`-D RGS`, `-D MS`, `-D HIT`, lib_6_3), then
compiles `ddicap.cpp` with the MSVC that vswhere finds. The output goes to `<workspace>\scratch\build\d3d12ddicap`.

## Cases

| Case | What the application creates |
|---|---|
| `ue426` | Three collections, then a link. Each collection has one library with renamed exports, both configurations, the global and a local root signature. The hit group collection also has its hit group. The link has the shader and pipeline configurations, the global root signature and three `EXISTING_COLLECTION` subobjects. |
| `ue426-hitnames` | The same, but the hit group collection exposes the hit group only. |
| `ue426-exportlist` | The same as `ue426`, but each import has an export list. |
| `client-collection` | One collection of three libraries, one hit group, both configurations and both root signatures. Then a pipeline of the collection, the global root signature and the pipeline configuration. |

The `ue426` shape is an inference from lab trial 465 (The Ascent, Unreal Engine 4.26). The failing link in its
minidump has 8 subobjects and a payload of 24 bytes. The engine source was not available.

## Result

Fact M837 records the measurement and its evidence. In short: the summary of an importer points into the arrays of
the imported collections for every export of a collection. The runtime also removes from the importer every root
signature and shader configuration that no export of the importer uses.
