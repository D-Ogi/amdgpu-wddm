# Exact Present packet content control

Hypothesis: the production Bc250EmitGfxPresentBltList packet sequence, including
ACQUIRE_MEM before each pass and full NOP padding, copies the intended pixels on
exact KMD164 while preserving untouched rows, padding and a one-pixel stripe.

Use gfx-blt-control --run-present-list. Five cases cover small different row pitches,
GTT/VRAM source/destination combinations, minimum16-DWORD command buffers forcing
multipass rotation, and a1920x1200 copy. Allocations are made resident and queried
before/after; monitor-fence completion precedes an independent direct-memory DMA
readback and comparison against the CPU pattern. The existing native control is the
positive reference. Required:five pixel PASS,30 residency PASS,zero mismatches,
full command-buffer consumption, unchanged DWM/boot and exact baseline artifacts.

The transport is BC2S on the test-owned UMD context. This exercises the exact Present
GPU packet bytes but not D3DKMTPresent, the BGP1 private-record consumer, CDD context
residency or DWM presentation. Those integration/content requirements remain open;
this test cannot by itself close G0. No KMD, registry or desktop renderer mutation.
