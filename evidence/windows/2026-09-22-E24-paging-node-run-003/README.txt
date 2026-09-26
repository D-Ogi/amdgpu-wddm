E24 run 003, 2026-09-22 10:57-11:01: bc250kmd 0.7.25 (3910bed) on unit A, owner at the monitor, FIRST bring-up of
a fresh boot (10:53:53), with the freshness guard (scratch/tmp/bringup_guard.ps1) refusing otherwise.

One run answering both open questions, cheap answers first: the full table with `-GpuVa 1 -Engines 1 -GpuSubmit 1
-Blit 1 -PagingNode 1 -VidPnFlip 1`; a DCN dump right after the start and again after 40 s of desktop with no
engine up; then gart, psp and ih; a dump again; then `gfx run 1` to `gfx run 8`, one escape call per stage; 40 s of
desktop with the paging node live; a dump, the counters, `ih state`, a scanout read-back; then the full undo.

Result: all eight stages returned (CP 346 us, SDMA 109 us, interrupt sources 23 us), no TDR, no bugcheck, undo
clean, 67 to 73 C. dxgkrnl asked for node 1's metadata and created a context on it; VidMm issued 1079 paging
operations of kind 11, 8 of kind 12 and 2 of kind 9, and no VIRTUAL_TRANSFER or VIRTUAL_FILL at all, so node 1 did
no hardware work - an idle desktop gives VidMm nothing to move. The vupdate event bit was SET in every dump while
the IH ring stayed empty; this run submitted no GPU work either, so the ring was never proven live in the same
device start (the next run has to fire a CP fence interrupt as the control).

run-003-console.txt is the unedited console, run-003-script.sh the script, run-003-scanout-half.png what HUBP0 was
scanning out near the end (the unpainted primary of fact M100).
