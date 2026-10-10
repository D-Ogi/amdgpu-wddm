# Correction, 2026-10-10: the 76 % is a modelled setting delta, not measured RT pass time

`evidence/` is immutable (`docs/01-evidence-rules.md`: "Never edit afterwards. A correction is
a new file."), so `RESULT.md` and every raw file next to it stay as the series wrote them. This
file names what the write-up claims more strongly than it measured. The independent audit of
2026-10-10 raised it as finding A19, with its appendix `perf-facts.md` finding PF1. The fact row
[M842](../../../docs/facts/games.md#m842) carries the corrected wording.

## What stands

Every measured number stands. The present rates, the frame-interval medians, p95, p99 and max, the
clock, voltage, Tctl, GRBM busy, SMU power and plug watts are what the instruments recorded. An
independent replay of the raw `474` ETL reproduced the four in-session rates (13.20, 12.50, 44.02
and 9.96 frames/s over 50 s windows) and the fitted elasticities of the 5 s slices (0.771, 0.754,
0.738). The clock-scaling fit and the normalised table are arithmetically reproducible.

## What is corrected

Three sentences read as measured time inside the ray-tracing passes. They are differences between
settings, scaled by a fitted clock model.

| Section | The write-up says | What the number is |
|---|---|---|
| Cost of each RT effect | "RT costs 76 % of the all-four frame (58 of 76 ms)" | `(76.4 - 18.1) / 76.4` of two frame times that `rate x (1.5 / GHz)^0.75` produced. An estimated incremental cost of turning the RT setting on, which holds the shader, BVH and denoising work and any other rendering change the setting makes |
| Cost of each RT effect | "GI, reflections and shadows cost about the same, 17 to 18 ms each. AO costs 6 to 9 ms" | Deltas between settings measured in different sessions, under the same model. No per-pass GPU timestamp was taken |
| Next steps 1 | "GI, reflections and shadows are three traced passes of 17 to 18 ms each at 1.5 GHz" | The same deltas. The passes were not timed; the sentence that follows it, which asks for per-dispatch timing, is the measurement that would time them |
| RADV knobs | "cswave32 is not a lever for this frame" | One session under this normalisation found no advantage it can measure. Not a general null result for the knob |

The 0.75 exponent is fitted to 5 s slices of the same observational data, under a thermal governor
that moved the clock between 1100 and 1500 MHz. There was no fixed-clock arm, so the exponent is a
description of this series and not a separate measurement. The "Levers" table already says that its
two "about" rows are sums of measured steps and not measured frames; that limitation is correct and
is the same class as the rows above.

## What would test the attribution

A fixed-clock repeated ABBA arm, or GPU timestamps per dispatch or per command buffer (the
measurement that Next steps 1 already asks for). Neither is needed to correct the wording, and this
file asks for no new lab session.

## Also noted by the same audit

`plan-475.md:8` scheduled `RADV_PERFTEST=rtwave64` as a measurement arm while naming BD-074, the
ray-query CTS case that returns a wrong value under wave64, and trial 251 had already bugchecked
with the knob. The attempt was refused by the stale-marker admission gate before any GPU work, and
`RESULT.md` ends with the right rule: the BD-074 CTS cases come first. The correctness check
belonged in the arm selection, not in the admission gate. 475 is not a performance sample.
