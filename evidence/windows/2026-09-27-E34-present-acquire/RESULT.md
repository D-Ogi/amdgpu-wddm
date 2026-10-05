# GFX10 acquire before GPU Present copies

M618, host validation only. Parent/source/reference hashes: source-sha256.json.

The Present producer now uses a shared, tested complete-IB wrapper. Every pass
starts with the eight-DWORD ACQUIRE_MEM sequence from the existing imported
Linux gfx_v10_0_emit_mem_sync (v6.18,7d0a66e4bb9081d75c82ec4957c50034cb0ea449),
then DMA_DATA copies and full NOP padding. It includes GL2 invalidate/writeback
and the lower-cache operations used by shim/bc250_dispatch.c. The global range
and header count match that reference. Minimum Present DMA capacity is64 bytes.
MultipassOffset continues to count only copy packets.

The production wrapper passes independent pixel decoding at seven aligned
capacities (16 through64 DWORDs), including resumed lists, empty completion,
full NOP padding, unchanged output beyond capacity, first RAW_WAIT/last CP_SYNC,
and rejection of a malformed late rectangle without output modification.
The acquire test independently decodes header, range and cache-control bits;
all short capacities leave the output untouched. Existing copy tests also pass.
All12 mandatory quick gates pass. Actual WDDM and gfx_blt compilation pass.
Logs are copied unchanged; no private identifiers removed.

No new runtime test or deployment. This has not demonstrated CPU-to-GPU cache
visibility on hardware. It does not supply cross-queue waits, allocation
residency or shared backing lifetime. WDDM2 Present allocation lists themselves
do not establish residency; that premise must not be inferred from this change.
The gate stays default-off, GDI interop is not advertised, and G0 remains open.
