# E58: ACO moved LDS spill reloads above their stores, the cause of the rtwave64 failure of E57 (M876)

Unit A, Windows 11, 2026-10-12 06:10-06:51 UTC, KMD 0.7.216.36, release 0.7.216.100-tester.28 installed. All runs
use the CTS batch runner, route "direct", one `deqp-vk` process per run, `RADV_EXPERIMENTAL=sparse`, a 120 s bound
per run. This directory finds the cause of the E57 failure (M795, backlog item BD-074) and tests the fix.

| item | identity |
|---|---|
| CTS binary | `deqp-vk`, SHA-256 `E3DABB44...` (the same binary as E57) |
| case lists | `rt-k97-di1.txt`, the one case `dEQP-VK.ray_query.misc.dynamic_indexing`, and `rt-k97.txt`, 140 names |
| released ICD | `vulkan_radeon.dll` of tester.28, SHA-256 `FE3455FB903B758800FC6F121669235F02C276C106F61C3FF669E31CE2151B28` |
| patched ICD | SHA-256 `E738ECE157DA9BE2D41A1375429461F7EB5BEC171663559DBC2B8322E059F3C0`, 21967872 bytes |
| patch | `aco-lds-spill-sync.patch` (SHA-256 `93F4928A...`), commit `630b3766` on branch `amdgpu-wddm/bd074-aco-lds-spill-sync` of the Mesa fork, on top of `38ee64d4` |
| host | BC250-A |

The patched ICD is the tester.28 source plus the patch, and nothing else. The source of tester.28 is Mesa `b54b820e`
plus a 15-entry diff whose SHA-256 is `FEE925CA...`. Commit `38ee64d4` carries exactly that diff. A control build of
that state with the recorded recipe gave `FE3455FB...`, byte for byte the released file. So the only difference
between the two ICDs is the patch.

## Runs

| run | ICD | environment | result | `deqp-vk` time |
|---|---|---|---|---|
| `bd074acodef-t28` | released | `RADV_DEBUG=shaders,nocache` | dynamic_indexing Pass | 7.8 s |
| `bd074acow64-t28` | released | `RADV_DEBUG=shaders,nocache`, `RADV_PERFTEST=rtwave64` | dynamic_indexing Fail | 9.1 s |
| `bd074nosched-w64-t28` | released | `RADV_DEBUG=nocache`, `ACO_DEBUG=nosched`, `RADV_PERFTEST=rtwave64` | dynamic_indexing Pass | 9.1 s |
| `bd074fix-di1-w64` | patched | `RADV_DEBUG=shaders,nocache`, `RADV_PERFTEST=rtwave64` | dynamic_indexing Pass | 12.1 s |
| `bd074fix-k97-def` | patched | default | 137 Pass, 3 NotRun of 140 | 12.9 s |
| `bd074fix-k97-w64` | patched | `RADV_PERFTEST=rtwave64` | 137 Pass, 3 NotRun of 140 | 13.4 s |

The three NotRun names are the same three as in E57: they are not cases of this `deqp-vk` build. Tctl was 60.8 C to
64.5 C at the start of each run. The run scripts are in `method/`. The runner's summaries, configurations and QPA
logs are in `runs/`. The runner does not record `RADV_DEBUG` or `ACO_DEBUG`, so the environment column comes from
the scripts.

## What the ACO dumps show

The `RADV_DEBUG=shaders` runs print the NIR and the ACO program of the case. The NIR of the default run and of the
rtwave64 run differ only in the subgroup size (32 against 64). The ACO programs differ in one way of kind: the
48-thread workgroup fits one wave64 wave, so under rtwave64 ACO spills VGPRs to LDS with `ds_write_addtid_b32` and
`ds_read_addtid_b32`. In the default run the workgroup needs two wave32 waves, and every VGPR spill goes to scratch.

In the released ICD these LDS instructions carry no memory synchronization information. The ACO scheduler then
treats them as instructions that do not alias, and it can move a reload above the store of the same slot. The
excerpt `excerpt-tester28-rtwave64-after-ra-46620-46672.txt` shows the case: the reloads of LDS slots 384 and 576
(`%49730`, `%49731`) come before the stores of the ray direction x and y (`%12876`, `%12877`) into those slots, and
`image_bvh64_intersect_ray` then takes the reloaded values as its direction. LDS returns the old contents of the
slots, so the ray has a wrong direction and the query never reports a hit.

`scan_spill_order.py` finds, inside one block, a reload and the next store to the same slot where the stored value is
defined after the reload and the reload is first used after the store. A spiller that works as designed cannot make that order.

| dump | spill kind | pairs | strict reorderings |
|---|---|---|---|
| released ICD, default | scratch | 509 | 0 |
| released ICD, rtwave64 | scratch | 178 | 0 |
| released ICD, rtwave64 | LDS | 421 | 167 in 62 blocks |
| patched ICD, rtwave64 | LDS | 325 | 0 |

In the patched dump the LDS spill lines print `storage:vgpr_spill semantics:private`, the stores of slots 384 and 576
come before their reloads, and the reloads come before the BVH instruction
(`excerpt-fix-rtwave64-after-ra-46620-46680.txt`).

## Reading

- The cause is in the ACO compiler of Mesa (`src/amd/compiler/aco_spill.cpp`), not in the ray query lowering, the
  WDDM winsys or the kernel driver. With the scheduler off (`ACO_DEBUG=nosched`) the released ICD passes the case,
  and with the patch the scheduler on passes it.
- The patch gives the four LDS spill and reload emitters `memory_sync_info(storage_vgpr_spill, semantic_private)`,
  the same information the scratch spills already have. It does not change the 140-case list in either mode.
- The fault does not need rtwave64. ACO spills to LDS for any compute or ray tracing shader on GFX9 and later whose
  workgroup fits one wave and that has no stack pointer. RADV compiles compute shaders as wave64 on GFX10 by
  default, so a spilling compute shader with at most 64 threads per workgroup can meet the same miscompile in the
  default configuration. No such shader is identified yet.
- Upstream Mesa main at `3e32b0e1` (2026-10-12) has the same code.
