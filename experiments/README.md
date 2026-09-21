# Experiments

One directory per experiment, numbered in the order they were planned. The README is written before the run (hypothesis, procedure, expected results) and completed after it (result, evidence paths, changes to `docs/facts.md`). Template and rules: `docs/01-evidence-rules.md`.

| # | Title | State |
|---|---|---|
| E01 | Linux baseline of our unit with the diagnostic USB | run once on unit A, 2026-09-21 (tool commit `ffa749e`, probes id `79152824`). Tool: `tools/diagusb` (record its commit and the `probes.json` id with the ... |
| E02 | Control read of the same registers under Windows | run 001 done on 2026-09-21. Evidence: `evidence/windows/2026-09-21-E02-run-001/`. |
| E05 | a display-only WDDM driver owns the GPU and keeps the firmware's display | run 001 done, H1-H4 hold (2026-09-21). Evidence: `evidence/windows/2026-09-21-E05-run-001/`. |
| E06 | first load of our own miniport (bc250kmd, milestone M3) | run 001 done, H1-H4 hold, H5 answered: yes (2026-09-21). Evidence: `evidence/windows/2026-09-21-E06-run-001/`. |
| E07 | first register write under Windows, through our own miniport | run 001 done, H1-H5 hold (2026-09-21). Evidence: `evidence/windows/2026-09-21-E07-run-001/`. |
| E08 | the VRAM carve-out is reachable by system physical address under Windows | run 001 done, H1-H6 hold, one finding about BAR0 (2026-09-21). Evidence: `evidence/windows/2026-09-21-E08-run-001/`. |
| E09 | GART and VM context 0 under Windows, with AMD's own code (milestone M4) | run 001 done: H1, H2, H3, H5 hold, H4 holds with a residue (2026-09-21). Milestone M4 reached. |
| E10 | GPU firmware through the PSP under Windows (milestone M5, first part) | run 001 done on unit A, 2026-09-21. The PSP takes the firmware under Windows. H1, H2, H3, H6, H7 hold; H4 |
| E11 | RLC, CP, KIQ, queues, ring tests and SDMA under Windows (milestone M5, second part) | run 001 done on unit A, 2026-09-21. M5's criterion is met: PM4 packets written by the CPU into rings in |
| E12 | interrupts under Windows: the IH ring, the first interrupt, fences (milestone M6) | part A run (2026-09-21, run 001): all four hold. Parts B and C run (run 002): the IH ring and fences work, a second bring-up failed (M44; fixed in E15). |
| E13 | second Linux reference session on unit A (wishlist L3, L4, L5, L7, L8, L9, L10, L11, L13, L15; L6 last) | run on 2026-09-21, three boots. H1 to H4 hold; one step hung the machine (facts M40, M41, M42). |
| E14 | Vulkan compute reference (Linux/RADV today, Windows/M8 later) | run on 2026-09-21 on unit A under the Alpine diagnostic stick. H1 holds for all eight |
| E15 | a compute dispatch under Windows, the SDMA ring test, a second bring-up (closes milestone M6) | run 001 done (2026-09-21): the dispatch works, the second bring-up works, one new defect (SDMA after a |

The State column is each README's own `State:` line, shortened; the README is the authority.
