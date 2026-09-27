# M609: native GFX row copies execute correctly in control002

Candidate14E4CA9D9DB2A835E2E232FE68849BB303E453BBEF09021D11EC71DFFF072B7A,
source fe543b4, links the production geometry/row-copy/DMA_DATA implementations.
Build /W4 /WX and help check passed. On diagnostic KMD153 SYS0C0DA4E1,
five requested heap combinations passed: small GTT/GTT, GTT/VRAM, VRAM/VRAM,
VRAM/GTT, and1920x1200 GTT/VRAM. Distinct pitches and nonzero origins are used.
The independent readback checks every word of each destination allocation,
including padding and untouched pixels: zero mismatches,30 residency receipts.
The largest case checks9,371,648 bytes including2,304,000 copied pixels and uses
1,200 row packets. Final monitored fence24 completes;24 SubmitCommand calls
succeed. Node0 summary delta24 submitted/24 completed matches these calls;
paging delta107/107, zero reported timeouts/refusals, no TDR callbacks reported.
The preexisting node0 cumulative submitted/completed gap remains568; the delta,
not equality of cumulative counters, is the witness for this bounded control.

All logged normal teardown calls succeeded. Live-object summary remains33.
Before/after snapshots and independent closure preserve boot and CPU DWM4596,
KMD153 flags15/epoch5, baseline UMD8279AC7F and ICD93B1D1FD. Closure05:41:20Z
measures1000MHz/VID116/66.375C, no native process/running worker and task removed.
This is not a blanket absence-of-leaks claim. No desktop switch or promotion.

Residency is tested independently of the requested heap label. All queries
return1, including GTT. KMD153's source restricts supported segment masks, but
this run does not independently inspect per-allocation physical placement or CPU
page-cache attributes. GPU PTE counters include both system segment0 and VRAM
segment1 changes; they are global counters and not a per-buffer placement map.
GPU coherent/noncoherent PTE fields do not measure the CPU Lock2 mapping policy.
The bounded copy control is not a cache-policy or performance comparison.

Original stdout and receipts are unmodified. Driver-log excerpts select the last
matching counter line from each UTF-16 private raw log; hashes of those raw logs
are retained. No identifying owner data was included. DDI allocation handling,
full-list prevalidation, MultipassOffset integration, producer synchronization,
CDD/DWM admission and full desktop G0 acceptance remain open. This proves actual
execution of the new copy primitive, not those remaining integration contracts.
