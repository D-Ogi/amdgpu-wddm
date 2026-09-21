# E14: Vulkan compute reference (Linux/RADV today, Windows/M8 later)

State: **run on 2026-09-21 on unit A** under the Alpine diagnostic stick. H1 holds for all eight
tests; H2 is not yet testable and is what this experiment exists to make testable.

## Why

Milestone M8 puts RADV on a WDDM winsys. When that stack first submits a dispatch, "it works" and
"it produces numbers" look the same from the outside. E13 gave us a reference for what the kernel
does (register writes, fences, interrupts); nothing yet says what the GPU should *compute*.

This experiment fixes that. `vkcompute` is a single C99 file over core Vulkan 1.1 that computes
every result twice - once on the GPU from SPIR-V, once on the CPU in plain C - and prints an
FNV-1a-64 hash of both. Every test is arranged so that its result is **exactly defined**: integer
arithmetic, or float32 arithmetic in which every intermediate value is exactly representable. A
different workgroup shape, a different accumulation order, `v_fma_f32` instead of a multiply and an
add - none of them may change a bit. So the table below is not a benchmark that happens to be
reproducible; it is a specification.

## Hypotheses

- **H1**: under Linux (amdgpu + RADV, Mesa 26.1.6) every GPU hash equals the CPU hash computed in
  C on the same machine. Tested here. **Holds**, 8 of 8.
- **H2**: the same SPIR-V, run through our Windows user-mode stack on the same unit, gives the same
  hashes. **To be measured in M8.** This file's result table and the `.spv` files in the evidence
  directory are the reference it will be compared against.

A hypothesis this design deliberately avoids needing: that float32 reduction order is reproducible.
It is not, in general, which is why the inputs are scaled so that nothing ever rounds.

## What the tests are, and why each is defined to the bit

Inputs come from the 32-bit LCG of Numerical Recipes, `state <- 1664525*state + 1013904223`, with
one documented seed per test (`SEED_*` in `vkcompute.c`). Values are always taken from the **high**
bits of the state: the low bits of a power-of-two-modulus LCG have very short periods (the low four
bits repeat every 16 draws), which would make the matrices and weight tables degenerate.

| test | what it does | why the result is exact |
|---|---|---|
| `fill_g1` | 64 KiB constant fill, 1 workgroup | constant store; mirrors libdrm's memset dispatch |
| `fill_g16` | the same fill, 16 workgroups | grid-stride loop, so it must produce the *same bytes* as `fill_g1` and differ only in time |
| `inthash` | 1M element xorshift-multiply mixing | pure uint32; the language defines it to the bit |
| `saxpy` | 1M element `z = a*x + y`, float32 | `a`=3, `x`,`y` integers in [0,1023]; every product and sum is an integer below 2^13 |
| `sgemm` | 256x256 float32 matrix multiply, naive | entries are integers in [0,15], so each of the 256 partial sums is an integer below 57601, far under 2^24 |
| `sgemm_tiled` | the same product through 8x8 workgroup-memory tiles | different accumulation order, same exact values, so it must give the *same hash* as `sgemm` |
| `mlp` | 2-layer perceptron 64 -> 128 -> 10, ReLU, batch 1024, argmax | inputs are multiples of 2^-2, weights of 2^-3, biases of 2^-2; layer-1 sums are multiples of 2^-5 below 113, layer-2 sums multiples of 2^-8 below 14464 - both well inside 2^24 |
| `reduce` | 1M uint32 sum, workgroup memory, two dispatches with a pipeline barrier | uint32 addition wraps mod 2^32 and is associative, so the shader's tree and the C loop must agree |

Two of these are internal cross-checks rather than extra coverage, and they are the point:
`fill_g1` vs `fill_g16` and `sgemm` vs `sgemm_tiled` must each produce **identical hashes**. They
do (see the table). If a future stack breaks that pair, the fault is in the dispatch or the
workgroup memory, not in arithmetic.

`mlp` additionally reports the argmax array's own hash and the class histogram. The histogram is
the human-readable half: a stack that returns zeros gets `1024,0,0,...` and is recognisable without
decoding a hash.

`reduce`'s per-workgroup partials are hashed along with the total, because a wrong partial
localises a failure to a 1024-element range of the input. Note that consecutive partials differ by
a constant (`0x33900000`): summing consecutive outputs of an affine generator is itself affine.
That is an artefact of the LCG, not a defect - the values still have to be computed correctly.

## Safety

Towards the GPU this is an ordinary Vulkan client: it submits compute work through amdgpu exactly
as any other program does. No register is touched, no module loaded or unloaded, nothing written to
a disk, firmware or NVRAM. The only writes outside `/tmp` are to tracefs in the `trace` phase, with
E13's arm/disarm procedure, undone at the end of it. `vkWaitForFences` always has a 10 s timeout
and the program stops on `VK_TIMEOUT` instead of retrying - a GPU that has not come back is the
lead's problem, not something to poke at.

## Procedure

```
sh build.sh              glslangValidator -V for the shaders, gcc for the C file, into /tmp/e14/build
sh run.sh env            kernel, Mesa, driver identity, GPU clocks, who else holds the render node
sh run.sh check          the full run; this is what decides pass or fail
sh run.sh timing         the full run again, for timings on a quiet GPU
sh run.sh radv           find out which RADV_DEBUG value dumps shader code here, and dump it
sh run.sh trace          amdgpu trace events armed around one fill dispatch
sh negative-control.sh   prove the comparison can fail (see below)
```

Sources are copied to `/tmp/e14/src` over ssh with tar; everything stays in RAM on the stick.

### Negative control

A harness that can only print `match=yes` proves nothing. `negative-control.sh` rebuilds `fill.comp`
with `p.value + 1u` and checks that the run reports `match=NO` and exits 1, while `inthash` from the
same directory still passes and exits 0. Measured:

```
fill_g1  n=16384  hash=0x881b077929212325 cpu_hash=0x7018cdd513a22325 match=NO   first_words=22222223 ...
exit=1
inthash  n=1048576 hash=0xbc33291b528dfab4 cpu_hash=0xbc33291b528dfab4 match=yes
exit=0
NEGATIVE CONTROL PASSED
```

Exit codes: 0 every hash matched, 1 at least one mismatch, 2 a setup or usage error. All five paths
were checked (`--list`, an unknown `--only`, a missing shader directory).

## Environment measured

| | |
|---|---|
| unit | A (ASRock BC-250), Alpine 3.24.2 diagnostic stick from RAM |
| kernel | 6.18.52-0-lts, cmdline `modprobe.blacklist=amdgpu bc250.mode=readonly` (amdgpu loaded by hand afterwards) |
| Mesa | 26.1.6-r0, `mesa-vulkan-ati` 26.1.6-r0 |
| loader / headers | vulkan-loader 1.4.347-r0, vulkan-headers 1.4.347-r0 |
| glslang | 1.4.341.0-r0, `glslangValidator` 11:16.2.0 |
| compiler | gcc (Alpine) 15.2.0, musl |
| device | `AMD BC-250 (RADV GFX1013)`, vendor `0x1002`, device `0x13fe`, integrated |
| driverVersion | `0x06801006` (26.1.6) |
| apiVersion | `0x00404162` (1.4.354) |
| queue family | 0, subgroup size **64** |
| limits | wgInvocations 1024, sharedMem 65536, pushConst 256, storageBuffersPerStage 8388606 |
| memory | 11 types, 2 heaps: heap 0 4008 MiB (not device-local), heap 1 8017 MiB (device-local) |
| GPU state during timings | DPM level 1 of 3 active = **1500 MHz**, edge 70 C, nothing else holding `/dev/dri/renderD128` |

`gpu_busy_percent` is not implemented on this SoC (read returns `-EOPNOTSUPP`); `pp_dpm_sclk` and
`fuser /dev/dri/renderD128` are what `run.sh` uses instead.

## Results

`sh run.sh timing` with `--runs 21`, quiet GPU. Hashes are identical to the earlier `--runs 5` run
taken while the lead's llama.cpp work was on the machine, so the hashes are reproducible across
processes; only the times moved.

| test | n | hash (GPU) | cpu_hash | match | median us (21 runs) | median us (5 runs, busy) |
|---|---|---|---|---|---|---|
| `fill_g1` | 16384 | `0x7018cdd513a22325` | `0x7018cdd513a22325` | yes | 52.1 | 65.5 |
| `fill_g16` | 16384 | `0x7018cdd513a22325` | `0x7018cdd513a22325` | yes | 43.5 | 45.5 |
| `inthash` | 1048576 | `0xbc33291b528dfab4` | `0xbc33291b528dfab4` | yes | 63.9 | 69.5 |
| `saxpy` | 1048576 | `0x3ee3408930ca6e61` | `0x3ee3408930ca6e61` | yes | 75.9 | 72.5 |
| `sgemm` | 65536 | `0x24396beb3a1a7367` | `0x24396beb3a1a7367` | yes | 382.0 | 387.9 |
| `sgemm_tiled` | 65536 | `0x24396beb3a1a7367` | `0x24396beb3a1a7367` | yes | 80.8 | 99.3 |
| `mlp` | 10240 | `0x0eb467ee3ae0f043` | `0x0eb467ee3ae0f043` | yes | 565.2 | 559.1 |
| `reduce` | 1025 | `0xe9fc02f21706317a` | `0xe9fc02f21706317a` | yes | 85.9 | 95.0 |

`8 test(s) run, 0 mismatch(es)`, exit 0. **H1 holds.**

The two cross-checks came out as required: `fill_g1` = `fill_g16` and `sgemm` = `sgemm_tiled`, bit
for bit, despite 16x the workgroups in one case and a completely different accumulation order
through workgroup memory in the other.

`mlp` extras: `argmax_hash=0x66d441abf069e3a2` (equal to the CPU reference), class histogram
`25,27,10,3,619,252,1,3,28,56`.

The time is wall clock from just before `vkQueueSubmit` to just after `vkWaitForFences` returns, so
it includes submission and fence latency; at these sizes that floor is roughly 40 us and dominates
the small tests. `sgemm` at 382 us against `sgemm_tiled` at 81 us is the one honest compute
measurement in the table: same arithmetic, 4.7x apart on memory traffic alone.

### The SPIR-V that produced these hashes

M8 must load these exact bytes, or the comparison proves nothing.

```
f44d48c21c4f1af26b09510f9a48d892f0b474899c95653442738f212da362f7  fill.spv
ca33985ea645d415931a60d489ff86ef078a928933fa3ac35dcb02b8d28a11f4  inthash.spv
1bf9eb2b8bfe4f3d2a19c6b44a6f9f257ed4e47c1acd7183630c41ec5660d606  mlp1.spv
6ad3b0478b7e3564f0383256bcb5b82877a723b7930c32b8c09d85a9d3217fa4  mlp2.spv
28af87f8275f192c536ca84f4ef6be822b46bece77db19f0ba9e898bec2f26c7  reduce1.spv
477398679c7e18a7c6b995a34dfd2b69ae4ec78187877cef792c96f9d4f4579e  reduce2.spv
cb1301ee2b7bbd8f68c6e74f44bb4a36f937165b1e0546e82f1a7ba92d04aa63  saxpy.spv
b3a60831a6cc76cec16d2b9c91e72d3647a43367533abd0f588f83d23a2e7bda  sgemm.spv
6ac4882c745e08dff2fa7103a4efc49f994ebb03417751b684c4a15e9d1ecba8  sgemm_tiled.spv
```

## What RADV_DEBUG offers on this build

`RADV_DEBUG=help` prints **nothing** in Mesa 26.1.6 - this RADV has no help handler - so the option
list was read out of the ICD's string table and then each candidate was tried. Worth knowing for
next time: short option names are **tail-merged** into longer ones by the linker, so `shaders` never
appears as a standalone string (it is the tail of `metashaders`) even though it works. Reading the
strings cannot settle whether an option exists; running it can.

Measured output from one `fill_g1` dispatch:

| `RADV_DEBUG=` | bytes on stderr | what it is |
|---|---|---|
| `shaders` | 6034 | NIR **and** the GFX10 machine code with encodings - this is the useful one |
| `preoptir` | 4740 | NIR before optimisation |
| `spirv` | 4098 | the SPIR-V disassembly |
| `nir` | 2376 | NIR after optimisation |
| `shaderstats` | 463 | SGPR/VGPR counts, code size, instruction mix |
| `metashaders` | 0 | RADV's own internal shaders; none run here |

The full table this ICD carries, as the contiguous debug block in its string section:

```
nofastclears nodcc nocache shaderstats nohiz nocompute allbos noibchaining spirv zerovram
syncshaders preoptir startup checkir nobinning nongg metashaders llvm forcecompress hang noumr
nodisplaydcc notccompatcmask novrsflatshading noatocdithering nonggc prologs nodma epilogs nofmask
shadowregs extra_md nogpl nort nomeshshader noeso psocachestats nirdebuginfo dump_trap_handler
pso_history bvh4 novideo validatevas dumpibs nosmemmitigation fullsync
```

`dumpibs`, `hang`, `syncshaders`, `validatevas` and `shadowregs` look directly useful for M8 and are
noted here so nobody has to rediscover them.

`radv-shaders.txt` is the full-run dump with `RADV_DEBUG=shaders`: 123 KB, 2750 lines, 10 compiled
pipelines (9 distinct modules; `fill.spv` is compiled twice, once per dispatch-size variant, and its
`source_blake3` is identical both times), 453 lines of GFX10 ISA with hex encodings. Sample from
`reduce1`, the workgroup-memory tree:

```
	ds_read2_b32 v[2:3], v0 offset1:4       ; d8dc0400 02000000
	s_waitcnt lgkmcnt(0)                    ; bf8cc07f
	v_add_nc_u32_e32 v2, v2, v3             ; 4a040702
	ds_write_b32 v0, v2                     ; d8340000 00000200
```

`shaderstats` for `fill`: 108 SGPRs, 4 VGPRs, no spills, code size 100 bytes, 20 instructions, LDS 0.

## The kernel-side trace of one dispatch

`run.sh trace` arms the amdgpu tracefs events with E13's exact arm/disarm procedure, so the result
is directly comparable with E13's traces, around a single `--only fill_g1 --runs 1` run.
`amdgpu-events-fill.txt`: 1366 lines, **0 overruns**. Event kinds:

```
242 amdgpu_device_wreg      242 amdgpu_dc_wreg        177 amdgpu_device_rreg
177 amdgpu_dc_rreg           89 amdgpu_vm_set_ptes     74 amdgpu_vm_update_ptes
 52 amdgpu_iv                42 amdgpu_sched_run_job   40 amdgpu_bo_move
 21 amdgpu_vm_bo_mapping     20 amdgpu_bo_create       13 amdgpu_vm_bo_update
 13 amdgpu_vm_bo_unmap       13 amdgpu_vm_bo_map       10 amdgpu_vm_bo_cs
  8 amdgpu_bo_list_set        7 amdgpu_vm_flush         2 amdgpu_cs
  1 amdgpu_vm_grab_id         1 amdgpu_cs_ioctl         1 amdgpu_cs_bo_status
  1 amdgpu_pasid_allocated    1 amdgpu_pasid_freed
```

So one Vulkan compute submission from a fresh process is: PASID allocation, 20 buffer objects, the
VM mappings and PTE writes for them, one `amdgpu_cs_ioctl`, one `amdgpu_vm_grab_id`, the VM flush,
`amdgpu_sched_run_job`, and the fence interrupt among the 52 `amdgpu_iv`. The `amdgpu_dc_*` and
`amdgpu_refresh_rate_track` events are the console display ticking in the background, not ours.

## Limits of this result

- One unit, one Mesa version, one kernel, one session. Nothing here says the hashes are stable
  across Mesa versions - they need not be, since RADV may legitimately compile differently. What
  must be stable is the *arithmetic*, and that is what the exactness argument protects.
- H2 is untested by construction: there is no Windows user-mode stack yet.
- The timings are wall clock including submission latency and were taken at DPM level 1 (1500 MHz)
  under Linux. Windows runs this unit underclocked to 1000 MHz from the startup task, so the times
  are **not** directly comparable across the two systems; the hashes are.
- `--runs` re-submits the same recorded command buffer. Every test is idempotent by design (no
  kernel reads a buffer it also writes across runs; `saxpy` is deliberately out of place for this
  reason), so the timing loop cannot disturb a result. A future test that is not idempotent must
  re-record.

## The inference half (`llama/`): a very simple AI test

Proposed by the owner during the session: something closer to a real workload than eight kernels, again with a result
that can be compared across the two systems. `llama/install.sh` installs Mesa's RADV, the Vulkan tools, a compiler and
Alpine's `llama.cpp` with its Vulkan backend into the probe's RAM (the stick is not written) and fetches three public
models; `llama/run.sh` generates greedily (temperature 0, fixed seed) on the GPU and on the CPU, repeats the GPU run three
times, runs `llama-bench`, and traces amdgpu's events around one short GPU run.

Measured on unit A (llama.cpp build 9564, Mesa 26.1.6, kernel 6.18.52-0-lts, 1500 MHz):

| model | GPU text, sha256 (first 16) | same as CPU? | GPU repeats | pp512 GPU / CPU (t/s) | tg128 GPU / CPU (t/s) |
|---|---|---|---|---|---|
| stories260K (1.2 MB) | `404328f9fe8ec445` | yes | - | - | - |
| stories15M Q4_0 (19 MB) | `9a75289c5ee188d7` | yes | 3 of 3 identical | 76102 / 5339 | 1292 / 1439 |
| TinyLlama 1.1B Q4_0 (638 MB) | `8a5491652c871b51` | no (CPU `11a2b2f60a0d2370`) | 3 of 3 identical | 1657 / 111 | 220 / 57 |

- The GPU's output is deterministic from run to run, so **the GPU hash is the reference for Windows** (H2 for this half:
  the same llama.cpp build, model file and prompt through our stack give the same text).
- GPU and CPU agree on the two small models and part ways on the 1.1B model at about the fortieth token ("United
  Kingdom" against "United States"): different kernels, different rounding, a near-tie between two tokens. That is
  expected and is why the CPU text is not the reference.
- Model files are identified by sha256 in `llama-install.txt`; they are public downloads and are not kept.
- One 16-token run of the smallest model is 283 `amdgpu_cs_ioctl` calls, **all on `gfx_0.0.0`**, 285 end-of-pipe vectors
  (client 20, source 181, ring 0), 173 SDMA vectors from the kernel's page table updates, 66 buffer objects created and
  2022 mapping events. RADV does not use this part's compute queues at all (Mesa's `ac_gpu_info.c` drops them for
  GFX1013 as "known to have broken compute queue"; `radv-info.txt` shows the device as RADV sees it), so for M8 the gfx
  ring is the one that matters, while the raw-ioctl memset of E13 (facts M49) shows that the compute rings do work.

## The clock half (`dpm_sweep.sh`): times that can be compared with Windows

Windows keeps unit A at 1000 MHz (the startup task), Linux runs it at 1500 MHz by default, so no time in this file could be
held against a Windows time. `dpm_sweep.sh` repeats the work at 1000 MHz. amdgpu refuses
`power_dpm_force_performance_level low|high` on this part; the one interface is `pp_od_clk_voltage`, which always names
a voltage with the clock (`cyan_skillfish_ppt.c:438-533`: `RequestGfxclk` and `ForceGfxVid`). The script sets 1000 MHz
at the voltage the part uses for 1500 MHz by itself and nothing else: no higher clock, no higher voltage. 2000 MHz was
left alone on purpose: it needs a forced voltage above the default and we have no measured curve for this unit.

| | 1000 MHz (899 mV) | 1500 MHz (default, 906 mV) | ratio |
|---|---|---|---|
| `sgemm` (naive) | 562 us | 392 us | 1.43 |
| `sgemm_tiled` | 113 us | 87 us | 1.31 |
| `mlp` | 819 us | 566 us | 1.45 |
| `inthash`, `saxpy` (1M elements) | 70, 74 us | 70, 79 us | 1.0 (submission latency, not GPU time) |
| TinyLlama 1.1B Q4_0 pp512 | 1120 t/s | 1657 t/s | 1.48 |
| TinyLlama 1.1B Q4_0 tg128 | 155 t/s | 219 t/s | 1.42 |
| PPT idle / peak under load | 49 W / 78 W | 58 W / 95 W | |
| edge temperature peak | 76 C | 79 to 80 C | |

Hashes are identical at both clocks. Throughput follows the GPU clock almost linearly, token generation included, so on
this part generation is bound by the shader clock and not by memory. **The 1000 MHz column is the one to hold Windows
against.**

## The command stream (`run-cs.sh`, `decode_cs.py`): what a winsys has to carry

`RADV_DEBUG=dumpibs` prints every IB on a healthy submit in this Mesa (found by testing; `RADV_TRACE_FILE` is dead in this
build and `hang` prints nothing until something hangs). One `fill_g1` dispatch is two IBs **on the GFX ring**: a 24-dword
preamble that points at a 168-dword state IB (CLEAR_STATE, CONTEXT_CONTROL, 17 SET_CONTEXT_REG, 7 SET_UCONFIG_REG, ...),
and a 64-dword main IB: WRITE_DATA, eight SET_SH_REG (COMPUTE_PGM_LO, PGM_RSRC1/2, NUM_THREAD_X/Y/Z, the push constants
arriving as COMPUTE_USER_DATA_3/4 = 16384 and 0x22222222), DISPATCH_DIRECT 1,1,1, DMA_DATA, EVENT_WRITE, ACQUIRE_MEM.
`decode_cs.py` runs on the PC, takes opcode names from Mesa's `sid.h` or the kernel's `nvd.h` (identical naming over all
63 packets) and register names through `tools/regcalc`; its `--selftest` holds its naming against ac_debug's.

## Evidence

`evidence/linux/2026-09-21-E14-vulkan-compute-reference/`: `compute/` (results, timings, the negative control, RADV's
shader dumps with the GFX10 machine code, the trace around one fill, `spv/` with the nine SPIR-V binaries H2 has to be
run with), `llama/` (texts, logs, benchmarks, the trace), `dpm/` (the clock half), `cs/` (the command stream), `vulkaninfo.txt`, `radv-info.txt` (`RADV_DEBUG=info`: the
whole `radeon_info` as Mesa derived it on this unit, the reference for `driver/contract/`), `llama-install.txt`.
`redact.py` replaced the UUID-like values of `vulkaninfo` as well; nothing else was touched.
