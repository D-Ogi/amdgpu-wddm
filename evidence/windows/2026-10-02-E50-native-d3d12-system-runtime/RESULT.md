# E50: Native D3D12 through the system runtime: rendering, Present, FL 12_1, ROV, conservative rasterization and DXR (M15.1-M15.5)

Dates: 2026-09-29 (trials 024-065), 2026-10-01 (237-240), 2026-10-02 (309). Unit A, Windows 11 Pro build 22631,
system `d3d12.dll`, `D3D12Core.dll` and `dxgi.dll` 10.0.22621.5415 (`taskmgr-probe-309.txt`).

This directory publishes the client-side records of the native D3D12 trials that `docs/m15-reconciliation.md`
cites for M15.1-M15.5. Trial numbers are those of the lab's native-caps series.

## Common setup

- Every client is an ordinary x64 process. It loads `d3d12.dll` and `dxgi.dll` from System32, selects the BC-250
  adapter (PCI 1002:13FE) and places no runtime or driver DLL of ours next to itself. Each client trace starts with
  `runtime=system32/d3d12.dll adapter=BC-250`.
- The runtime loads the adapter's registered DX12 user-mode driver: the fourth `UserModeDriverName` entry,
  `C:\BC250\m15\registration002\amdgpu_wddm_d3d12.dll` in every trial (`prestate.tsv`). That shell loads its engine
  `amdgpu_wddm_vkd3d.dll` (the vkd3d-proton fork) and the hosted RADV `amdgpu_wddm_radv.dll` from the same
  directory. A trial swaps candidate DLLs into that directory and puts the registered ones back afterwards. Trial
  309 swapped nothing (`config-309.json`: `"swap": []`, `"accepted_in_place": true`). The SHA256 of each loaded
  triplet is in `supervisor-NNN.json` under `artifacts`.
- Clients:
  - 024-065: `tools/win/d3d12queue` in its interactive mode, built with the variant flags named below.
  - 237-240: the conformance client `amdgpu_wddm_conformance.exe` (SHA256 prefix 2BB5CC0D). Its source is not in
    this repository yet.
  - 309: `tools/win/d3d12caps`.
- Supervisor: the lab's native-caps kit runs each trial as a bounded scheduled task (task 180 s, client deadline
  70 s). It records the registration and the desktop before the trial, swaps the DLLs and runs the client in the
  user's session through numbered command files (`create-device`, `create-queue`, `copy`, `status`, `exit`). It then
  restores and verifies the registered DLLs. The closure is in `supervisor-NNN.json`:
  - `functional-restored`: the client's checks passed, the process tree closed and the baseline was restored.
  - `diagnostic-restored`: the baseline was restored, but a check failed or the client left by its own deadline.
- Kernel driver (`prestate.tsv`): 0.7.171.1 for 024-065, 0.7.185.1 for 237-240, 0.7.193.1 for 309. The desktop was
  composed by the CPU desktop UMD for 024-065 and on the GPU route (router, hosted Zink, RADV) for 237-309.
- Files:
  - `client-NNN.txt`: the client's own trace, one JSON record per call with the elapsed milliseconds and the
    HRESULT. The conformance client also prints its `FEATURES` and `CONFORMANCE` lines, and writes
    `conformance-NNN.json`.
  - `shell-NNN.txt`: whole lines of the shell's stderr, selected by event name (`present-outputs`,
    `deallocate-callback`, `shell-memory-free`), by the `d3d12-caps` prefix or by `CreateStateObject:`.
  - `sources.tsv`: the workspace source of every file, with its SHA256.

The shells' full DDI traces, the kit's archives and the operator's screenshots stay in the workspace. The
screenshots show the lab desktop.

## 1. Copy, draw and scene at FL 11_0 (M778)

Engine ACEAB520 and ICD F43FD08C in all three trials. No sparse switch. Page heap was on for the client image.

| trial | shell (bc250-win) | client | work | result |
|---|---|---|---|---|
| 024 | 85E37537 (c809f517) | AE777A50 | 4096-byte UPLOAD to READBACK buffer copy | `Compare 4096 exact bytes` S_OK, exit 0; functional-restored, 60.6 s |
| 034 | E956DDDC (1fb52ffe) | B236035A, `-Draw` | one triangle into a 64x64 R32_UINT render target; the destination holds the complement before submission | `Compare 4096 exact words` S_OK, exit 0; functional-restored, 58.7 s |
| 036 | 6681B5A5 (ace3f699) | BF9A68AB, `-Scene` | buffer-to-texture copy; an SRV through a descriptor table in a shader-visible heap; D32_FLOAT clear to 1.0 with LESS test and depth write; a scissor per draw; R16 indices in reverse order; four indexed draws | `Compare 4096 exact words` S_OK, exit 0; functional-restored, 61.3 s |

- The device was created at FL 11_0 in all three trials, and the runtime reported a maximum of 11_1 (`b100`).
- The CPU oracle of 036 holds only if each of the listed steps works. Draw 4 passes only where draw 2 did not
  write depth.
- The clients of 034 and 036 pass on WARP.
- Not covered: sampling with a sampler (036 loads its texture by point), blending, MSAA, stencil.

## 2. Present (M779)

Shell 293344F5 (adapter073, bc250-win dff32c55), the default artifact with no experiment switch. Engine
D8BB19C3, ICD F43FD08C. Device at FL 11_0, page heap on, desktop composed by the CPU desktop UMD.

- Client 013 FBB67468 (053) and client 014 6C7C4604 (054, 056).
- Swap chain: `CreateSwapChainForHwnd` FLIP_DISCARD B8G8R8A8, two 256x256 buffers.
- Each frame is cleared to a colour and read back before its Present.

| trial | frames | resize | shell | closure |
|---|---|---|---|---|
| 053 | frame 1 `ffff8000` and frame 2 `ff008080`, each 65536 of 65536 texels equal; Present 1 and 2 S_OK | ResizeBuffers 128x128 S_OK | `present-outputs` stage 0 S_OK per Present. The four primaries were released through the runtime resource with flags 3 (ASSUME_NOT_IN_USE, SYNCHRONOUS_DESTROY), S_OK, `owner_expired` 0 | functional-restored, 70.9 s, exit 0 |
| 054 | as 053, then frame 3 `ffff00ff` at 128x128, 16384 of 16384 texels equal; Present 3 S_OK | as 053; both resized buffers fetched; `GetCopyableFootprints` row pitch 1024 at 256x256 and 512 at 128x128 | as 053, three `present-outputs` | functional-restored, 70.5 s, exit 0 |
| 056 | as 054, with `Three frames exact and presented with S_OK, the third after the resize` | as 054 | as 054 | diagnostic-restored, 92.4 s: the operator's screenshot loop delayed the exit command, and the client left by its own deadline (exit 3) |

- 055 and 056 add the operator's screenshots of the lab desktop during the frames.
  - 056 caught the window orange (shots 001-002, 3904 sampled pixels in the colour band), then teal (shot 003,
    4090), then magenta (shots 004-007, 3904). No shot shows two colours.
  - The window keeps its 256x256 client area. The magenta shot shows a Present after the resize, not the buffer's
    size; the size is in the 054 trace.
- Not covered: rendered content (the frames are clears), 10-bit and FP16 formats, a GPU-composed desktop. M771
  covers 10-bit under GPU composition.

## 3. FL 12_1 through the system runtime (M780)

| trial | triplet | switch | result |
|---|---|---|---|
| 028 | shell 81B38734, engine ACEAB520, ICD F43FD08C | the client sets `RADV_EXPERIMENTAL=sparse` in its own process | device at FL 11_0. The runtime reports tiled tier 3, binding tier 3, conservative rasterization tier 3 and `MaxSupportedFeatureLevel c100` (12_1). The copy is exact; exit 0; functional-restored, 59.8 s |
| 029 | as 028 | as 028, and the client asks for FL 12_1 | `D3D12CreateDevice FL12_1` S_OK, the same tiers, `Compare 4096 exact bytes`, exit 0; functional-restored, 59.7 s |
| 059 | shell E83EB275 (adapter077, bc250-win d5a6be69), engine D8BB19C3, ICD 51BC3953 | none: the trace reads `Effective RADV_EXPERIMENTAL absent` | `D3D12CreateDevice FL12_1` S_OK, tiled tier 3, `c100`. `CreateReservedResource` 4 tiles, `CreateHeap` 8 tiles, `UpdateTileMappings` 4 tiles to heap tiles 2-5, then a copy UPLOAD to RESERVED to READBACK: `Reserved buffer 262144 of 262144 bytes equal`. Exit 0; functional-restored, 64.9 s |
| 309 | the registered triplet, shell 5A1B7BAF (adapter119), engine D79FEC49, ICD F9DCB33B; nothing swapped | none | `caps-system-309.json`, below; functional-restored, 26.6 s |

- In 059 the sparse policy is the driver's. The shell's line reads
  `d3d12-caps instance-policy sparse=1 source=default status=c0000034` (`shell-059.txt`): the adapter key has no
  `AmdgpuWddmSparseBinding` value (not found), so the default applies, which is on. The registry off switch has not
  been run on the lab.
- 309: the `d3d12caps` witness through the system runtime with the triplet registered on 2026-10-02:
  - `D3D12CreateDevice` 12_1 S_OK, 12_2 `DXGI_ERROR_UNSUPPORTED`; `MaxSupportedFeatureLevel` 12_1 for every list
    that includes it.
  - OPTIONS: TiledResourcesTier 3, ROVsSupported true, ConservativeRasterizationTier 3, ResourceBindingTier 3,
    ResourceHeapTier 2, TypedUAVLoadAdditionalFormats true, PSSpecifiedStencilRefSupported true, 40 VA bits per
    resource. HighestShaderModel 6.6, root signature 1.1.
  - Not offered: OPTIONS5 RaytracingTier 0 (no switch, see section 5), mesh shader tier 0, sampler feedback tier 0,
    variable-rate shading tier 0, enhanced barriers false.
  - Modules: `d3d12.dll`, `D3D12Core.dll` and `dxgi.dll` from System32, our three DLLs from the registration
    directory (`other`), none from the executable's directory. `GetDeviceRemovedReason` S_OK.
  - `taskmgr-probe-309.txt` was taken after the kit's closure from an ordinary process with no variable.
    - D3D12: device creation S_OK at FL 11_0; `MaxSupportedFeatureLevel` 12_1; FL 12_2 refused (`887A0004`).
    - D3D11: the best feature level is 10_0. The adapter's D3D9-11 entries name the desktop router (the
      probe's registry listing), which sends processes outside its allowlist to the CPU desktop UMD.
- Not covered: a feature-level conformance run. The sparse coverage on the Vulkan side is M773 (8299 of 19078 CTS
  sparse cases NotSupported, 4715 of them device-group cases).

## 4. ROV and conservative rasterization (M781)

Conformance client 2BB5CC0D, offscreen, device at FL 12_1. Three subtests in one copy verb, each with an exact CPU
oracle and negative controls:

- **rov**: 43 overlapping triangles write ordered values through a rasterizer-ordered view. The controls: the
  reversed-order oracle differs in 4096 of 4096 pixels, and the same shader without ROV is observed to differ in
  4096 of 4096.
- **conservative**: standard, overestimated and inner-coverage passes against an analytic oracle (137, 235 and
  82 expected pixels). The standard image read against the conservative oracle differs in 98 pixels, and the
  reverse too.
- **dxr-indirect**: see section 5.

Shell 430CB1AE (adapter103) and engine 106D09E5 for 237-239. The trials differ in the ICD:

| trial | ICD | rov | conservative | closure |
|---|---|---|---|---|
| 237 | 85077E29 (registered then) | PASS 0/12288 | FAIL: 153 inner-coverage mismatches, first `got 0x0 want 0x8`. Partly covered pixels were not shaded with overestimation and inner coverage together | diagnostic-restored, 38.2 s |
| 238 | DECB5C8A (85077E29 + RADV overestimation and inner coverage programmed together, one extra sample) | PASS | FAIL: 153 mismatches, `got 0x9 want 0x8`. The partly covered pixels were shaded but read as fully covered | diagnostic-restored, 45.6 s |
| 239 | 2A13235D (238's change + MSAA enabled for that case) | PASS | **PASS** 0/12288: std, cons and inner 0/4096 each | diagnostic-restored, 44.4 s (the dxr subtest still failed) |
| 240 | 2A13235D, with shell 3B69F320 and engine 15E3E24E | PASS | PASS | functional-restored, 53.5 s, `CONFORMANCE overall PASS` |

- From 2026-10-01 08:58Z the lab registered 240's engine and ICD, with shell 6B12D522 (adapter106: 240's shell
  change plus FP16 composed primaries).
- The ICDs registered since then carry the same RADV change in their source line (Mesa fork commit 75020046), but
  this client has not been rerun on them.

## 5. DXR (M782)

The shell reports RaytracingTier 1.1 only when the `raytracing-tier` experiment is set for the process. The switch
is the environment variable `AMDGPU_WDDM_D3D12_EXPERIMENT` (the trial's `BC250_TRIAL_EXPERIMENT`), or an
application profile under `HKLM\SOFTWARE\amdgpu-wddm\D3D12\Applications` (E49). Without it the registered shell
reports tier 0 (309 above). The shell line is `d3d12-caps experiment raytracing-tier result=00000000`
(`shell-NNN.txt`). Every trial here set the switch, and every client trace reads
`Effective RADV_EXPERIMENTAL absent` and `Reported RaytracingTier 11` (or `1.1`).

| trial | shell | engine, ICD | client | result |
|---|---|---|---|---|
| 062 | 0DB479BA (adapter078, bc250-win d3d10ecc) | D8BB19C3, 51BC3953 | 017 E81E91C2: inline ray query in a cs_6_5 compute shader over one triangle | FL 12_1, SM 6.5 reported. The bottom and top levels were built, then `Ray query 64 of 64 words equal, 12 hits`. Exit 0; functional-restored, 50.5 s |
| 064 | F3819D09 (adapter080, bc250-win cbce5e07) | D8BB19C3, 51BC3953 | 019 C2819B77 (bc250-win 16d1fd01): state object lifecycle under page heap, no GPU work | two RAYTRACING_PIPELINE state objects (lib_6_3) created; 3 of 3 shader identifiers nonzero and different, twice; both released. Exit 0; functional-restored, 60.9 s |
| 065 | as 064 | as 064 | 018 34CE3554 (bc250-win 10a7215f): one library, one hit group, global root signature | `CreateStateObject` S_OK, three identifiers, `DispatchRays 8 8 1`, `Ray pipeline 64 of 64 words equal, 12 hits`. Exit 0; functional-restored, 53.7 s |
| 237-239 | 430CB1AE | 106D09E5, as section 4 | conformance | `dxr-indirect ERROR ... hr=887a0005`: that shell refused a DISPATCH_RAYS command signature, which removed the device |
| 240 | 3B69F320 (adapter105, bc250-win 332bef49) | 15E3E24E, 2A13235D | conformance | `dxr-indirect PASS mismatches=0 checked=694`. Five variants 0/128 each: direct, indirect max 1, count 1 of max 2, count 2 of max 2, max 2 without a count buffer. GPU-written arguments 0/54; 12 hits, 52 misses. The count-1 image differs from the count-2 oracle in 32/128 words |

- The shell lines of 064 and 065 describe the state object that the runtime sent, as this implementation reads it:
  type 3, 6 subobjects, 3 exports by name, 9 associated names, a DXIL library part of 584 DWORDs, pipeline config
  depth 1.
- Not covered: stack-size calls, local root arguments, collections and `AddToStateObject`, recursion beyond
  depth 1, a DXR conformance subset.
- The game with RT on through this path is in M775 (E47) and M777 (E49).

## Facts

M778, M779, M780, M781, M782.
