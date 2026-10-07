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
powershell -NoProfile -File tools\win\d3d12ddicap\build.ps1 [-NoRun] [-Cases ue426,ue426-add]
```

Options of `d3d12ddicap.exe`:

| Option | Effect |
|---|---|
| `--hardware` | Use the first hardware adapter, not WARP. No DDI lines. |
| `--hardware=<text>` | Use the first hardware adapter whose description contains `<text>`. |
| `--idle-seconds=<n>` | Wait `<n>` seconds before the first link of the `ue426`, `ue426-additions`, `ue426-basic` and `ue426-ahs-link` cases, and before the addition of the `ue426-add` cases. Then print the device removed reason. |

Each case ends with a line `== case <name>: hr <hr>, device removed reason <hr>`.

`build.ps1` compiles `ue426.hlsl` three times with the dxc of the SDK (`-D RGS`, `-D MS`, `-D HIT`, lib_6_3), then
compiles `ddicap.cpp` with the MSVC that vswhere finds. The output goes to `<workspace>\scratch\build\d3d12ddicap`.

## Cases

The `ue426` cases make the state objects in the sequence and the subobject order of the D3D12RHI of Unreal Engine
4.26.1-release (`D3D12RayTracing.cpp`). The program has no code of the engine. It uses facts from the engine source
only:

- Each shader gets its own collection. The subobjects are, in this order: the library with one export renamed to
  `<prefix>_<16 hex digits>`, the hit group (hit group collections only), the shader configuration (payload 24 bytes,
  attributes 8 bytes), an association of the shader configuration with the export, the pipeline configuration
  (recursion depth 1), the state object configuration, the global root signature, the local root signature and its
  association. A ray generation collection uses the empty local root signature.
- A link has the existing collections, the shader configuration, an association of the shader configuration with
  no export, the pipeline configuration, the state object configuration and the global root signature. Three
  collections give 8 subobjects. The failing link of trial 465 also has 8 subobjects and a payload of 24 bytes.
- An addition (`ID3D12Device7::AddToStateObject`) has the state object configuration and the new collections.
- The engine sets `ALLOW_STATE_OBJECT_ADDITIONS` in the state object configuration of each collection and link when
  the device reports ray tracing tier 1.1 and has `ID3D12Device7`. The driver of this project reports tier 1.1.

| Case | What the application creates |
|---|---|
| `ue426` | The occlusion pipeline of the engine start, with no state object flags. That is a ray generation, the default miss and a closest hit collection, then their link. |
| `ue426-additions` | The same as `ue426`, with `ALLOW_STATE_OBJECT_ADDITIONS` on the collections and the link. This is the start of the engine on a tier 1.1 device. |
| `ue426-basic` | The two pipelines of the engine start with additions: the occlusion pipeline, then the intersection pipeline. The second link uses the miss collection of the first again. |
| `ue426-add` | The occlusion pipeline with additions as the base. Then two new collections (a hit group with an any hit shader and a miss shader) and their addition to the base. The program reads the identifiers from the grown object, from the base, and from the grown object after the release of the base. |
| `ue426-add-chs` | The same as `ue426-add`, but the new hit group has a closest hit shader only. |
| `ue426-add-miss` | The same as `ue426-add`, but the addition has the new miss shader only. |
| `ue426-ahs-link` | No addition. One link of the ray generation collection, the new miss collection and the hit group collection with an any hit shader. |
| `client-collection` | One collection of three libraries, one hit group, both configurations and both root signatures. Then a pipeline of the collection, the global root signature and the pipeline configuration. |

The engine makes the start pipelines in `InitRayTracing`. It makes an addition later, in the game, when the pipeline
cache finds a base pipeline with the same ray generation shaders.

## Result

Fact M837 records the first measurement. The summary of an importer points into the arrays of the imported
collections for every export of a collection. The runtime also removes from the importer every root signature and
shader configuration that no export of the importer uses.

Fact M838 records the measurement of the engine shapes. The runtime removes each association subobject. The summary
of a collection associates the shader configuration, the local root signature, the pipeline configuration, the state
object configuration and the global root signature with the export. A link keeps its state object configuration and
its pipeline configuration. An addition gets a pipeline configuration from the runtime, also when the application
gives none.

## Lab use

The client makes the ray tracing calls of the engine start and of a later addition without the game. One run takes
less than 3 minutes.

1. Copy `d3d12ddicap.exe` to the lab. It needs no other file.
2. Run `d3d12ddicap.exe --hardware ue426 ue426-additions ue426-basic ue426-add` with the installed shell DLL.
   Expected: `hr 00000000` for every create and every addition, `found` for every identifier that the case expects,
   `null` for the identifiers of the new collections in the base, and device removed reason `00000000`.
3. Run `d3d12ddicap.exe --hardware --idle-seconds=120 ue426-add`. This is a short form of the long wait of trial 465
   before its link. Expected: the same result as step 2.
4. For a control, run step 2 with the shell DLL of the trial 465 stack (adapter131, A3F8E2A8). Expected: the first
   link fails with `887a0005` and the device removed reason is not zero. The shell log (`AMDGPU_WDDM_DDI_TRACE=2` or
   the debugger output) has the refusal line "summary association outside the description".

To get the real code of a refusal, set these variables before the run. An SSH session runs in session 0, and no
debugger reads its OutputDebugString lines, so the lines must go to a file:

| Variable | Effect |
|---|---|
| `AMDGPU_WDDM_LOG=file:<path>` | The shell, the engine and the ICD append their lines to `<path>`. The shell writes one line per state object. A refused one has `hr <real> reported as 8007000e`. |
| `VKD3D_DEBUG=warn` | The engine also writes its `err`, `fixme` and `warn` lines. |
| `VKD3D_SHADER_CACHE_PATH=<directory>` and `AMDGPU_WDDM_VKD3D_PSO_LOG=1` | The engine writes the `VkResult` of each pipeline that it makes (`rt-lib`, `rt-selflink`) to a `.pso-log.txt` file in `<directory>`. |

If a result is different, run the program on WARP on unit A (without `--hardware`). Compare its DDI lines with facts
M837 and M838.
