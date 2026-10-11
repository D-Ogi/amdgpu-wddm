# Where a token goes on our HIP runtime: dispatch counts, and the OS packet time inside the token

Date: 2026-10-11, unit A, Windows 11 Pro, boot 05:33:03.5Z. Seven supervised benchmark arms
between 06:46Z and 06:48Z. Two models, two backends, and one arm captured with ETW. The question
is what a decode token costs on our own HIP runtime, and how much of that cost the OS can account
for.

Three results come out of this directory:

1. **The counts.** One Gemma decode token issues 1144 HIP dispatches in 36 submissions, with 77
   wait calls, of which 42 are fast. Both Gemma profile arms report the same counts on every
   measured token.
2. **The rates.** The ordinary HIP benchmark takes 31.727623 ms a token. The Vulkan reference on
   the same model and the same command takes 25.247145 ms a token. The difference is
   6.480479 ms a token.
3. **The split.** In the ETW arm, OS DMA packets occupy a mean 25.596266 ms of a mean
   31.077735 ms token interval. A mean 5.481470 ms of the interval is outside those packets.

What this directory does **not** hold: the duration of one wait, the duration of one copy, the
execution time of one shader, and the total bytes copied per direction. Those fields are
unmeasured, and the ranked plan for them is in the follow-up section.

## The stack under test

| Part | Identity (SHA-256) |
|---|---|
| Kernel driver `bc250kmd.sys` | `8A873F3204C3F519477DAB39F5C33209023773EE296EF564617FC690EE7E565B` |
| HIP runtime `amdhip64.dll` | `B0FEB14BD34797DFBB866B504C13859AD0BD15935C62D3126AF019CA3311F106`, installed and pinned beside the candidate |
| `ggml-hip.dll`, FlashAttention enabled | `FC5B2D6586B5C402E646FD2F64CB6CE97CB863B4FB3AE68F361DEF1DF799C814` |
| Profiling benchmark `llama-bench-profile.exe` | `49E7E78D6A142F63012F8520531E7F14A8BE271B7BEB602491D16DF9406CA02E` |
| Ordinary benchmark `llama-bench.exe` | `CEEF618B217059BD0AB7CBF8C73C468D31219B0C3291F1AA2ECB2A97ACFE4771` |
| Vulkan reference `ggml-vulkan.dll` | `F8AED9FB4C978510462709891CE2A1218FDF2E3CDAD65421D71F76D4A235BD04` |
| Gemma model | `93567E57A8FE10B23569B9D9EC38CD005DEEDF71E29477C421A4B83F418A538B` |

Every arm ran the same command, with the model as its only difference:
`-ub 512 -b 512 -p 0 -n 128 -ngl 99 -fa on -r 1 -t 6 -o json`. The child environment was fixed at
`BC250_HIP_LOG=0;BC250_HIP_WAIT_TOTAL_MS=10000;GGML_SCHED_DEBUG=0`. Every arm had a 170 s
supervisor deadline and a 125 s workload cutoff. The clock policy stayed automatic: DPM 1, a
2000 MHz ceiling, a 500 MHz idle state, 40 compute units, the fan curve controlling, and a UMA
carve-out of 8192 MiB. The full pins of each arm are in its `arm.json`.

This is a comparison of the installed stack as it is. It is not a same-source backend
microbenchmark: the HIP side is a `b86d2f07`-derived consumer and the Vulkan side is the existing
`b11539` reference, so compiler and library revisions differ between the two columns.

## The rates

`ms_per_token` is `avg_ns / 128` of the benchmark's own `tg128` result. Every supervisor verdict
is PASS, with the process stopped and its scheduled task removed.

| arm | backend | model | tokens a second | ms a token | trial seconds | max sampled Tctl |
|---|---|---|---|---|---|---|
| `control-gemma-hip` | HIP, ordinary | Gemma 12B Q4_0 | 31.518276 | **31.727623** | 63.971 | 75.4 C |
| `profile-gemma-hip` | HIP, instrumented | Gemma 12B Q4_0 | 30.442449 | 32.848869 | 65.562 | 73.0 C |
| `trace-gemma-hip` | HIP, instrumented, ETW | Gemma 12B Q4_0 | 31.312147 | 31.936487 | 67.262 | 65.6 C |
| `profile-gemma-vulkan` | Vulkan | Gemma 12B Q4_0 | 39.608440 | **25.247145** | 61.512 | 77.8 C |
| `control-tiny-hip` | HIP, ordinary | TinyLlama 1.1B Q4_0 | 177.506390 | 5.633600 | 15.855 | 62.6 C |
| `profile-tiny-hip` | HIP, instrumented | TinyLlama 1.1B Q4_0 | 136.945587 | 7.302170 | 17.044 | 65.1 C |
| `profile-tiny-vulkan` | Vulkan | TinyLlama 1.1B Q4_0 | 305.011070 | 3.278570 | 15.852 | 61.8 C |

The instrumented benchmark is slower than its ordinary control, by 1.121246 ms a token on Gemma
and by 1.668570 ms a token on TinyLlama. Use the ordinary control for a throughput figure, and the
instrumented arm only for counts. Profiling output is written outside the per-decode timer and
inside the whole benchmark, and the counter reader itself can affect scheduling. One repetition an
arm does not support a claim about a small difference.

Total llama process time over the seven arms is 100.257 s, including model loading. Total
supervisor time is 307.058 s. Both are inside the 600 s series limit.

## The counts

`decode-profile.h` brackets the decode call and its existing synchronization with QPC, and reads
the runtime's counters before and after. Each profile arm retains 129 generation events, excludes
the first warmup event, and requires 128 one-token events with valid monotonic counters. The raw
events, one JSON object a line, are in each profile arm's `stderr.txt`.

| per-token quantity | Gemma, FA on | TinyLlama, FA on |
|---|---|---|
| HIP dispatches | 1144 | 424 |
| submissions, which are batches here | 36 | 14 |
| dispatches a submission | 31.777778 | 30.285714 |
| wait calls, mean | 77 | 42.953125 |
| fast waits | 42 | 30 |
| non-fast waits, the difference | 35 | 12.953125 |
| decode plus existing synchronization, mean | 32.186359 ms | 6.839764 ms |

Over the 128 measured Gemma tokens that is 146432 dispatches, 4608 submissions, 9856 wait calls
and 5376 fast waits. Every dispatch was batched. The TinyLlama wait count varies between 40 and
43, so its non-fast count varies between 10 and 13. The two Gemma profile arms agree on every
count.

Read these counts for what they are:

- A counted dispatch is one call of the runtime, not a distinct shader program.
- The fast and non-fast split is a classification of a fence read, not a measured CPU sleep.
  `bc250hsa/submit.c` flushes the pending batch before it tests the mapped fence.
- Kernel count, submission count and wait count are three different quantities. Do not multiply 35
  by a guessed wait latency, and do not add wait time to GPU work that overlaps it.

## The split inside a token interval

The ETW arm `trace-gemma-hip` ran the game collector in its GPU-only mode. The capture stopped
with the process after 23.486 s, and it covers initialization and the whole measured generation.
The parser joins packets by context and sequence and keeps only the workload process. The clock
join of the ETL to the profile's QPC has 323 calibration points, 10 MHz, and zero residual ticks.
The trace reports zero lost events and zero lost buffers. There are 4656 process DMA packets on
node 0, with zero missing stops and zero missing completion interrupts. All 128 token windows hold
36 DMA starts, which is the submission count the counters report.

| mean a token | ms | what it is |
|---|---|---|
| decode plus existing synchronization | 31.077735 | the measured QPC interval |
| DMA start to stop, union | 25.596266 | measured OS packet residence |
| DMA start to completion interrupt, union | 25.588570 | measured OS notification interval |
| the interval outside DMA start to stop | 5.481470 | the derived set complement |
| queue start to DMA start, union | 0.454258 | a measured queued interval, which overlaps the rows above |

The packet union is 25.596266 ms of the 31.077735 ms interval, which is 82.4 percent of it. That
union includes execution and driver and scheduler notification effects. It is not a GPU timestamp
around each shader, and it does not say which kernel dominates. The time outside the union is not
necessarily active CPU time: it can hold copies, dispatch preparation, scheduler delay and
waiting. The queued row overlaps the other classes and must never be added to them.

## The copy calibration, and what it does not prove

A separate hipbench sample on the same runtime measures one 1 MiB device-to-host copy at
**4.880850 ms**, and the host-to-device direction at 0.039700 ms. Both warmed copies had fast
waits only. That measurement is
[`evidence/m16/hip-device-contract-2026-10-11/hipbench-stdout.txt`](../../m16/hip-device-contract-2026-10-11/hipbench-stdout.txt),
not a file of this directory.

Gemma's vocabulary of 262144 entries times 4 bytes is 1048576 bytes, which is 1 MiB of logits for
each requested output token. The benchmark requests logits although it chooses the next token
synthetically. There are other transfers as well: a one-token input is 4 bytes, and positions,
masks and KV metadata are not counted in that figure. This client counted neither the total bytes
a direction nor the time its own copy calls took.

So the calibration is a current-runtime single-copy number, and the arithmetic below is an
estimate and nothing more:

- 4.880850 ms is 75.3 percent of the 6.480479 ms gap between the ordinary HIP and Vulkan arms, and
  15.4 percent of the 31.727623 ms HIP token.
- The HIP token time minus that calibration leaves 26.846773 ms. That residual holds compute,
  submission, synchronization and other work. It is not an isolated host cost.
- The calibration is of the same scale as much of the 5.481470 ms outside the DMA packets. The two
  were not measured as the same operation, so subtracting one from the other proves no
  attribution.

The runtime today performs `hipMemcpyAsync` after a synchronous stream wait. A device-to-device
path can also flush a batch and copy on the CPU. A GPU-enqueued device-to-device copy can
therefore remove a forced drain as well as a memory copy, while a copy kernel behind the same host
wait would not remove that drain. Eligibility depends on the actual type, layout and strides.
Ordinary KV updates emit `ggml_set_rows`, and they are no proof that these paths explain any of
the 35 non-fast waits.

## What enqueue microbenchmarks do and do not price

Merged earlier evidence gives 3.411 us for one dependent dispatch in a 1000-dispatch enqueue loop,
and 7.305 us for one dispatch in an empty enqueue loop. Scaled by 1144 dispatches those give
3.902184 ms and 8.356920 ms. Those products are sensitivity calculations, not measured CPU
overhead for Gemma: the loops hold their own backpressure, the runtime of that evidence differs
from the one measured here, and a retired-only `hipStreamSynchronize` of 0.063 us in the current
microbenchmark cannot price 35 non-fast waits. A small-count cold chain is not interchangeable
with a 1000-dispatch sample either.

## The health of the unit

Seven arms of seven passed. The longest trial was 67.262 s. The final gate reports faults 0, fence
timeouts 0, reset engines 0, hang recoveries 0, Display event 4101 count 0, bugchecks 0, and the
adapter healthy. The final health flags are 15. No TDR and no hardware fence timeout. Fault counts
are a scan of the retained log ring, so they are not an unlimited history. No workload, no owned
task and no ETW session was left behind. The carve-out read after the arms is `0x200000000` bytes,
which is 8192 MiB.

The retained ETL is 138412032 bytes, SHA-256
`0988F908FE4B84DB0DD88718CE335AD232A7A1A8EA78E97402B3F7D6AE1DFC95`. It is not in this directory,
because this repository takes no binary of that size. Its remote original and its archive were
removed only after the local extraction and the hash check, and the receipt of that removal is
`trace-cleanup.json`.

## What this does not establish

1. **Shader execution time is unmeasured.** OS packet residence is not a GPU timestamp.
2. **The duration of one wait is unmeasured.** Only counts and a fence-read classification exist.
3. **Total copy bytes a direction are unmeasured.** The logits size is derived from the model and
   the source, not counted at run time.
4. **CPU active time against blocked time is unmeasured.** This trace is GPU-only.
5. **One repetition an arm.** No arm was repeated, and the two backends differ in revision as well
   as in API.
6. **No output correctness claim.** These are synthetic token streams with no prompt. Numerical
   acceptance of Gemma on HIP is a separate gate, and this directory does not touch it.
7. **The HIP dispatch journal cannot fill the missing fields.** The shim header states that it has
   no read API, the kernel driver stores batch lifecycle timestamps in a private dump layout, and
   the command-line `journal` reads the paging journal. Neither per-copy boundaries nor a complete
   per-token HIP event stream comes out of it.
8. **The effective context allocation is not exported.** The source requests a context of 128, and
   neither output prints the rounded allocation that follows.

## The measurement that would close the gap

Ranked, each under the same 170 s supervisor and 125 s workload cutoff:

1. **A Gemma GPU-copy off and on A/B with timed counters.** The same instrumented binary in both
   arms, `BC250_HIP_GPU_COPY=0` against 1, plus an ordinary throughput control. It must report the
   bytes, the route and the QPC time of each copy, the time of fast and non-fast waits, and the
   time of the submission call. State whether a nested wait inside a copy is inclusive or
   exclusive, or the two will be counted twice.
2. **The same packet-time capture on the Vulkan arm.** It separates a backend compute difference
   from the host-side interval already observed. It needs a Vulkan client that exports token
   boundaries. Whole-run averages must not be turned into token timestamps.
3. **Batch cap 16 against 32**, and only when the timed counters report an important submission
   or backpressure component. The current 31.777778 dispatches a submission nearly fills a cap of 32,
   and earlier evidence found a cap of 64 slower in a microbenchmark.

## Files

| Path | What it is |
|---|---|
| `summary.json` | every arm: the benchmark JSON, the derived `ms_per_token`, the profile means, the supervisor verdict and the thermal samples |
| `identity.json` | the state of the unit before the arms: driver health, the clock policy and the installed pins |
| `final-health.json`, `final-gate.json` | the health and the fault gate after the last arm |
| `trace-cleanup.json` | the receipt of the removal of the verified remote ETL and its archive |
| `runs/<arm>/arm.json` | the command, the deadlines and every pin of one arm |
| `runs/<arm>/stdout.txt` | the benchmark result, as JSON |
| `runs/<arm>/stderr.txt` | the per-token profile events of an instrumented arm, one JSON object a line |
| `runs/<arm>/worker-result.json` | the process identity, the exit code and the QPC bounds of the workload |
| `runs/<arm>/dpm-before.txt`, `dpm-after.txt`, `health-*.txt` | the clock policy and the driver health around each arm |
| `etw-analysis/result.json` | the clock join, the packet counts and the per-token intervals of the ETW arm |
| `etw-analysis/tracestats.txt` | the trace statistics, with the lost-event counts |
| `analyzers/analyze.py` | the profile summarizer, which reads the `stderr.txt` events |
| `analyzers/analyze_etw.py` | the ETW analyzer, which produced `etw-analysis/result.json` |
| `sha256.txt` | the hash of every file here |

## How to repeat

1. Stage the candidate and the Vulkan reference on the lab, under their pinned hashes.
2. Run each arm under the supervisor, with the deadline and the workload cutoff above.
3. Read `ms_per_token` from the ordinary control arms, never from an instrumented arm.
4. Read the counts from the `stderr.txt` of an instrumented arm, through `analyzers/analyze.py`.
5. For the packet split, capture the GPU-only ETW session on one instrumented arm, then join it
   with `analyzers/analyze_etw.py`. Check the lost-event counts before you read the result.
