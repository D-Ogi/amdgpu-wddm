# GLX context-switch control (M519)

Same Linux setup and artifacts as M517. Full006 is a disjoint continuation
of full003: one pass and one fail, stopping at glx@glx-make-current.
Zink reads black instead of the expected colors in window2. Native RadeonSI
control with the same executable and arguments also fails, for windows0/1/2.
The detailed outcomes differ; this is not identical-output parity or a pass.
The helper creates all three windows at(0,0); any role of overlap/visibility
remains a hypothesis. No new driver fix is claimed.

This GLX-specific case has no Windows counterpart. Both unmodified outcomes
remain recorded. Reviewed continuation covers the rest of the exact Linux
inventory:45159 total,1005 retained,44154 remaining. Earlier warn/fail results
are not removed or rewritten. Multi-segment merging rejects duplicate cases.
Six host tests pass for remainder preparation, including retained warn/fail,
duplicate segments, forbidden timeout/crash continuation and literal# names.

Full007 is started with the same graphics artifacts and unchanged oracles;
its final result is not part of this checkpoint. Windows/Linux full comparison,
including shared cases and platform differences, remains incomplete.
