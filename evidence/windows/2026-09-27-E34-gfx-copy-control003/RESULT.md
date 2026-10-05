# M613: GPU dirty-list control003 passes

Candidate0821C9BDA9A2D72AEF7B8C3185D0ACB729F258A570BA26D3B3031E8DBA604963,
source4633abe, uses the production list adapter introduced in01e665e. All five
requested heap/size combinations pass in dirty-list mode on unchanged diagnostic
KMD153. The source/destination pitches differ and both origins are nonzero.
Three nonempty rectangles leave a one-pixel vertical stripe untouched; a fourth
rectangle has an empty intersection. The right side is split horizontally to
exercise rectangle transitions. Small IB capacities force multiple submissions.

Independent whole-allocation word comparisons find zero mismatches, including the
stripe, padding and regions outside the destination rectangle. The largest case
copies2,302,800 pixels through2,400 row packets while checking9,371,648 bytes.
All30 residency receipts pass. All34 SubmitCommand calls succeed and final
monitored fence34 completes. Latest node0 summaries advance153451/152666 to
153485/152700: delta34/34, preserving a preexisting cumulative gap785. Paging
advances79022/79022 to79128/79128 (delta106/106). No reported timeouts/refusals,
TDR callbacks or CollectDbgInfo calls. Object summaries retain33 live objects.

All logged teardown calls succeed. Before/after receipts preserve CPU DWM4596,
boot, KMD153 flags15 epoch5 and baseline UMD8279AC7F/ICD93B1D1FD. Independent
closure05:58:18Z confirms no native process/running worker, removes task003,
and measures1000MHz/VID116/66.75C. This does not prove absence of every leak.
The lab was then handed back for separate bounded presentation diagnostics.

Raw stdout/receipts are unchanged. Counter excerpts select the last matching
line from private UTF-16 driver logs; their hashes are in manifest.json. No owner
identifiers were included. As in M609, heap names denote requested BC2A heaps
constrained by KMD segment masks, not independent physical/cache measurements.
This run uses BC2S submission on KMD153. It does not execute the newly added
BGP1 Present consumer (M612), advertise interop, switch DWM to GPU, or prove
no CPU frame copies in the desktop path. Present producer integration and full
G0 acceptance remain open. Build /W4 /WX and help checks also passed.
