# M16 route B step 3B: llama.cpp on our HIP runtime, measured on unit A

MEASURED on unit A (ASRock BC-250, Windows 11 Pro), 2026-10-09 19:30 to 19:50Z. Four arms (a) to
(d) of part 3B, with part 3A as its precondition in front of them. Each arm is a bounded non-game
trial, the longest 27.7 s of wall time. No GPU fault, no timeout, no device loss, no wrong number,
no stop rule reached.

**Arm (a) passes and arms (b), (c) and (d) do not run.** Our own hipBLAS library computes the
right numbers on the hardware: 532 checks, 0 failures. Every llama.cpp program of the HIP backend
stops at its first kernel launch with `hipErrorNotSupported`, so no model runs on the HIP side, and
the token comparison that gate G5 asks for cannot be made. The Vulkan side of arms (b) and (c) ran
in the same session, so the baseline and the reference text are in hand.

The failure is defect BD-110. Its cause was found offline on the same day, from the staged bytes
and not from a second lab arm: 1752 of the 7105 gfx1013 kernels in `ggml-hip.dll` enable
`ENABLE_SGPR_DISPATCH_PTR`, because a kernel that reads its own `blockDim` loads it from the AQL
kernel dispatch packet, and `bc250hsa_plan_user_sgprs` refused that bit by name. Rule 3 of
`compute/hip/README.md` is the answer, and section 7.1 of `compute/hip/include/bc250hsa.h` is its
interface. All eight kernels of `bc250hipblas.dll` are free of the bit, which is why arm (a) passed
in the same session on the same runtime.

## The build that ran

Branch `m16/hip-step3` at `1f777613`. The lab received the files the plan names, read them back
before the first arm and again after the last one (`artifact-hashes-after.txt`, which ends
`verify: 42 ok, 0 not ok`). Nothing was installed: every program ran from `C:\BC250\tmp\...` with
its libraries beside it.

| File | Bytes | SHA-256 |
|---|---|---|
| `amdhip64.dll` | 256000 | `E754A950690D18388F823B97A53B7BC5F6981304393B83C1C0796D377521445B` |
| `bc250hipblas.dll` | 156672 | `A1B9E3A503C0B8148D9A62FAC29B9A73F5A611E9B20A071D88D1D358E05A2237` |
| `gemm_check.exe` | 202752 | `345FB7406AC6EAB5C7F07F17EDA34FAD27EF9B3237F7758201A42E196A998EE0` |
| `hipthreads.exe` | 198656 | `3527D90DC62B6ADABE907B108ACE0D907973772788B5F2FB34471A9BCCACB9B5` |
| `vadd.exe` | 163840 | `389B9380B9A1ADB8F67B686722E9B2B8D1C1F65DA2CC0A0A2917FB2FAB22B340` |
| `ggml-hip.dll` | 54291968 | `1102A1DDEAF3EBFF5DF394C6EC4C52C23E1E8616EECC401A2E6E103EA74564D0` |
| `ggml-hip.dll`, no-assembly variant | 54435328 | `B45DCA6D8C5D7FA971EFD11FE34AE02EA51CCCBE7F276CC3E94D9FD59FAE7C24` |
| `ggml-vulkan.dll` | 45315584 | `2E45226E359701198F8FB131F960EC5C46E98568E012C76C39D3DC8370F4668A` |
| `tinyllama-1.1b-chat-v1.0.Q4_0.gguf` | 637699456 | `DA3087FB14AEDE55FDE6EB81A0E55E886810E43509EC82ECDC7AA5D62A03B556` |

The other staged files are the nine llama.cpp libraries and two executables of each of the three
llama.cpp builds. The whole listing is in `artifact-hashes-after.txt`. The lab read back 42 of 42
files and no hash differs. The no-assembly directory holds `amdhip64.dll` and `bc250hipblas.dll`
as well, with the same bytes: `ggml-hip.dll` imports both by name, and Windows resolves an import
from the directory of the executable, so the variant directory cannot load without them. That is a
stated deviation from the plan's push list, and it is visible in the hash listing.

## Installed state: unchanged

`state-00-before.txt` (19:30:58Z) and `state-99-after.txt` (19:49:46Z) are equal on all 24 lines
apart from their own tag line, if trailing whitespace is ignored: the `kmdver` line differs by one
trailing space and holds an empty value in both captures, which `state-diff.txt` states itself.

- kernel driver `bc250kmd.sys`, 675256 bytes,
  `181C3E319F92867E63C6010498CD51E21D88A1056536477FE75C20CD8BAAFD9E`
- the system Vulkan driver `vulkan_radeon.dll`, 21914112 bytes,
  `4E3F393F9761CB53BDE8F5EBE5CFB04D4A62C9E6DCB8A101778CF664B4C72A04`
- `EnableRingVmFlush` absent, `EnableDpAudioContainerId` 1, `CuMode` 40, `DpmMaxMHz` 1500,
  `DpmIdleMHz` 500, `ReportAmdDriverVersion` absent, `HangRecoveryMode` 1, `TdrDelay` 10
- boot `2026-10-09T16:12:09Z`, the same before and after: nothing restarted

Free space on drive C: of the lab was 161.77 GB before the session and 161.03 GB after it. The
model file is the difference.

## The arms

Every arm ran through a wrapper that reads the clock before and after, refuses to launch at Tctl
87 C or above, samples Tctl every 3 s while the program runs, stops the program at 89 C or at 87 C
held for 10 s, and bounds it at 170 s.

| Arm | Program | Result | Wall | Exit |
|---|---|---|---|---|
| (a) GEMM on the hardware | `gemm_check.exe --wait-total 20000` | **PASS**, `numeric on`, `532 checks, 0 failed`, expected dispatches 43 | 4.1 s | 0 |
| (b) HIP bench | `llama-bench.exe -m tinyllama...Q4_0.gguf -p 512 -n 128 -ngl 99 -r 1 -t 4` | **FAIL**, `hipErrorNotSupported` at the first launch, no row printed | 9.5 s | 0xC0000409 |
| (b) HIP bench, build-1 policy | the same with `BC250_HIP_BATCH=0 BC250_HIP_BARRIER=full` | **FAIL**, byte-identical message | 3.1 s | 0xC0000409 |
| (b) Vulkan bench | the same command on the Vulkan backend | **PASS**, pp512 **2634.20** t/s, tg128 **300.49** t/s | 13.1 s | 0 |
| (c) HIP 64 tokens | `llama-completion.exe ... -s 1234 --temp 0 -n 64 -no-cnv -p "The capital of Poland is"` | **FAIL**, the same error, 1.7 ms after the thread pool was built | 27.7 s | 0xC0000409 |
| (c) Vulkan 64 tokens | the same command on the Vulkan backend | **PASS**, 64 tokens, `vk-64.txt` | 23 s | 0 |
| (d) no-assembly dp4a | `llama-bench.exe` on the second HIP build | **FAIL**, the same error | 9.5 s | 0xC0000409 |

Tctl 59.5 to 64.6 C for the whole session. The GPU idles at 500 MHz and the arms ramped it. In the
two Vulkan arms the dynamic power management reached **1500 MHz** at 919 mV for about a second
each time, which the kernel driver ring records at uptime 12633.7 and 12727.9, and then stepped
back to 1000 MHz. That is the deployed ceiling `DpmMaxMHz` 1500 and no rule was broken, but it is
the condition under which 2634.20 and 300.49 were measured. The plug read 82.8 to 89.5 W, 0.589 to
0.628 A and 239.2 to 240.1 V during the two bench arms, far below the 300 W bound of the power
supply.

## Arm (a): the GEMM kernel is right on the hardware

```
gemm_check: numeric on
...
gemm_check: expected dispatches 43
gemm_check: 532 checks, 0 failed
gemm_check: ok
```

Exit 0 in 4.1 s (`3b-a-gemm-20261009T193922Z.out.txt`, 50 lines). The run included the numeric
cases over f32, f16 with an f32 accumulator and bf16, all four transpositions, odd shapes, both
batch forms, `beta` 0, 1 and 0.25, the zero-K case, every refusal of all eleven entry points, and
the negative control, whose 33 elements differed as they must. The development PC measures 446
checks offline. The hardware run measures 532, because the numeric cases run here.

This is the criterion that turns `compute/hipblas` from correct on the processor into correct on
the hardware, and it is met.

## Arms (b), (c) and (d): the first kernel launch is refused

All three HIP arms stop at the same source line with the same message:

```
ggml_cuda_init: found 1 ROCm devices (Total VRAM: 8192 MiB):
  Device 0: AMD BC-250, gfx1013 (0x1013), VMM: no, Wave Size: 32, VRAM: 8192 MiB
ROCm error: this build does not support the operation
  current device: 0, in function ggml_cuda_kernel_launch at .../ggml/src/ggml-cuda/common.cuh:1721
  hipGetLastError()
.../ggml/src/ggml-cuda/ggml-cuda.cu:109: ROCm error
```

"this build does not support the operation" is our own text for `hipErrorNotSupported`, so the
refusal is ours. Everything in front of the launch works: the backend enumerates the device
correctly, and in arm (c) it loaded the model and built its thread pool first. The three error
streams are byte-identical, which is why the two controls below are literal and not approximate.

What the session ruled out, each by a measurement:

1. **Not the batching or the barrier policy.** The same arm under `BC250_HIP_BATCH=0` and
   `BC250_HIP_BARRIER=full`, which is build 1, gives a byte-identical error.
2. **Not the RDNA1 inline-assembly branch of `dp4a`.** Arm (d) is a separate build of the whole
   backend with that branch compiled out, and it gives the same error.
3. **Not the device-printf refusal.** The kernel metadata of both `ggml-hip.dll` files holds 7105
   kernels in 139 offload bundles, `hidden_block_count_x` and `hidden_group_size_x` 708 times
   each, and `hidden_hostcall_buffer`, `hidden_printf_buffer`, `hidden_heap_v1`,
   `hidden_queue_ptr`, `hidden_multigrid_sync_arg` and `hidden_completion_action` zero times. No
   ggml kernel asks for a host call buffer.
4. **Not the hardware and not the kernel driver.** Over the whole session the driver log holds
   11029 new node-0 hardware submissions, 0 timeouts, 0 refused, 0 soft-recovered, 0 "not run", no
   page fault and no TDR: `ResetEngine`, `ResetFromTimeout` and `RestartFromTimeout` were never
   called, and the last completed fence is 810304 against 810304 submitted. `vadd.exe
   --expect-compute` passed before the arms and after them. The refusal never reached the ring.

The session could not narrow it further with these bytes, because `amdhip64.dll` had seven
environment switches and none of them turned on the library log. The offline work that followed
added that switch (`BC250_HIP_LOG`) and found the cause, which the head of this record states.

## The Vulkan side, recorded for the comparison this session could not make

`llama-bench`, TinyLlama 1.1B Q4_0, `-p 512 -n 128 -ngl 99 -r 1 -t 4`, 13.1 s, exit 0, llama.cpp
build `b86d2f075 (11482)`, device `AMD BC-250 (RADV GFX1013) (radv) | uma: 1 | fp16: 1 | warp
size: 32 | shared memory: 65536 | int dot: 0 | matrix cores: none`:

| Backend | pp512 (t/s) | tg128 (t/s) |
|---|---|---|
| Vulkan, this session, DPM free to 1500 MHz | **2634.20** | **300.49** |
| HIP, this session | did not run | did not run |
| HIP, no-assembly dp4a | did not run | did not run |

The 64-token reference text is `vk-64.txt`, 251 bytes, SHA-256
`00D338AEC0070BEF86CC8D7D113CBB2D874B8082892E0EDF08DC307D3D55100E`, greedy at `--temp 0` with seed
1234. It is the exact string the HIP side has to reproduce. Kill criterion K3 of `docs/routes.md`
cannot be evaluated: it compares generated tokens of the two backends and the HIP side has none.

## Part 3A, run as the precondition of part 3B

`vadd.exe --expect-compute --wait-total 10000` passed twice: `vadd: ok`, `api failures 0`,
`mismatches 0 of 65536` in both passes, and the memory back to 8589926400 of 8589934592 after each
teardown.

`hipthreads.exe --wait-total 20000 --threads 4 --iterations 4 --spin 2000000 --grid 256`, three
runs, the second and the third as policy controls:

| Run | Policy | Phase A wait | Operations inside it | Phase B, 4x4 waits | Serialised | Verdict |
|---|---|---|---|---|---|---|
| 1 | defaults (batch 1, light) | 842.4 ms | 29 (13 / 0 / 16) | 578.9 ms | 591.2 ms | FAIL |
| 2 | `BATCH=0` | 1540.4 ms | 48 (16 / 16 / 16) | 583.8 ms | 591.0 ms | FAIL |
| 3 | `BATCH=0 BARRIER=full` (build 1) | 69.7 ms | 8 (0 / 8 / 0) | 585.5 ms | 592.1 ms | FAIL |

Phase A answers the question the owner asked on 2026-09-29: a wait of one thread does not hold the
others off, and 8 to 48 operations of the other threads complete inside it. The length of that
wait follows the clock, which is why it is 69.7 to 1540.4 ms.

Phase B fails its criterion: four threads with four waits each take 578.9 to 585.5 ms against a
serialised 591.0 to 592.1 ms, which is 1.01 to 1.02 times and not the four times the shape allows.
The three policies give the same answer within 1.2 %, so the batching default is not the cause.
Filed as BD-111. The offline reading of the path that followed this session names the cause and
section 4.7a of `docs/design/m16-hip-route-b.md` states it: one node-0 context, one indirect
buffer at a time in the ring, and a barrier between two dispatches of one buffer, so a
compute-bound dispatch costs its own device time whatever the thread count. The criterion itself
was wrong for this device, and the four times came from the mock backend, which retires each
dispatch on its own timer.

## Pass criteria, one by one

| Arm | Criterion | Verdict |
|---|---|---|
| (a) | `gemm_check` prints its checks with 0 failures and exits 0, numeric cases included | **MET**: 532 checks, 0 failed, `numeric on`, exit 0 |
| (b) | both backends print `pp512` and `tg128`, with the clock, the CU mode and the temperature | **NOT MET**. Vulkan yes: 2634.20 and 300.49, from a 500 MHz idle clock that reached 1500 MHz, `CuMode` 40, Tctl 59.75 to 64.63 C. The HIP side printed no number |
| (c) | the 64 generated tokens are identical between HIP and Vulkan | **NOT MET**. Vulkan produced 64 tokens. HIP produced none, so there is nothing to compare. This is the acceptance criterion of gate G5 and of M16, and it is not met |
| (d) | the run finishes and its two numbers go next to arm (b)'s | **NOT MET**, the same refusal. The cost of the RDNA1 assembly branch stays unmeasured on the hardware |
| all | no memory fault, no timeout, no device loss in the driver log | **MET** |
| all | Tctl below 87 C | **MET**, 59.5 to 64.6 C |
| all | each arm inside three minutes | **MET**, the longest 27.7 s |

## Files

33 files, the verbatim output of every arm in both streams, one pair per arm, named by the arm and
by the UTC time it began:

- `pre-vadd-*`, `pre-vadd2-*`, `pre-3a-threads-*` (three runs): part 3A and its controls
- `3b-a-gemm-*`: arm (a)
- `3b-b-hip-bench-*`, `3b-b-hip-bench-build1-*`, `3b-b-vk-bench-*`: arm (b)
- `3b-c-hip-64-*`, `3b-c-vk-64-*`, `vk-64.txt`: arm (c)
- `3b-d-noasm-bench-*`: arm (d)
- `kmd-3b-before.txt`, `kmd-3a-after.txt`, `kmd-3b-b-fail.txt`, `kmd-3b-final.txt`: the driver log
  ring, its summary, the information line and the clock reading at four points of the session
- `state-00-before.txt`, `state-99-after.txt`, `state-diff.txt`: the installed state
- `artifact-hashes-after.txt`: all 42 staged files read back on the lab after the last arm

`kmd-3b-final.txt`, `artifact-hashes-after.txt` and the three state files cover the whole session,
the perf re-run set of `../perf-rerun-2026-10-09` included, and are kept here only.

Jak nie zmierzysz, to nie wiesz. A jak zmierzysz i nie chodzi, to tez wiesz. (If you do not measure
it, you do not know it. And if you measure it and it does not work, you also know.)
