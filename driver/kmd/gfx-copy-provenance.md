# CP DMA definitions

PROVENANCE: Mesa (https://gitlab.freedesktop.org/mesa/mesa), MIT.

The selected definitions in gfx_copy_fields.h are unchanged lines from the
existing RADV build's generated src/amd/common/amd_cp_packets_gfx11.h, SHA256
d64d9e81fc7c795cbb38c5f9879fcc0fabff08570f68ef5a38359ba1b3851b89.
Despite that generated filename, RADV uses these definitions for the GFX9+
branch of radv_cs_emit_cp_dma, including GFX10. We follow that explicit branch,
not a general claim that GFX11 packets can be used on GFX10. The Linux nvd.h
already imported by this project supplies PACKET3 and PACKET3_DMA_DATA.

Behavior source: Mesa05e6c9622e135ac2aeaf56ec70222642627e2162,
src/amd/vulkan/radv_cp_dma.c, radv_cs_emit_cp_dma, cp_dma_max_byte_count,
radv_cp_dma_prepare. The GFX9+ count field is aligned down to32 bytes for each
maximum-size chunk; the final packet carries CP_SYNC. The first packet uses
RAW_WAIT to order earlier CP DMA. L2 source/destination selection matches RADV's
CP_DMA_USE_L2 path. Cache flushes and shader ordering remain caller obligations.

PROVENANCE: Linux amdgpu (https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git), MIT for the imported AMD files.

The acquire sequence follows the existing imported gfx_v10_0_emit_mem_sync in
shim/bc250_dispatch.c:224-244 and nvd.h. It has eight DWORDs including its header,
full coherency range, and GL2 writeback/invalidate plus lower cache operations.
This does not provide residency or inter-queue waits. Exact existing reference
file hashes for this addition are retained with the validation evidence.
