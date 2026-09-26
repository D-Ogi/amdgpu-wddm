# M382 - Measure unfinished capture pressure

Add owner current/peak unfinished-plan count and reserved arena bytes, with
adapter-lifetime maxima of each per-context peak. Update on attach/detach/drain;
continuations must not increment counts, reuse must not inflate peaks. Preserve
existing reservation/heap totals. Host extracted builder tests must distinguish
two simultaneous plans from sequential reuse and retain maxima after owner drain.
Build with WDK. No deployment or restart in this step; these measurements will
inform resource architecture, not prove a universal concurrency bound.

Reference review: Microsoft System Paging Process (1 GiB, chunked transfers) and
Threading and Synchronization Level One (nonreentrant function class), retrieved
2026-09-24. Neither supplies a numeric bound on interleaved unfinished operations.
https://learn.microsoft.com/en-us/windows-hardware/drivers/display/system-paging-process
https://learn.microsoft.com/en-us/windows-hardware/drivers/display/threading-and-synchronization-first-level
