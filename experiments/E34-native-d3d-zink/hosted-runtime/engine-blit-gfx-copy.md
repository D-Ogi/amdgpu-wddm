# GFX10 CP DMA copy emitter

Bc250EmitGfxCopy builds ME DMA_DATA packets for the presenting GFX node. It follows
RADV's GFX9+ byte-count and L2-selection path and the imported AMD packet layout;
see driver/kmd/gfx-copy-provenance.md for the exact references. The builder itself
neither submits nor modifies the deployed KMD.

The range is divided at the RADV maximum count aligned down to32 bytes. The first
packet carries RAW_WAIT for preceding CP DMA; the last carries CP_SYNC to wait
for completion before subsequent ME work. Packet addresses retain all64 bits.
A zero-size request, overlapping backing-address ranges, arithmetic wraparound
or inadequate output capacity returns0 without modifying the output buffer.

CP_SYNC does not itself complete a WDDM fence, and RAW_WAIT does not flush shader
writes. The caller must establish valid GPU address mappings, cache visibility,
residency and allocation lifetime, submit this IB on the presenting node and
retire the corresponding fence through the hardware completion path. Cross-node
substitution by the paging SDMA engine is not implemented or assumed safe.

Next positive control, before any Present integration: use disjoint scratch
buffers, initialize nonuniform source and sentinel destination/padding, submit
copy packets through the existing GFX job/fence path, wait for that hardware
completion and compare every copied/untouched byte. Include both VRAM and mapped
system-memory staging, multiple rows and pitches, and a multi-packet copy. Retain
negative tests and exact source/artifact identity. No interop capability should
be enabled on the basis of host packet tests alone.

Local tests cover one-byte and unaligned lengths,32-byte boundary,4096 bytes,
a full1920x1200x4 frame, maximum packet count, one byte over it and UINT_MAX bytes.
They decode every packet's addresses/count/ordering flags, check total coverage,
last-packet sync, untouched output canaries, short-capacity atomic rejection,
range overlap, wraparound and adjacency. Host and kernel-flag compilation pass;
tools/quality/quick.ps1 enforces the gfx-copy test. This checks construction only;
it does not prove packet execution, coherence or the WDDM presentation contract.
