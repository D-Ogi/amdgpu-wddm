# M611: dirty-list multipass command construction

Bc250EmitGfxBltList validates the complete rectangle list and GPU-VA allocation
spans before emitting any command. A UINT packet ordinal resumes within a row,
between rows and between rectangles, without retaining kernel state. Count0
means the complete destination rectangle. Empty intersections consume no packets.
Lists exceeding UINT packets, bad geometry, overlapping allocation VA spans,
address wrap and invalid offsets are rejected before command-buffer writes.
Physical backing-store non-aliasing remains a caller obligation.

The existing mandatory gfx-blt suite now tests dirty-list pixel copies at30
command capacities, including overlapping dirty rectangles and an empty
intersection. Independent expected pixels verify translation, untouched regions
and padding. Canaries check output bounds. A bad final rectangle is rejected
without output both at offset0 and resumed offsets. Further controls cover
no-space, terminal/out-of-range offsets, four-packet wide-row resume and a total
packet count exceeding UINT. Host tests and kernel-flag compilation pass /W4 /WX.
The existing single-plan regression passes in the same invocation.

Intermediate rectangle boundaries conservatively retain CP_SYNC/RAW_WAIT pairs;
no performance claim. The adapter emits unpadded command words. WDDM integration
must still reserve/pad IB alignment, validate typed allocation entries and formats,
translate its result to NTSTATUS, retain allocation lifetime/residency, establish
producer ordering, and route Present submission/completion through node0. No
WDDM callback calls this adapter yet; no KMD deployment, capability advertisement,
GPU execution of the list adapter or new desktop trial occurred. The M609 hardware
control covers the underlying single-plan emitter, not this newly added wrapper.
G0 remains open. Lab slot belongs to the separate cached-presentation controls.
