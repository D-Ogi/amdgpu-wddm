# M409 - active graphics pipeline overlay

2026-09-24, unit A. Deployed monitor SHA256
0627D56A08ACEC6138938E6E11B456E6C1CDA7EE406BD5396CBCD8BBFBC6F6CA.
C# build with warnings as errors and four existing stage-contract tests passed.
API independently reports DWM PID8364 since12:46:44 with profile UMD hash
E1976C425405: Mesa softpipe, CPU software rendering, TGSI without LLVM/JIT.
Live KMD reports full WDDM, hardware flip count1200, enabled CPU Blt/count3,
compute0/0 and SDMA1472/1472. Counts are cumulative, not current utilization.
No Windows/DWM restart: boot stays11:44:14 and DWM PID is unchanged.

First deployment encountered a still-open executable after Stop-Process.
Stopping the overlay task and waiting for process exit permitted replacement.
The final counter wording distinguishes enabled paths from actual work.
A full scanout screenshot was inspected locally; only the pipeline crop is
retained publicly to exclude network addresses. The API snapshot is in
`deploy-overlay-final.log`. Unknown builds are not given a CPU/GPU label.

Owner observed normal mouse motion while the overlay was stopped, then the
same roughly half-second fluid/stalled alternation after it returned. The
feature is installed; it does not resolve rendering latency. This supports
an overlay-update trigger but does not isolate the internal rendering cost.
The llvmpipe comparison remains separate pending work.
