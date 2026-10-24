# D3D12 mesh and amplification shaders on gfx1013: the r3-sync lab acceptance (2026-10-23 and 2026-10-24)

Unit A, Windows 11 22631, with the tester.32 package installed. Each stage replaced one file, the x64 D3D12 ICD
`d3d12\amdgpu_wddm_radv_d3d12.dll` in the DriverStore folder of the package (`-RenameLoaded`). It restored the
tester.32 file `3BF4F341` at the end of the stage. The D3D12 shell, the engine, the other ICDs and the KMD did not
change. The logs in `lab\` are the complete output of each stage.

## The candidate

| Item | Value |
|---|---|
| Mesa source | mesa-amdgpu-wddm `bd7e79973f84c0976aaa2efde1450054a3b9b846`, the head of `amdgpu-wddm/upstream-2026-10-23` (upstream main `a0ef4c88` merged in `53836c02`, then the nine commits of the gfx1013 mesh series) |
| ICD, release flavour | SHA256 `91A35561A5AA91565C93E4E0BC7D7AF092978016C21B342B0AB5E03FB0E58BB8`, 18,192,384 bytes |
| ICD, lab flavour (stage ab only) | SHA256 `1F63352166238DC790F645A884FAF2C515FA974051A3F55B5D87C340F1D72563`, 18,236,416 bytes. It reads `RADV_BC250_TASK_PFP_SYNC` |
| ICD recipe | `tools/build/build-mesa.ps1 -Config radv-mt -Arch x64 -Compiler clang-cl -Variant release,isa` (LLVM 23.1.4, x86-64-v3) |
| Witness | `amdgpu_wddm_mesh.exe` from `tools/win/conformance-clients` (`build-mesh.ps1`), SHA256 `8A5F99308644AD280A8987035C027F9FF938D861566FDB1E1403CCB38B644895` |

The nine commits: NIR intrinsics, the GFX10.1 NGG mesh lowering, the ACO legacy NGG mesh ABI, a winsys hook that merges
a child stream's buffer list, the RADV mesh and task port, no GDS access where the winsys programs no GDS partition,
MESH and TASK together in the DGC stage mask, the hybrid task route on by default, and the PFP sync between a hybrid
task producer and the CP read of its output.

## Why the PFP sync

The hybrid task route lets a compute producer write one mesh record (x, y, z) per task group. A
`DISPATCH_MESH_INDIRECT_MULTI` then draws the records, and the PFP fetches them as indirect arguments. Our Mesa base
holds the PFP after a barrier only when the barrier has `AC_BARRIER_PFP_SYNC_ME`. Without that flag on the seven
producer-to-CP barriers, the PFP can read the records before the producer writes them. The draw then starts no mesh
group. Stage ab shows this on the hardware.

## Results

| Stage | Time (UTC) | Log | Result |
|---|---|---|---|
| 1 | 2026-10-23 23:31-23:32 | `lab/slot-stage1-20261023T233118Z.log` | `MeshShaderTier 10` with the inbox and the Agility runtime, DGC stage mask `0x00003fff` with TASK and MESH. Probe `p-as`: `as_threads=8 ms_groups=8`, masks `0xff`, `ms_prims=8`, `ps_invocations=520`, PASS. Witness `w-ms`, `w-as`, `w-all`, `w-single`: `CONFORMANCE overall PASS`, `mismatches=0 checked=4096`. Task witness: `record_pfp=00000008,00000001,00000001 record_me=00000008,00000001,00000001 payload=00000000,00000001,00000002 pfp_sync=1`. eb client: `RESULT failed 0`. No graphics event in the System log |
| ab | 2026-10-23 23:32 | `lab/slot-ab-20261023T233229Z.log` | Lab flavour. AB-on: as stage 1. AB-off (`RADV_BC250_TASK_PFP_SYNC=0`): `p-as` FAIL with `ms_groups=0 ps_invocations=0`, `record_pfp=00000000,00000000,00000000 record_me=00000008,00000001,00000001 pfp_sync=0`. The CP read zero records while the producer had written them |
| 2 | 2026-10-23 23:33-23:34 | `lab/slot-stage2-20261023T233310Z.log` | MeshletRender, MeshletInstancing, MeshletCull and DynamicLOD (Agility samples): alive and responding at the end, 0 debug-layer errors. DynamicLOD shows the model |
| 4 | 2026-10-24 00:33-00:45 | `lab/slot-swap-20261024T003332Z.log`, `lab/slot-restore-20261024T004520Z.log` | Rise of the Tomb Raider, D3D12, built-in benchmark to its result screen: Mountain Peak 85.92, Syria 62.13, Geothermal Valley 23.74, overall 54.47 FPS. This is the first run after the ICD change. The picture is correct in the half-scale shots, with no TDR and no graphics event in the System log. The highest Tctl was 87.3 C |

Stage 4 notes. The benchmark numbers come from the result screen of the game session. Before this run, the first
two attempts stopped at the game start (the first swap is `lab/slot-swap-20261024T001023Z.log` to
`lab/slot-restore-20261024T001522Z.log`). A control session on `3BF4F341` stopped in the same way, so the stop does
not come from this ICD. It is defect BD-234 (the lab overlay).

Stage 3 (the hybrid task route off) was not run on this build. `lab/r3-slot-stage3-20261023T223708Z.log` is stage 3
of the same nine commits on the earlier base `66986af4` (ICD `B852C248`): with `RADV_BC250_HYBRID_TASK=0` the device
reports `MeshShaderTier 0`, and the witness skips with exit 4.

## Rebuilds

`rebuild/icd-pe-compare.txt`: a build of `amdgpu-wddm/upstream-2026-10-23` at `bd7e7997` from another checkout and
build directory has the same bytes as the lab-tested ICDs of both flavours, apart from the link time stamps and the
PDB identity. The release flavour hashes to `91A35561` and the lab flavour to `1F633521` when those fields are copied
in. The PDB records the absolute source directory, and `/Brepro` derives those fields from it. A tester build with
`-Version` also changes the version resource.

`rebuild/witness-pe-compare.txt`: the `tools/win/conformance-clients` tree of this commit is the tree (`db41461d`)
from which `8A5F9930` was built. A rebuild in the same checkout path has the same bytes, apart from the per-build
fields. In another checkout path, MSVC lays out the same code in another order. The build is deterministic in one
path.
