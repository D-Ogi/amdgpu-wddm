/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The ring write path, transcribed from amdgpu_ring.c and amdgpu_ring.h at kernel tag v6.18,
 * commit 7d0a66e4bb9081d75c82ec4957c50034cb0ea449 (driver/amdgpu-import/PROVENANCE.md).
 *
 * This is the whole of it: a packet buffer in GPU-visible memory, a write pointer in dwords, and a
 * doorbell. Upstream carries a scheduler, fences, IB pools and power-management hooks on the same
 * struct; none of that is reachable from a bring-up packet, so none of it is here. Every deviation
 * is named at the point it happens.
 *
 * The write pointer of a CP ring counts dwords. SDMA counts bytes, which is why sdma_v5_0's
 * set_wptr shifts by 2 - that shift lives in bc250_sdma.c, not here.
 */
#include "amdgpu.h"

/* amdgpu_ring.c:81 amdgpu_ring_alloc().
 *
 * Deviations: upstream WARN_ON_ONCE()s an oversized request and returns -ENOMEM; the shim returns
 * the same kind of negative code through the caller's own error space. There is no
 * ring->funcs->begin_use, because there is no power management to wake. */
int amdgpu_ring_alloc(struct amdgpu_ring *ring, unsigned int ndw)
{
	/* Align requested size with padding so unlock_commit can pad safely */
	ndw = (ndw + ring->funcs->align_mask) & ~ring->funcs->align_mask;

	if (ndw > ring->max_dw) {
		dev_err(ring->adev->dev,
			"ring alloc of %u dwords exceeds max_dw %u\n", ndw, ring->max_dw);
		return -1;
	}

	ring->count_dw = (int)ndw;
	ring->wptr_old = ring->wptr;

	return 0;
}

/* amdgpu_ring.c:132 amdgpu_ring_insert_nop(). memset32() is a Linux helper (GPL-2.0 header), so the
 * two fills are written out; the behaviour is the same. */
void amdgpu_ring_insert_nop(struct amdgpu_ring *ring, uint32_t count)
{
	uint32_t occupied, chunk1, chunk2, i;

	occupied = (uint32_t)(ring->wptr & ring->buf_mask);
	chunk1 = ring->buf_mask + 1u - occupied;
	chunk1 = (chunk1 >= count) ? count : chunk1;
	chunk2 = count - chunk1;

	for (i = 0; i < chunk1; i++)
		ring->ring[occupied + i] = ring->funcs->nop;

	for (i = 0; i < chunk2; i++)
		ring->ring[i] = ring->funcs->nop;

	ring->wptr += count;
	ring->wptr &= ring->ptr_mask;
	ring->count_dw -= (int)count;
}

/* amdgpu_ring.h:482 amdgpu_ring_clear_ring() */
void amdgpu_ring_clear_ring(struct amdgpu_ring *ring)
{
	u32 i;

	for (i = 0; i <= ring->buf_mask; i++)
		ring->ring[i] = ring->funcs->nop;
}

/* amdgpu_ring.c:175 amdgpu_ring_commit().
 *
 * Deviations: upstream's mb() is a full memory barrier before the doorbell, which on Windows is the
 * caller's KeMemoryBarrier; the shim has no barrier primitive of its own, so the backend's doorbell
 * write is documented as having to order after the ring stores (driver/shim/README.md). There is no
 * ring->funcs->end_use.
 *
 * The write pointer reaches the hardware exactly as gfx_v10_0_ring_set_wptr_compute()
 * (gfx_v10_0.c:8598) does it: store it into the writeback slot the CP polls, then ring the
 * doorbell. Upstream BUG()s if a compute or KIQ ring has no doorbell; the shim refuses instead. */
void amdgpu_ring_commit(struct amdgpu_ring *ring)
{
	uint32_t count;

	if (ring->count_dw < 0)
		dev_err(ring->adev->dev, "writing more dwords to the ring than expected\n");

	/* We pad to match fetch size */
	count = ring->funcs->align_mask + 1u - (uint32_t)(ring->wptr & ring->funcs->align_mask);
	count &= ring->funcs->align_mask;

	if (count != 0)
		amdgpu_ring_insert_nop(ring, count);

	if (!ring->use_doorbell) {
		dev_err(ring->adev->dev,
			"ring %u has no doorbell; gfx10 has no MMIO write-pointer path\n",
			ring->doorbell_index);
		return;
	}

	if (ring->wptr_cpu_addr != NULL)
		*(volatile u64 *)ring->wptr_cpu_addr = ring->wptr;
	bc250_shim_wdoorbell64(ring->adev, ring->doorbell_index, ring->wptr);
}

/* amdgpu_ring.c:204 amdgpu_ring_undo() */
void amdgpu_ring_undo(struct amdgpu_ring *ring)
{
	ring->wptr = ring->wptr_old;
}
