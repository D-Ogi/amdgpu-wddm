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

With `--hardware`, the program does not load WARP. It runs the cases on the first hardware adapter through the
installed driver and prints the API results only. This is the lab client for The Ascent (see "Lab use").

## Build and run

```
$env:BC250_ROOT = '<workspace>'
powershell -NoProfile -File tools\win\d3d12ddicap\build.ps1 [-NoRun] [-Cases ue426,client-collection]
```

Options of `d3d12ddicap.exe`:

| Option | Effect |
|---|---|
| `--hardware` | Use the first hardware adapter, not WARP. No DDI lines. |
| `--hardware=<text>` | Use the first hardware adapter whose description contains `<text>`. |
| `--idle-seconds=<n>` | Wait `<n>` seconds after the collections and before the link of each `ue426` case. Then print the device removed reason. |

Each case ends with a line `== case <name>: hr <hr>, device removed reason <hr>`.

`build.ps1` compiles `ue426.hlsl` three times with the dxc of the SDK (`-D RGS`, `-D MS`, `-D HIT`, lib_6_3), then
compiles `ddicap.cpp` with the MSVC that vswhere finds. The output goes to `<workspace>\scratch\build\d3d12ddicap`.

## Cases

| Case | What the application creates |
|---|---|
| `ue426` | Three collections, then a link. Each collection has one library with renamed exports, both configurations, the global and a local root signature. The hit group collection also has its hit group. The link has the shader and pipeline configurations, the global root signature and three `EXISTING_COLLECTION` subobjects. |
| `ue426-5` | The same as `ue426` with five collections: a second miss shader (`MS_00000004`) and a second hit group (`HitGroup_00000005`). The link then has 8 subobjects. |
| `ue426-hitnames` | The same, but the hit group collection exposes the hit group only. |
| `ue426-exportlist` | The same as `ue426`, but each import has an export list. |
| `client-collection` | One collection of three libraries, one hit group, both configurations and both root signatures. Then a pipeline of the collection, the global root signature and the pipeline configuration. |

The `ue426` shape is an inference from lab trial 465 (The Ascent, Unreal Engine 4.26). The failing link in its
minidump has 8 subobjects and a payload of 24 bytes. The `ue426-5` link also has 8 subobjects. The engine source
was not available.

## Result

Fact M837 records the measurement and its evidence. In short: the summary of an importer points into the arrays of
the imported collections for every export of a collection. The runtime also removes from the importer every root
signature and shader configuration that no export of the importer uses.

## Lab use

The client reproduces the failing call of trial 465 without the game. One run takes less than 3 minutes.

1. Copy `d3d12ddicap.exe` to the lab. It needs no other file.
2. Run `d3d12ddicap.exe --hardware ue426 ue426-5` with the shell DLL of the trial 465 stack (adapter131,
   A3F8E2A8). Expected: the link fails with `887a0005` and the device removed reason is not zero. The shell log
   (`AMDGPU_WDDM_DDI_TRACE=2` or the debugger output) has the refusal line "summary association outside the
   description".
3. Install the shell DLL with the fix. Run the same command. Expected: `hr 00000000` for every create, `found` for
   every identifier, and device removed reason `00000000`. The debugger output has no refusal line.
4. Run `d3d12ddicap.exe --hardware --idle-seconds=120 ue426-5` with the fix. This is a short form of the 9-minute
   wait of trial 465. Expected: the same result as step 3.

If step 2 does not fail, the runtime of unit A gives a different description. Then run the program on WARP on unit A
(without `--hardware`) and compare its DDI lines with fact M837.
