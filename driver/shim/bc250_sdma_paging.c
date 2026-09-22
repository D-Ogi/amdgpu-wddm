/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * See include/bc250_sdma_paging.h. The room check and the throwaway-ring plumbing here are new;
 * every packet dword comes from bc250_sdma_emit_copy_linear()/emit_fill() (driver/shim/bc250_sdma_copy.c,
 * M95, already checked dword for dword by driver/shim/test/sdma_copy_packets.c) - this file writes
 * no opcode, field shift or mask of its own.
 */
#include "bc250_sdma_paging.h"
#include "bc250_sdma.h"

/* A mask covering [0, n) with the fewest extra bits: the smallest (power of two - 1) that is >= n - 1.
 * amdgpu_ring_write()'s "index & buf_mask" needs this to behave as a wrap boundary; since every call
 * here starts a fresh throwaway ring at wptr 0 and amdgpu_ring_alloc() never admits more than
 * buffer_dwords dwords (<= this mask + 1 by construction), no write ever actually wraps - the mask
 * only has to be wide enough that no two different indices fold onto the same slot. */
static unsigned int PagingRingMask(unsigned int buffer_dwords)
{
	unsigned int mask = 1u;

	while (mask < buffer_dwords)
		mask <<= 1;
	return mask - 1u;
}

static void PagingRingInit(struct amdgpu_ring *ring, struct amdgpu_device *adev, u32 *buffer,
                           unsigned int buffer_dwords)
{
	memset(ring, 0, sizeof(*ring));
	ring->adev = adev;
	ring->funcs = bc250_sdma_ring_funcs();
	ring->ring = buffer;
	ring->buf_mask = PagingRingMask(buffer_dwords);
	ring->ptr_mask = 0xFFFFFFFFFFFFFFFFULL;
	ring->max_dw = buffer_dwords;
	ring->use_doorbell = false;	/* never committed: gpumem.c:GpuMemDoorbellWrite rings the live ring's, not this one's */
}

/* The dword count amdgpu_ring_alloc() actually gates on, i.e. ndw rounded up to the SDMA engine's
 * own align_mask - the same rounding amdgpu_ring_alloc() (bc250_ring.c) performs internally, spelled
 * out here only so the INSUFFICIENT answer can report it (amdgpu_ring_alloc() itself returns nothing
 * but -1 on refusal). */
static unsigned int PagingAligned(unsigned int ndw)
{
	unsigned int align_mask = bc250_sdma_ring_funcs()->align_mask;

	return (ndw + align_mask) & ~align_mask;
}

int bc250_sdma_paging_copy(u32 *buffer, unsigned int buffer_dwords, u64 src_mc, u64 dst_mc,
                           unsigned int bytes, unsigned int *dwords_written)
{
	struct amdgpu_device adev_stub;
	struct amdgpu_ring ring;
	unsigned int ndw;
	int r;

	if (buffer == NULL || bytes == 0 || dwords_written == NULL)
		return BC250_SDMA_PAGING_EINVAL;

	ndw = bc250_sdma_copy_linear_size(bytes);
	PagingRingInit(&ring, &adev_stub, buffer, buffer_dwords);

	r = amdgpu_ring_alloc(&ring, ndw);
	if (r != 0) {
		*dwords_written = PagingAligned(ndw);
		return BC250_SDMA_PAGING_INSUFFICIENT;
	}

	r = bc250_sdma_emit_copy_linear(&ring, src_mc, dst_mc, bytes);
	if (r != 0) {
		amdgpu_ring_undo(&ring);
		return BC250_SDMA_PAGING_EINVAL;
	}

	*dwords_written = (unsigned int)ring.wptr;
	return BC250_SDMA_PAGING_OK;
}

int bc250_sdma_paging_fill(u32 *buffer, unsigned int buffer_dwords, u64 dst_mc, u32 pattern,
                           unsigned int bytes, unsigned int *dwords_written)
{
	struct amdgpu_device adev_stub;
	struct amdgpu_ring ring;
	unsigned int ndw;
	int r;

	if (buffer == NULL || bytes == 0 || dwords_written == NULL)
		return BC250_SDMA_PAGING_EINVAL;

	ndw = bc250_sdma_fill_size(bytes);
	PagingRingInit(&ring, &adev_stub, buffer, buffer_dwords);

	r = amdgpu_ring_alloc(&ring, ndw);
	if (r != 0) {
		*dwords_written = PagingAligned(ndw);
		return BC250_SDMA_PAGING_INSUFFICIENT;
	}

	r = bc250_sdma_emit_fill(&ring, dst_mc, pattern, bytes);
	if (r != 0) {
		amdgpu_ring_undo(&ring);
		return BC250_SDMA_PAGING_EINVAL;
	}

	*dwords_written = (unsigned int)ring.wptr;
	return BC250_SDMA_PAGING_OK;
}
