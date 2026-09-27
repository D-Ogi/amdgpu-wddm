# Bounded GFX row-copy construction

Bc250EmitGfxBlt connects the M604 geometry plan to the M605 DMA_DATA emitter.
Each call produces as many packets as the supplied command-buffer capacity can
hold. An internal Row/ByteInRow cursor resumes within a row wider than the packet
limit, not just between rectangles. Partial output is an explicit More result;
less than one packet of capacity returns NoSpace without advancing the cursor.

The builder validates the complete rectangle address footprint before writing
any output, checks64-bit base/offset/extent arithmetic and rejects overlapping
bounding spans. The allocation mapper must still reject backing-store aliasing
through distinct virtual addresses. Pitch, row size and offsets are4-byte aligned.
Empty plans return Done without touching a command buffer. Caller storage must
be disjoint except that input/output cursors may share storage.

The copy-span emitter permits first/last flags so each nonempty batch has one
first RAW_WAIT and one final CP_SYNC. Individual rows in the same batch have no
additional wait. Standalone Bc250EmitGfxCopy retains its previous first/last
behavior. Producer cache ordering and hardware/WDDM fence completion are still
caller responsibilities.

This is an internal cursor, not a completed DxgkDdiPresent MultipassOffset
implementation. Integration must validate every rectangle before submitting the
first partial present, retain allocation lifetime/residency, adapt the DDI's
multipass offset, and submit/retire on the presenting GFX node. No interop cap
has been enabled and this builder has not been submitted on hardware.

Tests decode the emitted packets and apply their address/count fields to source
and sentinel-filled destination byte arrays. Across30 buffer capacities the
result matches an independent absolute-coordinate pixel oracle, including row
padding and untouched pixels. A two-row case wider than the maximum packet tests
four resumptions inside/across rows. Rejected whole-range wraparound/overlap,
invalid cursor/pitch, empty input and insufficient capacity preserve output.
Host and kernel-flag compilation pass; gfx-blt is an enforced quick quality gate.
