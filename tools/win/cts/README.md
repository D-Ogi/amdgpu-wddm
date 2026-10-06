# Vulkan CTS harness (deqp-vk)

The conformance harness for the Vulkan features that our D3D12 and D3D11 work stands on. It gives three
milestone criteria their numbers:

- M15.2, tiled resources, which sit on RADV sparse binding and residency. The sweep list holds 19078
  `dEQP-VK.sparse_resources.*` cases, cut into 147 bounded batches.
- M15.5 and M14.3, ray tracing and the FL 12_1 feature set. The ray-tracing lists hold 140 cases.
- Both, through the fault correlator, which reads the kernel driver's log ring after a batch.

One batch is one lab trial of at most 170 s. The runner stops at the bound and the next call continues with
the cases that remain, so a 19078-case sweep fits the three-minute limit for each trial.

PROVENANCE: VK-GL-CTS (https://github.com/KhronosGroup/VK-GL-CTS), Apache-2.0.

## The pinned CTS revision

`deqp-vk` comes from one pinned revision. Every number in this directory belongs to it.

| item | value |
|---|---|
| tag | `vulkan-cts-1.4.6.2`, the newest 1.4.x tag on 2026-10-01 |
| tag object | `42c723aa10d2652590f02741827aef43b0421d23` |
| commit | `f6a29701220f34dd1407513bfe80d74ca7b392ce`, 2026-08-06 |
| sweep list in that tree | `external/vulkancts/mustpass/main/vk-default/sparse-resources.txt`, SHA256 `FE08C706...` |
| binary built on | 2026-10-01, MSVC 14.44.35207, Release |
| `deqp-vk.exe` | 57923072 bytes, SHA256 `E3DABB44EB8D252083C33B80C3E18897301EB4F7E788F95F6070BCF0AC473183` |
| `cts-direct.dll` | SHA256 `A0CC25B366D1172C49829740A00DD512B60E8493E5DA76E795F34B1AB82DB9BE` |

The external sources of that tree stand at these commits. `deqp-build/fetch-deps.sh` prints them again after
each fetch, so a later build can show the same table.

| dependency | commit |
|---|---|
| spirv-tools | `2d14d2e76aa7de72404b17078eda15c20a6a0389` |
| glslang | `715c8500e7cd67f2eba9e60e98852a1ed49d2f15` |
| spirv-headers | `6dd7ba990830f7c15ac1345ff3b43ef6ffdad216` |
| vulkan-docs | `4abe0260bbd8e59930786eed645481808a3fe6f1` |
| amber | `53a4c8934bf7335d27c694e9fdac9ae1b180c0d2` |
| jsoncpp | `9059f5cad030ba11d37818847443a53918c327b1` |
| vulkan-video-samples | `aaf915a0e08d679d1e8b835c30e6a346ab0c7ad2`, v0.3.9 |
| video_generator | `426300e12a5cc5d4676807039a1be237a2b68187` |

The build turns the video tests off, and it takes zlib and libpng from `external/`, not from an msys64
install. `deqp-build/build-cts.cmd` holds the complete option set.

## What is here, and what stays outside

This directory holds the source of record: the runner, the case lists, the host tools, the build recipe and
the offline test. The large and the generated parts stay in the workspace scratch directory, by default
`<BC250_ROOT>/scratch/cts`:

- `deqp-vk.exe` (58 MB) and its 18 MB `vulkan/` data directory, plus the CTS source and build trees.
- `cts-direct.dll`, built from `deqp-build/cts-direct.c`.
- The lab package, which is the binary, the data, the two lab scripts and `batches/` in one directory.
- Every run result, and the reference run of the development PC.

`BC250_ROOT` is the workspace root, by default the parent directory of this repository. No script in this
directory writes inside the repository.

## Layout

| path | what it is |
|---|---|
| `run-batch.ps1` | the lab runner. One batch, one bounded call, resumable. Windows PowerShell 5.1 |
| `pack-results.ps1` | lab side. Packs one run directory into a tar for `target.py pull` |
| `sweep.py` | drives `run-batch.ps1` over a range of batch indices, one call at a time |
| `summarize.py` | merges the per-batch results and compares them with a reference run |
| `plan_batches.py` | cuts a case list into the bounded batches and writes `batches/manifest.json` |
| `make_caselist.py` | builds `lists/` from the CTS tree and from the binary's own case list |
| `make_smoke.py` | picks the short smoke list, one fast case per group |
| `qpa.py` | a small reader for the `.qpa` logs that deqp writes |
| `lab_history.py` | collects per-case durations of earlier unit A runs, to scale the batch plan |
| `batches/` | 147 sparse batches, `manifest.json`, the smoke list and the ray-tracing lists |
| `lists/` | the sweep list, the inventory of counts and hashes, the 2026-09-25 lab durations |
| `faults/` | the fault correlator: the kernel driver's log ring read beside a batch |
| `analysis/` | small read-only reports over a run directory on the lab |
| `deqp-build/` | the recipe that produced the pinned binary, and the `cts-direct.c` forwarder |
| `test/` | the offline checks of the runner, and the reference run on the development PC |

## Case lists

`lists/sparse-resources.txt` is the sweep list of 19078 cases in mustpass order. `batches/sparse-0001.txt` to
`sparse-0147.txt` hold the same cases, cut into contiguous slices. The sweep list stays whole because
`summarize.py` needs it as the denominator, and the runner needs each slice as a separate file whose SHA256 the
manifest pins.

`lists/inventory.json` records the counts and the hashes. The binary and the mustpass list contain the same
set, with 0 binary-only and 0 mustpass-only cases, so the 19078 figure in `docs/m15-reconciliation.md` needs no
explanation. The groups of the sweep list:

| group | cases |
|---|---|
| image_sparse_residency | 4275 |
| shader_intrinsics | 3420 |
| image_sparse_binding | 2619 |
| device_group_image_sparse_binding | 2556 |
| image_block_shapes | 1498 |
| image_sparse_memory_aliasing | 848 |
| device_group_image_sparse_memory_aliasing | 848 |
| mipmap_sparse_residency | 660 |
| device_group_image_sparse_residency | 660 |
| device_group_mipmap_sparse_residency | 660 |
| image_rebind | 480 |
| aligned_mip_size | 236 |
| buffer | 177 |
| multisampled_image_sparse_binding | 66 |
| multisampled_image_sparse_residency | 44 |
| queue_bind | 18 |
| transfer_queue | 13 |

The 4691 `device_group` cases answer NotSupported on any single-GPU machine. They are not capability gaps.

`batches/smoke.txt` holds 23 cases, the fastest passing case of each group. It takes 2.3 s on the development
PC. Run it first against each new ICD. On unit A (2026-10-05, from this directory) it gives 17 Pass and
6 NotSupported, with the release ICD 72E1D192 and with the BD-068 fix ICD 45712A13 alike.

### The ray-tracing lists

| list | cases | what |
|---|---|---|
| `batches/rt-k97.txt` | 140 | the ray-tracing subset: 61 `dEQP-VK.ray_query.*` and 79 `dEQP-VK.ray_tracing_pipeline.*` cases |
| `batches/rt-r4x.txt` | 79 | the `ray_tracing_pipeline` part of the same list, as one shorter call |
| `batches/rt-k97-di.txt` | 2 | `ray_query.misc.dynamic_indexing` and `dynamic_indexing_use_first` |
| `batches/rt-k97-di1.txt` | 1 | `ray_query.misc.dynamic_indexing` alone, the case that the wave64 run fails |

Each of these is a `-CaseList` argument, not an `-Index`. The runner pins `RADV_PERFTEST` per RunId, so
`-Perftest rtwave64` and the default empty value need two different RunId values.

## The batch plan

`plan_batches.py` wrote the 147 batches with `--budget-s 100 --max-cases 1000 --min-scale 10`. Each batch holds
37 to 859 cases, with a median of 108. The per-case estimate comes from the measured duration on the
development PC, in this order: the case's own duration where it passed, its own duration where it answers
NotSupported for a reason that also holds on unit A, then the median of its group.

The lab estimate is 10 times the host duration, plus 10 s of process start and 0.02 s for each case. The scale
is `max(10, p90)` of the measured lab/host ratio over 170 common cases from
`lists/lab-history-2026-09-25.tsv`, whose median is 3.0 and whose p90 is 7.3. Every batch is estimated at 84.6
to 100 s against the 170 s bound, which sums to 243.5 min. The plan is deliberately pessimistic. After the
first 5 to 10 lab batches, the real `elapsed_s` values exist. Plan again from that ratio with the same
tool and sweep with a new RunId.

## Running a sweep on the lab

The lab needs the package at `C:\BC250\cts`, which is one push of about 74 MB. The runner takes `-Root` when it
sits elsewhere. `target.py ps` copies one script to `C:\BC250\tmp` and runs it there, so each lab script in this
directory stands alone and reads nothing beside itself.

```
python tools/win/target.py push <scratch>/cts/package/bin <scratch>/cts/package/direct ^
    tools/win/cts/batches tools/win/cts/run-batch.ps1 tools/win/cts/pack-results.ps1 --to C:\BC250\cts

python tools/win/target.py ps tools/win/cts/run-batch.ps1 -CaseList C:\BC250\cts\batches\smoke.txt -RunId icd85077e29
python tools/win/target.py ps tools/win/cts/run-batch.ps1 -Index 1 -RunId icd85077e29
python tools/win/cts/sweep.py --run-id icd85077e29 --first 1 --last 147 --max-calls 20
```

What the runner does, with the complete description in the script header:

- **The ICD route.** `-Route direct` copies `-IcdPath` next to a copy of `cts-direct.dll` and points
  `--deqp-vk-library-path` at it. The Vulkan loader stays out of the way, because it ignores
  `VK_DRIVER_FILES` and `VK_ICD_FILENAMES` in an elevated SSH session. `-Route registered` takes the system
  loader and the registered ICD instead. The default `-IcdPath` is the RADV of the registered D3D12 triplet.
  Take the current path from the workspace `STATE.md` when it moves.
- **The witness.** The runner checks the exact ICD module path in the process, and the `deviceName` in the
  `.qpa` against `-ExpectDevice` (default `BC-250`). A mismatch stops the sweep.
- **The pins.** Each RunId pins the route, the ICD SHA256, `RADV_EXPERIMENTAL`, `RADV_PERFTEST`, the deqp-vk
  SHA256 and the list SHA256 in `config.json`. The runner refuses a change, so name the RunId after the ICD.
- **Sparse binding.** RADV gives a stand-alone Vulkan process sparse binding only with
  `RADV_EXPERIMENTAL=sparse`, which is the `-Experimental` default. The D3D12 host sends the same policy
  through the instance chain instead.
- **The bound and the resume.** The runner kills the process tree at `-BoundSeconds` (20 to 175, default 170).
  Finished cases accumulate in `cases.tsv` and the next call runs only the rest. A crashed case keeps the
  status `Crash` with its exit code. Cases that deqp never started become `NotRun` only after a clean
  `#endSession`.
- **Safety.** Tctl comes from one `bc250rd_cli` spawn every 20 s, and the run stops at 87 C. The runner also
  reads the overlay STOP flag every 20 s. A second runner meets `runner.lock` and stops.

Each batch writes `results/<RunId>/batch-NNNN/`: the per-attempt `.qpa`, `.caselist.txt`, `.stdout.txt`,
`.stderr.txt` and `.json`, then `cases.tsv`, `summary.json` and `summary.txt`.

| exit | meaning | what to do |
|---|---|---|
| 0 | complete, only Pass and NotSupported | next batch |
| 1 | complete, other results as well | next batch. The names are in the summary |
| 2 | incomplete | call again with the same arguments |
| 3 | stop the sweep | look at the GPU and the logs first. The same call then continues |

Exit 3 covers a lost device, a case that timed out, the thermal stop, the owner STOP flag, a witness mismatch,
a batch where no case ran, and a setup error.

## Collecting and comparing the results

```
python tools/win/target.py ps tools/win/cts/pack-results.ps1 -RunId icd85077e29
python tools/win/target.py pull C:\BC250\cts\results-icd85077e29.tar <scratch>/cts/results-icd85077e29.tar
tar -xf <scratch>/cts/results-icd85077e29.tar -C <scratch>/cts/lab-results
python tools/win/cts/summarize.py <scratch>/cts/lab-results/icd85077e29 ^
    --cases tools/win/cts/lists/sparse-resources.txt --reference <scratch>/cts/host-ref/results/<gpu>
```

`summarize.py` writes `merged/merged.tsv` with one row for each case: the status and duration here, the status
and duration in the reference, and the details. It also writes `summary.json` and `summary.md` with the counts,
the status matrix against the reference and three difference lists. It exits 0 only when all 19078 cases have a
result and each is Pass or NotSupported.

A reference run is a run of the same lists through the same runner on the development PC's own GPU, with
`-Route registered`. It shows which cases a mature sparse implementation passes, and how long each one takes.
It is not the expected RADV status for each case. The 2026-10-01 reference on an RTX 4090 gave 11710 Pass and
7368 NotSupported in 19.6 min, with no failure and no warning. The partial unit A run of 2026-09-25 answered
NotSupported for 1025 cases that this reference passes, and that list is where a capability gap shows.

`analysis/` holds the small read-only reports for a run that is still on the lab: the counts of one run or one
batch, the groups and reasons behind its NotSupported cases, the durations and the full record of one case, and
one TSV export of a run for evidence.

## The fault correlator

A clean CTS result does not prove that the kernel driver saw no fault. The log ring holds about the last 1024
lines, which is seconds of activity, so the read must follow the runner at once. `faults/` does that:

| script | what it reads |
|---|---|
| `k97-fault-check.ps1` | runs one case list through the runner, then reads the ring and counts the fault lines |
| `kmdlog-faults.ps1` | the ring lines that name a fault, a timeout, a reset or a hang, plus the ring tail |
| `kmdlog-summary.ps1` | the last complete `wddm summary` block, which holds the cumulative counters |
| `kmd-objects.ps1` | one line of object and allocation counters, to show a leak over a batch |

`k97-fault-check.ps1` also prints how many gfx job lines of the deqp-vk process the ring still holds. A count
of 0 means the ring rotated past that process, and the check then says nothing.

The CLI ships with the kernel-driver kit, and that directory changes with every kit. Each script takes `-Cli`,
reads `BC250_KMD_CLI`, or takes the newest `bc250kmd_cli.exe` under `C:\BC250`. The workspace `STATE.md`
names the deployed one. Each script repeats the resolution on purpose, because `target.py ps` copies one file
to the target and a shared helper would not be there.

## Offline checks

```
bash tools/win/cts/test/run-tests.sh                    10 checks, no GPU and no lab
powershell -File tools/win/cts/test/parse-check.ps1 -Path <the .ps1 files>
```

`test/run-tests.sh` builds `test/fake-deqp.c` into a stand-in for `deqp-vk.exe` and drives the runner through
six behaviours: the normal run, a case the binary does not hold, a crash, a watchdog Timeout, a hang past the
bound, and a lost device. It checks the exit code and `cases.tsv` of each, and of the resume call after each.
The result on 2026-10-01 was 10 of 10. The runner also ran 1500 cases under a 20 s bound in 6 calls on the
development PC, with no case lost and none run twice.

`test/parse-check.ps1` checks that a script parses under Windows PowerShell 5.1, which is the only shell the
lab offers.

## Caveats

- `lists/sparse-resources.txt` is the M15.2 list. The 38315-case name-based extension of it stays in scratch,
  unreviewed, and no criterion uses it.
- The direct route has no Vulkan loader, so no loader layer and no loader-side check applies. The CTS is the
  ICD's loader.
- The machine-wide ICD knob file `C:\BC250\tmp\amdgpu_wddm_radv.cfg` changes ICD behaviour. The runner records
  its content for each attempt, but it does not pin it. Keep it as the D3D12 trials have it.
- The deqp watchdog of 1.4.6.2 stands at 300 s in total and 30 s for each progress interval. It is not
  configurable, so the runner's 170 s bound is the real per-call limit. One case that runs past the bound is
  recorded as `Timeout` and stops the sweep.
- The 10x lab scale is an assumption from 170 historical cases under the drivers of 2026-09-25. The first lab
  batches decide whether to plan again.
- The desktop runs on the GPU. A lost device during a batch also hits DWM, and the workspace `STATE.md`
  holds the recovery.

Kto pyta, nie błądzi - who asks does not stray. The sweep list asks 19078 times.
