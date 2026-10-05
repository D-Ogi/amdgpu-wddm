# M380 - Current 119 inference benchmark

Measured on unit A in retained full-WDDM 0.7.119.1 session, Windows boot
2026-09-24T08:30:00. No restart or driver transition. Native workloads and host
SSH wrapper exited 0; final independent CLI info/confirm succeeded and retained
the initialized session. See run.log, final.log and comparison.json.

The executable, both model files and cache-intent-v2 ICD SHA256 values exactly
match M338/105 run.log. Reported benchmark non-result fields also match. Settings:
1000 MHz/820 mV, b9564, Vulkan, ngl99, t6, pp512/tg128, r3, shader cache disabled.

| Model | Prompt tokens/s | Generation tokens/s | Change vs 105 (prompt / generation) |
|---|---:|---:|---:|
| Stories15M | 48335.30 +/- 231.45 | 518.46 +/- 123.66 | -1.32% / -2.82% |
| TinyLlama | 1101.77 +/- 4.40 | 118.75 +/- 4.63 | +0.06% / +0.40% |

Three samples per measurement; observed differences do not establish a speedup.
Stories15M generation has particularly large dispersion. Historical Linux
TinyLlama 1119.59/154.92 tokens/s remains a reference with configuration caveats,
not a fresh matched comparison. Windows superiority is not achieved.

Before/after: GFX 2900/2900 -> 13265/13265; paging 80888/80888 ->
103353/103353; reserved captures 76 -> 101, heap 0 -> 0. No reported timeouts,
refusals or TDR. This is workload evidence, not full memory/lifecycle acceptance.
Raw per-model stdout/stderr/native exit files and before/after logs are retained.
Host run/final logs remove irrelevant hardware/interface identity lines and PCI
instance identifiers; benchmark output is unchanged.
