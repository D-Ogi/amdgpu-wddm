E24 run 005, 2026-09-22 11:38-11:42: bc250kmd 0.7.26 (4042b0e) on unit A, owner at the monitor, first bring-up
of the 11:29:35 boot (the freshness guard of M78 passed).

The run that run 004 was supposed to be, on a driver that survives it: the same eight stages with the node and
the flip gates open, the same `fence gfx x2` control for the IH ring, then `kmtprobe` holding 64, then 256, then
512 MB - three steps instead of one, so the first SDMA packet VidMm ever asked us for would be built under a
counter read rather than in the middle of half a gigabyte.

What happened: all four fills VidMm sent were answered (M107). 64 MB at physical 0x271C62000 in 80 dwords, then
256 MB there twice in 320 dwords each, then 256 MB at 0x281C62000 - 872,415,232 bytes in all, no transfers, no
insufficient-buffer, no refusal for any of the five unsupported reasons. The 64 MB fill's destination is the
address the run 004 crash dump carried in r8, which is as direct a confirmation as this experiment can give that
the two runs met the same allocation and that the only thing between them was 0x5B00 bytes of stack. Nothing
bugchecked, nothing TDR'd, the undo was clean and the unit stayed between 67 and 72 C.

What did not happen: none of it ran. `DxgkDdiSubmitCommand` was never called - not once in 120 s, it is absent
from the DDI tally - and node 1 ends the run at `0 hardware submitted, 0 completed, 0 timeouts, 0 refused`
(M108). dxgkrnl asked us to build four paging buffers and then submitted none of them, so the packets sat in the
shadow buffer and the hardware never saw a byte. H1 is confirmed and H2's build half with it; H2's hardware half
and H4 are now waiting on a question in dxgkrnl's court, not ours.

The IH ring's second measurement is here too: 3122 interrupts, 3550 DPCs, 3123 vectors, 0 overflows, and the
vectors printed are `client 4 source 87` again (M106 at n = 2).

Files: `run-005-console.txt` (the whole console, tee'd to disk from the first line this time - run 004 taught us
that), `run-005-ring.log` (the driver's own 509-line ring log, which is where the four fills and the counters
are), `run-005-script.sh`, `run-005-scanout-half.png` (what HUBP0 was scanning out near the end: the unpainted
primary of M100 again, the picture the owner sees while the flip gate is open - this run did not need that gate
and should not have opened it).
