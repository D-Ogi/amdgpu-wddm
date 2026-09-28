# DWM049 - full interval after clock admission correction

Hypothesis: KMD169 keeps VSync advancing during combined hosted GPU DWM/native GPU window rendering, including the interval that failed on166 in DWM047. Correct image alone cannot prove the VSync fix.

Derived from DWM048/34814a9, preserving KMD169/AF715A56, UMD92697AE5 and hosted ICDC0CE. Router/control binaries are rebuilt for the049 paths and checked against the build receipt. QueryInterruptTime uses the tested API-set import; a live clock admission runs before desktop mutation. Full Save-KmdVsyncWitness passed a read-only CPU control before this trial. The original048 package and failure evidence remain immutable.

Preflight requires confirmed CPU169/health15, exact artifacts, normal TDR settings, operating point1000MHz/820mV, temperature below85C and no competing workload. CPU169 is restored at the end;166 rollback package remains preserved.

Render105s, checkpoints130s, watchdog rollback140s, maximum trial180s. Tasks have180s caps. Pixel, native-client, module, audit-marker and ETW checks remain. VSync ACK rate must reach95 percent of the queried integer refresh, with final read-begin ACK/notify age below100ms, at most one percent deferred without old-buffer report, report coverage and unchanged error counters. Counter snapshots are non-atomic. DPC-recovered ACKs are evidence, not required positive.

TDR gates require clean latest summaries, zero CollectDbgInfo, same boot and no queried Display4101/WER117/141 events. WER can arrive asynchronously; the immediate empty query is not proof that no later report will appear. Offline ETW retirement, trace-loss and inter-VSync analysis remain required.

On failure preserve data and restore CPU; never repeat an existing trial identity. Success of this bounded test does not close CPU-copy, lifecycle or other outstanding M13/G0 criteria.

Result: not run.
