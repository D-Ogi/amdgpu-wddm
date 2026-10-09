# M16 route B: what one kernel dispatch costs off the GPU, measured on unit A

MEASURED on unit A (ASRock BC-250, Windows 11 Pro), 2026-10-09 17:29 to 17:40Z. Six arms of the
microbenchmark `hipbench`, each a bounded non-game trial, the longest 3.8 s of wall time. No GPU
fault, no timeout, no wrong result, no stop rule reached. Facts M850 to M854.

The question: build 1 of this route submits one kernel dispatch as one indirect buffer and one
`D3DKMTSubmitCommand`. A decode step of a language model is hundreds of kernels, so the fixed cost
of a dispatch decides whether the route is usable. Design section 8 adds two switches for it,
dispatch batching and a lighter cache barrier between two dispatches of one buffer, and this
session measured both.

## The build that ran

**This is the pre-merge perf build, not the build that carries the defaults.** The three files
below are branch `m16/hip-perf` at `dd23cf6e`, which held the batching work alone. The integration
branch `m16/hip-step3` merged that lane with the hipBLAS lane afterwards, and the defaults that
this session's numbers justify are in the merged build and not in the one measured here. The next
lab slot runs the merged build. `scratch/m16-hip/lab/perf-README.md` names its hashes.

| File | Bytes | SHA-256 |
|---|---|---|
| `amdhip64.dll` | 249856 | `09E3E8AEDB7472B270AF83DB8049F9FA11020517811833392EF34C1D0DAB2270` |
| `hipbench.exe` | 189952 | `8787081BCD9C01A9DBE7B0E515E8ED203B06B4C99EBB9EBCF5D92281121DFE62` |
| `vadd.exe` | 163840 | `98E737D2285C75E07EF6469AEDCFDC047AEDB9C046C378906183FBF3A35A2DF9` |

Each arm ran as an ordinary user-mode program with the DLL beside the executable. Nothing was
installed, and `state-00-before.txt` and `state-99-after.txt` are equal line for line apart from
their timestamp: the same installed Vulkan driver, the same kernel driver `bc250kmd.sys`
(`181C3E31`, milestone 7 revision 216), `CuMode` 40, `DpmMaxMHz` 1500, `DpmIdleMHz` 500,
`TdrDelay` 10, and the same boot of 2026-10-09T16:12:09Z before and after.

The device every arm opened: `AMD BC-250`, `gfx1013`, 40 multiprocessors, 8589934592 bytes of
local memory. Tctl 59.6 to 60.4 C for the whole session, the plug at 80.0 W.

## The six arms

Every arm: `hipbench --expect-compute --wait-total 20000 --budget-ms 150000 --json`. Exit 0,
`exact yes` and `api failures 0` in all six.

| Arm | Policy | launch us each | launch subs/dispatch | chain us each | chain subs/dispatch | exact |
|---|---|---|---|---|---|---|
| P1 | batching off, barrier full | 16.933 | 1.000 | 14.612 | 1.000 | yes |
| P2 | cap 32, barrier full | 7.962 | 0.033 | 4.819 | 0.032 | yes |
| P3 | cap 32, barrier light | 7.601 | 0.033 | 3.832 | 0.032 | yes |
| P4 | cap 256, barrier light | 8.823 | 0.018 | 4.596 | 0.016 | yes |
| P4b | cap 256, light, hold 100 ms | 8.912 | 0.016 | 4.607 | 0.016 | yes |
| P1b | batching off, full (drift control) | 16.579 | 1.000 | 16.499 | 1.000 | yes |

`launch` is 2000 independent empty kernels. `chain` is 1000 kernels that each read what the one
before it wrote, which is the shape of a decode step, and `exact yes` means every one of the 65536
items of the chain held exactly 1000 at the end. P4b raises the batch time cap from its default
1000 us to 100000 us at the same dispatch cap, which is what separates the two caps below.

The copy, synchronisation and event lines of each arm are in `measurements.jsonl`, with the
console output of every arm verbatim in `arm-00-vadd-before.txt` to `arm-07-vadd-after.txt`.

## What the numbers say

**Batching is exact and it is faster.** P2 and P3 against the pair P1 and P1b: 2.1 to 2.2 times
less host time on the launch line, 3.0 to 4.3 times less on the chain line, and submissions per
dispatch from 1.000 to 0.032, which is one submission for 32 kernels. `sync` (0.057 us) and
`event` (0.090 to 0.093 us) do not move, and neither do host-to-device copies at any size or
device-to-host copies at 1 MiB and 64 MiB.

**The light barrier is exact and it is faster than the full one.** P3 against P2: chain 3.832
against 4.819 us a dispatch, launch 7.601 against 7.962 us. Nothing else moves. The open question
of design section 8.5 was whether a dependent dispatch may skip the level-2 cache write-back and
invalidate on this part. Two chains of 1000 dependent kernels in this session (P3 and P4) answer
yes, both with `exact yes`.

**The drift control holds.** P1b against P1: launch 16.579 against 16.933 us, 2.1 % apart, both
exactly 1.000 submissions per dispatch, Tctl 59.75 C before each. The chain line of that pair is
14.612 against 16.499 us, 13 % apart, which is the run-to-run spread of a 15 ms measurement, and
it is why the batched chain figures above are read against the pair and not against one run.

**The driver saw nothing wrong.** 6781 new node-0 hardware submissions across the session, 0
timeouts, 0 refused, 0 soft-recovered, no page fault, no reset, no new error of any kind
(`kmd-before.txt`, `kmd-after.txt`, `kmd-final.txt`). The last submitted fence value was reached.
`vadd --expect-compute` passed before the first arm and after the last one, 0 mismatches of 65536
values in each pass, with the free memory back to its earlier figure after every teardown.

## Why a dispatch cap above 64 cannot be reached

This is the one place where the session's own write-up drew the wrong conclusion, and the
arithmetic below replaces it.

P4b has the time cap effectively off and a dispatch cap of 256, and it closed **exactly 32**
buffers for 2000 launches and **exactly 16** for 1000 chained launches. Both are 62.5 dispatches a
buffer. The session attributed that to the dword capacity of a command ring slot and concluded that
one dispatch and its barrier is 262 dwords. It is not. The pure builder of layer 1 writes **72**
dwords for a dispatch and its barrier, which the golden stream of `tests/host/test_pm4.c` states
to the dword (83 dwords for one dispatch alone: 11 of buffer head, 64 of body, 8 of completion
write). A 65536-byte slot holds 16368 usable dwords, so the dword cap would close a buffer at about
227 dispatches and cannot be what closed these at 62.5.

What closed them is in layer 2: the kernel argument pool, `kKernargPoolMax` in
`compute/hip/runtime/hip_launch.cpp`, which holds **64** buffers. A kernel argument buffer may not
be written again until the dispatch that reads it has retired. Launch 65 of a run therefore finds
every buffer in flight and waits for the oldest, and a wait for a value an open buffer has promised
is a flush point: it submits the buffer. The predictions of that mechanism are exact against this
session:

| Prediction | Measured |
|---|---|
| 2000 launches, pool 64: ceil(2000/64) = 32 submissions | 32 (P4b launch) |
| 1000 launches, pool 64: ceil(1000/64) = 16 submissions | 16 (P4b chain and P4 chain) |
| one wait per buffer boundary | 32 waits on the launch line, 16 on the chain line (P4b) |

P4 differs from P4b by five submissions (37 against 32), and those five are the 1000 us time cap
expiring inside a 17.7 ms loop. P2 and P3 are 65 submissions for 2000 launches, two above the ideal
63 of a cap of 32, and exactly 32 for 1000 chained launches, which is the ceiling of 1000/32.

Three consequences:

- **A cap of 32 is the right default.** It is below the pool, so the pool never binds, and the
  whole gain is already there. P4 is worse than P3 on both kernel lines (launch 8.823 against
  7.601, chain 4.596 against 3.832) for 1.8 times fewer submissions, which the measurement says
  costs more host time than it saves.
- **A cap above 64 needs the pool raised with it**, at 4096 bytes of host-visible memory an entry.
  Nothing yet needs that, and this evidence is why it is a separate decision and not a tuning knob.
- **The dword cost of a dispatch is still worth cutting**, because the host writes it and the
  command processor reads it on every dispatch. That is design section 8.8, measured off the
  hardware: 72 dwords become 23 for the shape a real launch has.

## The one line that got slower, and what is not known about it

`copy 4 KB from device` is 19.246 and 19.396 us in the two unbatched arms and 28.816, 28.853,
28.866 and 28.906 us in the four batched ones. The split is clean across six arms: about **9.5 us**
more per 4 KB device-to-host `hipMemcpy` with batching on.

What is established:

- It is not visible at 1 MiB (4874 to 4910 us, every arm) or at 64 MiB (360.1 to 360.4 ms, every
  arm), and host-to-device copies do not move (0.264 to 0.333 us at 4 KB in all six arms).
- The unbatched 4 KB rate (203.0 MiB/s) equals the unbatched 1 MiB rate (204.9 MiB/s), so the 4 KB
  line carries almost no fixed cost at all. The batched figure is 135 MiB/s. The difference
  is therefore a **read rate**, not a fixed host cost added to the call.
- The 512 waits of that line are answered by the fence mapping in both policies, so it is not a
  wait that became slow.
- The host path cannot hold it. A `hipMemcpy` of either direction runs the same code: wait for the
  device, look the allocation up, flush (which returns at once with no buffer open), and one
  `memcpy`. The only difference between the two directions is which way that `memcpy` goes. The
  host-to-device line of the same arms bounds that whole shared path under one microsecond and
  shows it does not move with the policy.

What is not established: what sets the read rate of the write-combining mapping over those 5 to
7 ms. One state of the machine is perfectly bimodal with it in this session's own logs, the GPU
clock after each arm (1000 MHz after both unbatched arms, 500 MHz after all four batched ones), and
it is **not** an explanation, because the 1 MiB and 64 MiB lines of the same arms would then differ
too and they do not. The arm that decides it is in `scratch/m16-hip/lab/perf-README.md`: the same
copy line with no kernel phase in front of it, in both policies.

Read as a trade, the cost is paid back at once: a batched dispatch saves 9 to 12 us of host time
each, so one copy's 9.5 us is repaid by the second kernel after it. The plan's clause "no slower on
anything else" is, read strictly, not met, and the defaults were set with that stated.

## What this says about a language model, honestly

400 kernels a token (12 a layer over 32 layers, plus about 10 for the sampling tail). The chain
line is the right cost, because a decode step's kernels depend on each other as its kernels do.

| Quantity | Build 1 | Cap 32, barrier light |
|---|---|---|
| submissions a token | 400 | 13 |
| off-GPU cost a token | 6224 us | 1533 us |
| tokens a second at that cost alone | 161 | 652 |

A 7-billion-parameter model on this hardware decodes far below 161 tokens a second for reasons of
memory bandwidth, so the fixed dispatch cost is not its first bottleneck. It is a tax of about
6.2 ms of one processor core a token, which batching cuts to 1.5 ms. Where it decides the result is
a small model, a long kernel chain, or a build that wants the processor for its own host side.

## What this session did not do

It ran one process at a time, so it says nothing about several processes sharing the device. It ran
no language model: the first inference of this route is a separate lab arm. Its copy lines are host
copies through the mapping, because that is what this build does. No copy engine was measured. And
the build it measured is the pre-merge perf build, as the first section says.

## Files

| File | What it is |
|---|---|
| `measurements.jsonl` | 42 JSON lines, one per measurement, each tagged with its arm |
| `arm-00-vadd-before.txt` to `arm-07-vadd-after.txt` | the console output of every arm, verbatim |
| `artifact-hashes.txt` | the sizes and SHA-256 of the three files, as the lab read them back |
| `state-00-before.txt`, `state-99-after.txt` | the installed driver state before and after |
| `kmd-before.txt`, `kmd-after.txt`, `kmd-final.txt` | the kernel driver log around the session |
