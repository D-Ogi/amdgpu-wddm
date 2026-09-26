/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * SDMA copy and fill packets (ADR 0013): the paging node's two building blocks, checked before
 * they ever reach BuildPagingBuffer. Every block names the upstream function and line it follows,
 * in driver/amdgpu-import/reference/sdma_v5_0.c (unmodified copy of the kernel's
 * drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c at v6.18, commit
 * 7d0a66e4bb9081d75c82ec4957c50034cb0ea449) or in the splitting loop of amdgpu_copy_buffer() /
 * amdgpu_ttm_fill_mem() (drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c, same tag - not imported, for the
 * reason driver/amdgpu-import/PROVENANCE.md already gives for amdgpu_gfx.c: it drags in the TTM
 * job, fence and reservation machinery, and the loop itself is three lines).
 *
 * bc250_sdma_copy_test() is the positive control (ADR 0013's own phrase): one linear copy and one
 * constant fill, submitted and fenced the way bc250_sdma_ring_test() already is, that never touches
 * the WDDM table. driver/kmd's BC250_ESCAPE_RUN_SDMACOPY reads its destination back from VRAM by
 * CPU before it trusts any of this against BuildPagingBuffer.
 */
#include "bc250_sdma.h"
#include "bc250_gmc.h"                    /* BC250_EINVAL */

/* AMD's SDMA 5.0 packet definitions, the same imported copy bc250_sdma.c uses; see its own header
 * comment and third_party/linux-amdgpu/PROVENANCE.md for where it comes from. No opcode, field
 * shift or mask below is typed - every one is one of this header's names. */
#include "navi10_sdma_pkt_open.h"

/* sdma_v5_0_buffer_funcs (reference/sdma_v5_0.c:2057-2065): 0x400000 for both copy and fill. Not
 * arbitrary: it is the width of SDMA_PKT_COPY_LINEAR_COUNT's and SDMA_PKT_CONSTANT_FILL_COUNT's
 * 22-bit count field (both hold byte_count - 1), and the host test checks that against the header's
 * own mask rather than this file repeating the number a third time. */
#define BC250_SDMA_COPY_MAX_BYTES	0x400000u
#define BC250_SDMA_FILL_MAX_BYTES	0x400000u

/* amdgpu_ttm.c: amdgpu_copy_buffer()'s `DIV_ROUND_UP(byte_count, max_bytes)` and
 * amdgpu_ttm_fill_mem()'s `DIV_ROUND_UP_ULL`, the same expression either way at these widths. */
static unsigned int PacketCount(unsigned int bytes, unsigned int max_bytes)
{
	return (bytes + max_bytes - 1u) / max_bytes;
}

unsigned int bc250_sdma_copy_linear_size(unsigned int bytes)
{
	if (bytes == 0)
		return 0;
	return PacketCount(bytes, BC250_SDMA_COPY_MAX_BYTES) * 7u;      /* .copy_num_dw */
}

unsigned int bc250_sdma_fill_size(unsigned int bytes)
{
	if (bytes == 0)
		return 0;
	return PacketCount(bytes, BC250_SDMA_FILL_MAX_BYTES) * 5u;      /* .fill_num_dw */
}

/* reference/sdma_v5_0.c:2018 sdma_v5_0_emit_copy_buffer(), looped as amdgpu_copy_buffer()
 * (amdgpu_ttm.c) loops it: one packet per up-to-copy_max_bytes chunk, both addresses advanced by
 * what the previous packet moved, byte_count - 1 into the COUNT field exactly as upstream writes
 * it (the dword is not run through SDMA_PKT_COPY_LINEAR_COUNT_COUNT() upstream either - the field
 * starts at bit 0 of its own dword, so the raw value and the macro's output are the same bits). */
int bc250_sdma_emit_copy_linear(struct amdgpu_ring *ring, u64 src_mc, u64 dst_mc, unsigned int bytes)
{
	unsigned int remaining;

	if (ring == NULL || ring->adev == NULL || ring->funcs == NULL || ring->ring == NULL)
		return BC250_EINVAL;
	if (ring->funcs->type != AMDGPU_RING_TYPE_SDMA)
		return BC250_EINVAL;
	if (bytes == 0)
		return BC250_EINVAL;

	remaining = bytes;
	while (remaining != 0) {
		u32 chunk = (remaining < BC250_SDMA_COPY_MAX_BYTES) ? remaining : BC250_SDMA_COPY_MAX_BYTES;

		amdgpu_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_COPY) |
					SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_COPY_LINEAR));
		amdgpu_ring_write(ring, chunk - 1u);
		amdgpu_ring_write(ring, 0);                        /* src/dst endian swap: none */
		amdgpu_ring_write(ring, lower_32_bits(src_mc));
		amdgpu_ring_write(ring, upper_32_bits(src_mc));
		amdgpu_ring_write(ring, lower_32_bits(dst_mc));
		amdgpu_ring_write(ring, upper_32_bits(dst_mc));

		src_mc += chunk;
		dst_mc += chunk;
		remaining -= chunk;
	}
	return 0;
}

/* reference/sdma_v5_0.c:2045 sdma_v5_0_emit_fill_buffer(), split the same way
 * (amdgpu_ttm_fill_mem(), amdgpu_ttm.c, fill_max_bytes). Upstream sets no sub_op and no FILLSIZE
 * field (both stay 0, byte granularity); this does not set them either, for the same reason the
 * copy above does not set ENCRYPT or BACKWARDS - a deviation from upstream's zero would be a
 * decision this driver has not made. */
int bc250_sdma_emit_fill(struct amdgpu_ring *ring, u64 dst_mc, u32 value, unsigned int bytes)
{
	unsigned int remaining;

	if (ring == NULL || ring->adev == NULL || ring->funcs == NULL || ring->ring == NULL)
		return BC250_EINVAL;
	if (ring->funcs->type != AMDGPU_RING_TYPE_SDMA)
		return BC250_EINVAL;
	if (bytes == 0)
		return BC250_EINVAL;

	remaining = bytes;
	while (remaining != 0) {
		u32 chunk = (remaining < BC250_SDMA_FILL_MAX_BYTES) ? remaining : BC250_SDMA_FILL_MAX_BYTES;

		amdgpu_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_CONST_FILL));
		amdgpu_ring_write(ring, lower_32_bits(dst_mc));
		amdgpu_ring_write(ring, upper_32_bits(dst_mc));
		amdgpu_ring_write(ring, value);
		amdgpu_ring_write(ring, chunk - 1u);

		dst_mc += chunk;
		remaining -= chunk;
	}
	return 0;
}

/* [shim] the alloc, the three emits and the commit together - the shape bc250_sdma_signal_fence()
 * and bc250_sdma_ring_test() both use. Order is fill, then copy, then fence: the fence only proves
 * the copy retired, and the copy has to read what the fill just wrote or it proves nothing about
 * either packet, only that some bytes moved from wherever src_mc happened to hold. On a refusal
 * from any emitter the reservation is undone and the ring is left as it was, exactly as
 * bc250_sdma_signal_fence() leaves it. */
int bc250_sdma_copy_test(struct amdgpu_ring *ring, u64 src_mc, u64 dst_mc, unsigned int bytes,
                         u32 pattern, u64 fence_addr, u64 seq, unsigned int flags)
{
	unsigned int fill_dw, copy_dw, fence_dw, ndw;
	int r;

	if (bytes == 0)
		return BC250_EINVAL;
	fill_dw = bc250_sdma_fill_size(bytes);
	copy_dw = bc250_sdma_copy_linear_size(bytes);
	fence_dw = bc250_sdma_fence_size(ring, flags);
	if (fill_dw == 0 || copy_dw == 0 || fence_dw == 0)
		return BC250_EINVAL;
	ndw = fill_dw + copy_dw + fence_dw;

	r = amdgpu_ring_alloc(ring, ndw);
	if (r)
		return r;

	r = bc250_sdma_emit_fill(ring, src_mc, pattern, bytes);
	if (r == 0)
		r = bc250_sdma_emit_copy_linear(ring, src_mc, dst_mc, bytes);
	if (r == 0)
		r = bc250_sdma_emit_fence(ring, fence_addr, seq, flags);
	if (r != 0) {
		amdgpu_ring_undo(ring);
		return r;
	}

	amdgpu_ring_commit(ring);
	return 0;
}

/* [shim] the two VRAM scratch regions, out of the same pool bc250_gfx_fence_page_alloc() and
 * bc250_sdma_fence_page_alloc() already draw from. All or nothing: a source that allocated but
 * left the destination unable to is not a region driver/kmd's escape could do anything useful
 * with, so both come back on any failure, exactly as bc250_sdma_fence_page_alloc() leaves nothing
 * half-allocated on its own refusal (bc250_sdma.c, D-03). */
int bc250_sdma_copy_regions_alloc(struct amdgpu_device *adev, unsigned int bytes,
                                  struct bc250_mem *src, struct bc250_mem *dst)
{
	int r;

	if (adev == NULL || src == NULL || dst == NULL || bytes == 0)
		return BC250_EINVAL;

	r = bc250_shim_mem_alloc(adev, BC250_MEM_VRAM, bytes, AMDGPU_GPU_PAGE_SIZE, src);
	if (r == 0)
		r = bc250_shim_mem_alloc(adev, BC250_MEM_VRAM, bytes, AMDGPU_GPU_PAGE_SIZE, dst);
	if (r == 0 && (src->cpu == NULL || dst->cpu == NULL))
		r = BC250_EINVAL;      /* bc250_shim.h: a caller that cannot map what it was handed refuses, not guesses */
	if (r != 0) {
		bc250_shim_mem_free(adev, src);
		bc250_shim_mem_free(adev, dst);
		return r;
	}
	return 0;
}

void bc250_sdma_copy_regions_free(struct amdgpu_device *adev, struct bc250_mem *src, struct bc250_mem *dst)
{
	bc250_shim_mem_free(adev, src);
	bc250_shim_mem_free(adev, dst);
}
