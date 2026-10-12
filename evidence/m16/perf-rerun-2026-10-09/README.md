# M16 route B: the dispatch-cost arms re-run on the merged build

MEASURED on unit A (ASRock BC-250, Windows 11 Pro), 2026-10-09 19:46 to 19:50Z, in the same
session as `../step3b-2026-10-09`. Seven arms of the microbenchmark `hipbench` and two `vadd`
controls, each a bounded non-game trial, the longest 27.2 s of wall time. No GPU fault, no
timeout, no wrong result, no stop rule reached.

`../perf-2026-10-09` measured the pre-merge build `09E3E8AE`, which held the batching work alone.
This set re-runs the arms on the merged build, which carries the defaults those numbers justified,
and answers the two questions the first session left open: whether a deeper dispatch cap beats the
default of 32, and where the 9.5 us that the first session added to every device-to-host copy under
batching comes from.

**Every arm passes.** A dispatch cap of 64 buys fewer submissions and costs 11.6 to 11.8 % of host
time per dispatch, so the default stays at 32. The 9.5 us of the first session does not appear in
this build at all, under either policy, with or without a kernel phase in front.

## The build that ran

Branch `m16/hip-step3` at `1f777613`. The three files ran from `C:\BC250\tmp\m16-perf` as ordinary
user-mode programs with the library beside the executable. Nothing was installed.

| File | Bytes | SHA-256 |
|---|---|---|
| `amdhip64.dll` | 256000 | `E754A950690D18388F823B97A53B7BC5F6981304393B83C1C0796D377521445B` |
| `hipbench.exe` | 189952 | `E2627FA78FE870B0FA22FAB64C920EDE00DD00996300CFA3B08CC484268D806A` |
| `vadd.exe` | 163840 | `389B9380B9A1ADB8F67B686722E9B2B8D1C1F65DA2CC0A0A2917FB2FAB22B340` |

These three files overwrote the three of the first session, so no number below is comparable with
the first session's except as the plan intends: as a control. The device every arm opened: `AMD
BC-250`, `gfx1013`, 40 multiprocessors, idle clock 500 MHz at VID 116. Tctl 59.9 to 60.5 C for the
whole set. The plug read 82.8 to 89.5 W.

The installed state, the artifact hashes read back on the lab and the kernel driver log of this
session are in `../step3b-2026-10-09` (`state-00-before.txt`, `state-99-after.txt`,
`state-diff.txt`, `artifact-hashes-after.txt`, `kmd-3b-final.txt`): one session, one set of
session-wide files, kept once.

## The arms

Every arm: `--expect-compute --wait-total 20000 --budget-ms 150000 --json`. `exact yes`, `api
failures 0`, `hipbench: ok` and exit 0 in all seven.

| Arm | Policy | launch us each | launch subs/dispatch | chain us each | chain end to end | chain subs/dispatch | program ms |
|---|---|---|---|---|---|---|---|
| R1 | `--batch 0 --barrier full` (build 1) | 15.796 | 1.000 (2000 of 2000) | 14.071 | 14.2 us | 1.000 (1000 of 1000) | 3412 |
| R3 | no environment at all (cap 32, light, state cache on) | **7.305** | 0.033 (65 of 2000) | **3.411** | 3.6 us | 0.032 (32 of 1000) | 3386 |
| R5 | `--batch-max 64` | 8.167 | 0.018 (37 of 2000) | 3.805 | 3.9 us | 0.016 (16 of 1000) | 3383 |
| R5n | `--batch-max 64`, `BC250_HIP_PM4_STATE_CACHE=0` | 8.585 | 0.018 (36 of 2000) | 4.641 | 4.8 us | 0.016 (16 of 1000) | 3390 |
| Rc build 1 | one dispatch each, 2048 copies, `--batch 0 --barrier full` | 8.100 | 1.000 (1 of 1) | 5.200 | 40.2 us | 1.000 (1 of 1) | 24191 |
| Rc default | the same, bare | 2.000 | 1.000 (1 of 1) | 1.300 | 40.0 us | 1.000 (1 of 1) | 24098 |
| R1b | `--batch 0 --barrier full`, the drift control, run last | 17.047 | 1.000 (2000 of 2000) | 15.043 | 15.2 us | 1.000 (1000 of 1000) | 3417 |

`launch` is 2000 independent empty kernels. `chain` is 1000 kernels that each read what the one
before it wrote, which is the shape of a decode step, and `exact yes` means every one of the 65536
items held exactly 1000 at the end. The copy, synchronisation and event lines of every arm:

| Arm | copy 4 KB h2d | **copy 4 KB d2h** | copy 1 MiB h2d | copy 1 MiB d2h | copy 64 MiB h2d | copy 64 MiB d2h | sync us each | event us each |
|---|---|---|---|---|---|---|---|---|
| R1 | 0.270 us | **19.301 us** | 28.175 us | 4882.575 us | 4315.438 us | 359994.775 us | 0.051 | 0.091 |
| R3 | 0.274 us | **19.377 us** | 29.512 us | 4890.988 us | 4306.287 us | 360433.725 us | 0.052 | 0.092 |
| R5 | 0.300 us | **19.257 us** | 32.863 us | 4880.337 us | 4322.100 us | 359935.188 us | 0.051 | 0.093 |
| R5n | 0.270 us | **19.282 us** | 30.437 us | 4883.412 us | 4318.962 us | 360415.175 us | 0.051 | 0.092 |
| Rc build 1 | 0.278 us | **19.376 us** | 29.827 us | 4912.903 us | 4311.156 us | 361715.847 us | 0.100 | 1.700 |
| Rc default | 0.273 us | **19.264 us** | 27.967 us | 4886.734 us | 4314.158 us | 360323.419 us | 0.100 | 1.600 |
| R1b | 0.270 us | **19.598 us** | 28.088 us | 4877.813 us | 4323.050 us | 360287.263 us | 0.052 | 0.093 |

Rc's copy lines are 2048 iterations at 4 KB and 64 at 1 MiB and 64 MiB. Every other arm's are 256
and 8. Its `sync` and `event` lines are a single sample each and carry no spread. The first launch
of a process, with the code object load inside it, was 0.558 to 0.611 ms in every arm. Every
number of every measurement is in `measurements.jsonl`, 49 JSON lines, one per measurement, each
tagged with its arm, and the console output of all nine arms is in `R*.out.txt` verbatim.

## The controls

- **`vadd.exe --expect-compute --wait-total 10000` before the first arm and after the last**:
  `vadd: ok`, `api failures 0`, `mismatches 0 of 65536` in both passes, the memory back to
  8589926400 of 8589934592 after each teardown, exit 0.
- **R1b against R1**, the first and the last `hipbench` arm of the set: launch 17.047 against
  15.796 us and chain 15.043 against 14.071 us, which is 7.92 % and 6.91 % with R1 as the base.
  Both are inside the 13 % spread the first session measured for the chain line, both arms measure
  exactly 1.000 submissions per dispatch on both lines, and Tctl was 60.25 against 60.38 C.
  Nothing drifted, and this pair is the base the batched arms are read against.
- **R1 against the first session's P1 and P1b**: launch 15.796 against 16.933 and 16.579 us, chain
  14.071 against 14.612 and 16.499 us, all four at 1.000 submissions per dispatch. The merged
  build's build-1 policy is the old build's build-1 policy within the run-to-run spread.
- **R3 against the first session's P3**: R3 sets no environment at all and the program reports
  `batch 1 (default), barrier light (default)`. Launch 7.305 against 7.601 us, chain 3.411 against
  3.832 us, the same 0.033 and 0.032 submissions per dispatch. The defaults reach a program, and
  they measure what the environment measured before them.
- **The driver log** (`../step3b-2026-10-09/kmd-3b-final.txt`): over the whole session 11029 new
  node-0 hardware submissions, 0 timeouts, 0 refused, 0 soft-recovered, 0 "not run", no page
  fault, no TDR. The final summary reads `last completed fence 810304` against 810304 submitted,
  so the hardware fence reached the last submission and no submission is outstanding.

## Pass criteria of the plan, one by one

| # | Criterion | Verdict |
|---|---|---|
| 1 | every arm exits 0 and prints `exact yes` for the chain | **MET**, seven of seven |
| 2 | `vadd --expect-compute` passes before the first arm | **MET** |
| 3 | the build-1 arm measures 1.000 submissions per dispatch | **MET**, in R1 and R1b, on both lines |
| 4 | the batched arms measure the submissions per dispatch their caps imply | **MET, with the time-cap deviation named**. The chain line is exact: 32 of 1000 at cap 32, and 16 of 1000 at cap 64. The launch line is 65 against the ideal 63 at cap 32. At cap 64 it is 37 against 32. That is two and five submissions over the rounding. The reason is the 1 ms batch time cap. It expires inside a 15 to 17 ms loop, and the first session isolated it with arm P4b |
| 5 | no device loss, no timeout, no API failure, no dangling submission | **MET** |
| 6 | no new error in the driver log, the closing `vadd` passes | **MET** |
| 7 | every number of every measurement recorded, pass or fail | **MET**, `measurements.jsonl` and the console output of all nine arms |

## R5: a deeper dispatch cap buys nothing here

| | R3, cap 32 | R5, cap 64 | R5 against R3 |
|---|---|---|---|
| launch, submissions per dispatch | 0.033 (65 of 2000) | 0.018 (37 of 2000) | 1.76 times fewer |
| launch, us a dispatch | 7.305 | 8.167 | **11.8 % slower** |
| chain, submissions per dispatch | 0.032 (32 of 1000) | 0.016 (16 of 1000) | 2.00 times fewer |
| chain, us a dispatch | 3.411 | 3.805 | **11.6 % slower** |
| chain, end to end | 3.6 us | 3.9 us | slower |
| copy, sync, event | within noise | within noise | unchanged |

A cap of 32 therefore stays the default: the gain in submissions is already in, and a deeper cap
adds a boundary check per dispatch that costs more than it saves. The first session measured the
same shape at a cap of 256 (8.823 against 7.601 us, 4.596 against 3.832 us).

**This upholds fact M856. It is not proof of it.** M856 says the kernel argument pool of 64
buffers is the ceiling. At a cap of 64 the dispatch cap itself produces the 16 submissions for
1000 dependent dispatches, while the dword cap of this build allows 711 dispatches a ring slot
with the state cache and 227 without it. Neither R5 nor R5n reaches the dword cap or the
64-buffer pool, and no cap above 64 ran on the merged build, so these two arms cannot measure
where the pool ceiling is. What they measure is what the decision needs.

## R5n: what 72 dwords a dispatch instead of 23 cost on the hardware

R5n is R5 with `BC250_HIP_PM4_STATE_CACHE=0`, which turns off the per-dispatch state cache of
design section 8.8. `test_pm4` measures the stream itself on the development PC: 72 dwords for a
repeated dispatch without the cache and 23 with it. What that costs on the hardware, which no host
test can measure:

| | R5, cache on | R5n, cache off | the cache's worth |
|---|---|---|---|
| launch, us a dispatch | 8.167 | 8.585 | **0.418 us (5.1 %)** |
| chain, us a dispatch | 3.805 | 4.641 | **0.836 us (22.0 %)** |
| chain, end to end | 3.9 us | 4.8 us | 0.9 us |
| submissions per dispatch, both lines | 0.018 and 0.016 | 0.018 and 0.016 | unchanged |
| copy 4 KB d2h, sync, event | 19.257 / 0.051 / 0.093 us | 19.282 / 0.051 / 0.092 us | unchanged |

So the cache is worth **0.836 us of host time for each dependent dispatch**, which is 22.0 % of
the whole cost of a dispatch on the chain line, and 0.418 us (5.1 %) for an independent one. 49
dwords saved buys 0.836 us, which is about 17 ns a dword. That is the host writing into the mapped
ring and not the GPU reading it, because the submissions per dispatch and the end-to-end time move
together with the host time.

What does not change is the number of buffers. At a cap of 64 the dispatch cap closes a buffer
before the dword cap does under either setting, because 64 dispatches at 72 dwords is 4608 of the
16368 usable dwords. The cache buys host time here and not a deeper batch.

## Rc: fact M857 closes, because the difference is not reproduced

M857 recorded that `copy 4 KB from device` was 19.2 to 19.4 us in the two unbatched arms of the
first session and 28.8 to 28.9 us in its four batched ones: about 9.5 us of fixed cost added to
every device-to-host copy when batching is on. Rc runs one launch and one chain link in front of
the copies instead of 2000 and 1000, so the machine is in the same state under both policies:

| | 4 KB d2h | 4 KB h2d | 1 MiB d2h | 64 MiB d2h |
|---|---|---|---|---|
| Rc, build 1 | **19.376 us** | 0.278 us | 4912.903 us | 361715.847 us |
| Rc, bare (cap 32, light, cache on) | **19.264 us** | 0.273 us | 4886.734 us | 360323.419 us |
| difference | **0.112 us (0.6 %)** | 0.005 us | 26 us (0.5 %) | 1392 us (0.4 %) |

The 0.112 us between the two policies is a quarter of the spread of that line under one policy
(R1 19.301 against R1b 19.598 us, 0.297 us apart). And the arm is stronger than it had to be: the
three batched arms with the full kernel phase in front measure 19.377, 19.257 and 19.282 us. On
this build the 4 KB device-to-host copy costs the same under every policy, with or without a
kernel phase in front, with the state cache on or off.

**So M857 closes as not reproduced.** The difference was a property of the pre-merge build
`09E3E8AE` and this build does not show it, under any of the six policies run here. Nothing needs
instrumenting in the copy path. For the record, Rc also measures that at one dispatch the batched
policy is faster on the host (launch 2.000 against 8.100 us, chain 1.300 against 5.200 us),
because a single dispatch under batching returns to the caller before the submission, while the
end-to-end time is the same 40.0 against 40.2 us.

## What this means for a decode step

400 kernels a token, which is 12 a layer over 32 layers plus about 10 for the sampling tail. `L`
is the host cost of one dispatch from the `chain` line, because its kernels depend on each other
as a decode step's do. The `launch` line is beside it because its kernels depend on nothing and the
GPU may overlap them.

| Quantity | R1 and R1b (build 1) | R3 (cap 32, light) | R5 (cap 64) | R5n (cap 64, no cache) |
|---|---|---|---|---|
| kernels a token | 400 | 400 | 400 | 400 |
| submissions a token | 400 | 13 | 7 | 7 |
| `L` chain, us a dispatch | 14.071 and 15.043, mean 14.557 | 3.411 | 3.805 | 4.641 |
| off-GPU cost a token, chain | 5823 us | 1364 us | 1522 us | 1856 us |
| **tokens a second at that cost alone** | **172** | **733** | **657** | **539** |
| `L` launch, us a dispatch | 15.796 and 17.047, mean 16.422 | 7.305 | 8.167 | 8.585 |
| off-GPU cost a token, launch | 6569 us | 2922 us | 3267 us | 3434 us |
| tokens a second at that cost alone | 152 | 342 | 306 | 291 |

The build-1 column is the mean of R1 and R1b. The other three columns are one run each.

Read honestly: the host path of build 1 caps decode at about 172 tokens a second before any shader
runs, and the defaults of this build raise that ceiling to about 733. The Vulkan backend generates
300.49 tokens a second on TinyLlama 1.1B Q4_0 on this hardware, measured in the same session with
the dynamic power management free to its 1500 MHz ceiling (`../step3b-2026-10-09`). So for a model
of that size the dispatch path of build 1 would have been the first bottleneck, 172 against 300,
and with the defaults of this build it is not, 733 against 300. That is the reason the defaults
moved. The caveat stands: the state cache is worth 334 us a token at 400 kernels, and it is on by
default.

## Stop rules: none reached

No device loss, no timeout, no `exact NO`, no hang, no TDR, no bugcheck. Tctl 59.9 to 60.5 C
through the set: these arms are microseconds of shader work and they heat nothing. No arm reached
its 170 s bound, the longest being 27.2 s, and the budget of the program itself was never spent.

Jak nie zmierzysz, to nie wiesz. (If you do not measure it, you do not know it.)
