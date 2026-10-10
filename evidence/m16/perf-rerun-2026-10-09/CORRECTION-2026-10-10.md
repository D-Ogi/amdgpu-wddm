# Correction, 2026-10-10: what the timed loop measures, three percentages, and one overlap claim

`docs/01-evidence-rules.md:56` says: "Never edit afterwards. A correction is a new file." This is
that file. `README.md`, `measurements.jsonl` and every raw file of this directory stay as the
session wrote them. Every number they hold stands. What changes is what the numbers are called.

## 1. The two kernel figures are enqueue-loop wall time, not processor-only cost

`hipbench` times the loop of `hipLaunchKernel` calls (`samples/hipbench.hip`). A launch inside
that loop can wait: for a free entry of the 64-buffer kernel argument pool
(`runtime/hip_launch.cpp`) and for a free command slot of the ring (`include/bc250hsa.h` section
8, "may wait for a free ring slot"). The kept raw files show those waits happened, not merely that
they could: R1 launch has 997 waits and R3 chain 31, with zero fast waits in both.

So the figure is the cost the caller sees for a dispatch before it waits for a result:
enqueue-loop wall time with backpressure. The following readings in `README.md` go beyond it:

- "us a dispatch" read as host or processor time (lines 139 to 153),
- "The cache buys host time here" (line 153) and "the submissions per dispatch and the end-to-end
  time move together with the host time" (line 149): when GPU progress controls the loop's waits,
  the end-to-end time moving with the figure cannot tell a host write from a device read,
- "the host path of build 1 caps decode at about 172 tokens a second before any shader runs"
  (line 201): the 400 x L arithmetic is an illustration of the scale, not a measured processor
  budget, and it is not a measurement of a TinyLlama bottleneck.

What would establish the attribution is one bounded arm per variant that records the thread's
active time and its blocked time separately and correlates GPU fence intervals under the same
workload. No such arm has run. The audit of 2026-10-10 states this as finding HIP-F3. Facts M853,
M854, M862 and M863 now carry the narrower wording.

## 2. Three percentages use the wrong denominator

- The state cache saves 0.836 us of 4.641 us on the chain line, which is **18.0 %** of what the
  dispatch costs without the cache. The 22.0 % of lines 140 and 145 is 0.836 / 3.805, the overhead
  relative to the cached cost, not the fraction removed from the original total.
- On the launch line it saves 0.418 us of 8.585 us, which is **4.9 %**. The 5.1 % of lines 139 and
  146 again uses the cached denominator.
- The 0.112 us between the two policies on the 4 KB device-to-host line is **37.7 %** of the
  0.297 us spread of that line under one policy (19.301 against 19.598 us), not "a quarter"
  (line 168).

## 3. Independent launches do not overlap on this path

Line 186, "the GPU may overlap them", does not hold for this submission path: every PM4 dispatch
is followed by an unconditional `CS_PARTIAL_FLUSH`, and one node-0 context submits one indirect
buffer at a time (`bc250hsa.h` rule 5). That serialization is what BD-111 and fact M861 record.
Host threads overlap their waiting. The device's compute does not overlap here.

## 4. What this directory does witness

All 49 rows of `measurements.jsonl` match the child JSON output of the arms exactly, and the 42
rows of the first set match theirs. The submitted and completed counter deltas, the `exact yes`
results and the state comparison before and after the session are in the kept files. The
per-arm exits, wall times, thermal ranges and plug samples are not: see the same note in
`../step3b-2026-10-09/CORRECTION-2026-10-10.md`, which applies to this set as well.
