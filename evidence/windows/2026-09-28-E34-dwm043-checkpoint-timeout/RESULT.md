# M698 - DWM043 refused its first checkpoint acknowledgement

Exact166, UMD3484e1f4/B514FF61 and hosted ICD C0CE; runner08b4657.
Trial aborted after28.857496s with success=false and measured_seconds=0.
The first marker request timed out, so there is no accepted render interval,
final pixel control or G0 result from this attempt. Do not relabel the attempt
as a successful desktop run merely because the GPU DWM started.

The DWM log nevertheless contains marker1 at sequence1408, with318 begun and
318 ended stores. The strict prefix through this marker validates256 successful
maps,251 ended maps and10196 completed writer-span bytes. This is prefix evidence
only, not a complete lifetime census. The tail was interrupted during rollback.
Full raw receipts are retained in scratch/g0-hosted/dwm043-ops.

A334782-character prefix through that exact marker was replayed under PS5 on
the lab with CPU DWM, using a writer that waits for atomic marker publication.
The old character reader passed in1826.726ms; the block reader passed in421.432ms,
both under the unchanged4000ms deadline. Earlier controls used an unsynchronized
150ms writer delay and could append before the reader opened; those controls
are invalid and excluded. Neither valid replay reproduces the original timeout.
Thus the observed optimization provides headroom but does not establish the
cause of the failed DWM request. Commitc62fbf8 adds block reads and published-QPC/
processed-character diagnostics for the next run. Fragmented-line positive and
five negative protocol controls still pass on PS5.

Rollback succeeded. Run/watchdog/control tasks and original collector terminal;
tasks removed, CPU baseline8279/CF39/gates0 restored. Closure22:31:10Z Sep27:
exact166,health15,1000MHz/VID116,67.125C,same OS boot. No permanent promotion.
Only the numeric marker line and reduced derived result are exported; raw logs,
process metadata and archive remain local. No copy-free desktop claim is made.
