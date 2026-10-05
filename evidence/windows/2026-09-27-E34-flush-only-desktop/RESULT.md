# M589: flush-only desktop diagnostic

DWM019 ran185.783s on unit A with UMD91345C63 and hosted ICD3508416F,
the same mode2 diagnostic tested in M588 control109. The added per-draw
fence_finish is skipped; flush still occurs after each draw. Three mode2 log
witnesses and exact process module hashes confirm the selected path.

All sampled cyan-shape checks and8000 exact static composition pixels pass.
ETW attributes41943 matched DMA pairs to DWM9940, zero lost events/buffers,
no pending/unmatched/duplicate starts, matching submission/completion IDs,
no preemptions. No owner visual verdict was received when this record was made.
Sparse captures cannot exclude transient artifacts, especially the residual
exposed-area glitch reported during017. This is not full visual acceptance.

The passing samples without the added completion wait make batch boundaries
and state re-emission useful next isolation targets. They do not establish a
root cause: per-draw flush also changes submission timing. No claim that all
CPU waits are absent, that G0 is complete, or that performance is acceptable.
The additional flush is diagnostic and not promoted as a production solution.

Unlike018,019 persisted startup module inventories before the acceptance gate
and logged router selection. It started successfully with the unchanged UMD;
018's startup failure cause remains unknown. The instrumentation adds evidence,
not a proven fix for018.

Rollback verified baseline ICD93B1D1FD and CPU UMD8279AC7F; CPU DWM3040,
all019 tasks removed. Raw ETW/images/logs remain private, hashes retained here.
