# E27 run016: quiet ICD validation and residency control

Unit A, boot2026-09-23 04:20:42. KMD0.7.57.1; quiet ICD SHA256
4e05f1df627cd6d9d64fe7f1adda29673d5b3b96b3750a08034a7ee88460c9ea.
llama.cpp b9564/3b3da01dc, requested1000MHz820mV (clock.txt).
Source repository HEAD bed764d plus uncommitted E27 changes; quiet change in
experiments/E27-m9-inference/mesa-quiet-submit.patch, Mesa MIT.

Saved-output validation: validate-016.py compares all eight texts directly to immutable
Linux E14 references with CRLF normalization only, checks full layer offload and candidate
loader/progress witnesses. M8 reports8tests/0mismatches. Final KMD19709submitted,
19709completed,0timeouts/refused/notrun,noTDR. See analysis.json and raw outputs.

- stories15M: pp512 34132.815875 +/- 786.032316, tg128 406.511222 +/- 1.183531 tokens/s.
- tinyllama: pp512 1090.041721 +/- 4.988073, tg128 103.058608 +/- 4.041902 tokens/s.

TinyLlama generation is4.90% higher than instrumented run014 (98.242479), but
33.48% below Linux M52 (154.92). Runs are sequential, on different boots, with three
samples each and no interleaved A/B control: do not attribute the whole change to
logging or treat this as a statistically established speedup. KMD diagnostics remain on.
The remaining performance gap is unexplained.

The original wrapper aborted during benchmark validation: positional Select-String
arguments treated the pattern as a path. Both benchmarks had already completed with
exit0; offline validation recovers these results without executing GPU work again.
Original wrapper and observer retained. Finally restored display-only mode,
restarted DWM, and confirmed UnconfirmedStarts=0 at04:28:06.

Both64KiB CPU residency probes exited1 after successful Evict and a query transition
from1(GPU memory) to2(shared memory). They incorrectly demanded3(NOTRESIDENT)
immediately. D3DKMTEvict queues removal and does not promise synchronous physical
page-out; ref/ddi-display/d3dkmthk.md and d3dukmdt.md, WDK26100 declarations,
original documentation commit7515063cea4c9e98db6a92986c5b4ddb0463fd16.
The failure does not establish a driver eviction defect. Neither1GiB control ran.
No BuildPagingBuffer transfer/fill or hardware paging submission occurred, so this
is not GPU paging validation. The revised CPU lifecycle probe remains untested on lab.
