# Run014: instrumented performance baseline

Same KMD0.7.57.1 and ICD9d3189..., official b9564, requested1000MHz820mV (SMU successful responses in clock.txt). Three benchmark repetitions per test. Stories15M pp51233338.78+/-711.05t/s,tg128345.74+/-49.55t/s. TinyLlama pp5121079.71+/-19.09t/s,tg12898.24+/-2.38t/s. LinuxM52at1000MHz899mV:1119.59and154.92t/s. TinyLlama Windows is3.56%lower prompt and36.59%lower generation. Instrumented UMD prints IB slices and every submission; KMD probes and logs every IB. These are hypotheses for host overhead, not yet measured attribution.

M8passes;10389hardware submitted/completed,0timeouts/refused/notrun,noTDR. JSON model/offload metadata retained. Display-only restored04:07:17,UnconfirmedStarts0. Eviction/paging not established by this benchmark.
