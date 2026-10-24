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

/* GFX job capacity, separate from bring-up and SDMA. GFX10's get_rptr_gfx
 * reads the low 32 bits of the CP writeback slot (gfx_v10_0.c:8541).
 * Keep one dword empty so equal modulo pointers mean empty, never full.
 * In a 2048-dword ring with 256-dword alignment this admits seven slots.
 * Returning busy does not mutate the ring. The PASSIVE submit caller owns
 * waiting and its timeout; never spin at DIRQL or overwrite unread packets. */
static int bc250_ring_room(const struct amdgpu_ring *ring, u32 ndw)
{
	u32 used = ((u32)ring->wptr - *ring->rptr_cpu_addr) & ring->buf_mask;

	return ndw <= ring->buf_mask - used;
}

int bc250_ring_has_space(const struct amdgpu_ring *ring, unsigned int ndw)
{
	if (!ring->rptr_cpu_addr || ndw > ring->max_dw ||
	    ndw > (~0u - ring->funcs->align_mask)) return 0;
	ndw = (ndw + ring->funcs->align_mask) & ~ring->funcs->align_mask;
	if (ndw > ring->max_dw) return 0;
	return bc250_ring_room(ring, ndw);
}

/* [shim] BD-235: no reservation straddles a multiple of 2^30 ring dwords, which is ring byte 2^32 and,
 * because 2^32 is a multiple of 2^30, also dword 2^32. Unit A's SDMA0 stops inside a packet that
 * straddles byte 2^32 of its 64-bit ring pointer; the same packet behind NOPs that end exactly at
 * 2^32 runs clean (arm-ringstart3, boot a1). Upstream has no such rule (sdma_v5_0.c and
 * amdgpu_ring.c at v6.18 pad only to align_mask + 1), and it keeps long payloads in IBs, so its ring
 * packets are short. A CP ring's reservations are 256-dword slots on 256-dword boundaries and its
 * runtime frames are under 256 dwords, so there this rule can only act on a longer request.
 * bc250_ring_boundary_pad is 1 except in a lab control run (the lab KMD's PagingBoundaryMode 0). */
int bc250_ring_boundary_pad = 1;

unsigned int bc250_ring_boundary_pad_size(const struct amdgpu_ring *ring, unsigned int ndw)
{
	u64 mask = ring->funcs->align_mask;
	u64 end, next;

	if (!bc250_ring_boundary_pad || ndw > (~0u - ring->funcs->align_mask)) return 0;
	end = (ring->wptr + ndw + mask) & ~mask;
	next = (ring->wptr | (BC250_RING_BOUNDARY_DW - 1)) + 1;
	return end > next ? (unsigned int)(next - ring->wptr) : 0;
}

/* amdgpu_ring.c:81 amdgpu_ring_alloc().
 *
 * Deviations: upstream WARN_ON_ONCE()s an oversized request and returns -ENOMEM; the shim returns
 * the same kind of negative code through the caller's own error space. There is no
 * ring->funcs->begin_use, because there is no power management to wake.
 * [shim] BD-235: a reservation that would straddle a multiple of 2^30 dwords first gets NOPs up to
 * it (bc250_ring_boundary_pad_size()); they go out with the caller's commit. The pad is a multiple
 * of align_mask + 1 on an aligned write pointer, so the caller's own alignment arithmetic holds. A
 * ring that tracks its read pointer must have room for the pad and the request together. The paging
 * path of the KMD pads and publishes before it calls this, so here it finds nothing to pad. */
int amdgpu_ring_alloc(struct amdgpu_ring *ring, unsigned int ndw)
{
	unsigned int pad;

	if (ndw > (~0u - ring->funcs->align_mask)) return -1;
	/* Align requested size with padding so unlock_commit can pad safely */
	ndw = (ndw + ring->funcs->align_mask) & ~ring->funcs->align_mask;

	if (ndw > ring->max_dw) {
		dev_err(ring->adev->dev,
			"ring alloc of %u dwords exceeds max_dw %u\n", ndw, ring->max_dw);
		return -1;
	}

	pad = bc250_ring_boundary_pad_size(ring, ndw);
	if (pad != 0 && ring->track_rptr && (!ring->rptr_cpu_addr || !bc250_ring_room(ring, pad + ndw)))
		return -16;
	if (pad != 0)
		amdgpu_ring_insert_nop(ring, pad);

	if (ring->track_rptr && !bc250_ring_has_space(ring, ndw)) return -16;

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
	u64 wptr;

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

	/* The write pointer an SDMA engine is given counts BYTES, not dwords.
	 * sdma_v5_0_ring_set_wptr() (sdma_v5_0.c:371-389) publishes ring->wptr << 2 into both the
	 * write-back slot and the doorbell, and sdma_v5_0_ring_get_wptr() (:345-361) reads it back
	 * with >> 2; gfx_v10_0_ring_set_wptr_compute() (gfx_v10_0.c:8598) publishes ring->wptr as it
	 * stands. Upstream keeps the two apart in per-ring set_wptr callbacks. The shim's
	 * amdgpu_ring_funcs carries no callbacks (see amdgpu.h), so the ring type decides here, the
	 * same way bc250_gfx_emit_fence() branches on it.
	 *
	 * Nothing had committed on an SDMA ring before M6, so this was latent rather than wrong: the
	 * bring-up programs SDMA0_GFX_RB_WPTR directly and never goes through here. The first caller
	 * is bc250_sdma_ring_test(), and with the unshifted value the engine would have been told a
	 * write pointer four times too small and would have executed nothing. */
	wptr = (ring->funcs->type == AMDGPU_RING_TYPE_SDMA) ? (ring->wptr << 2) : ring->wptr;

	if (ring->wptr_cpu_addr != NULL)
		*(volatile u64 *)ring->wptr_cpu_addr = wptr;
	bc250_shim_wdoorbell64(ring->adev, ring->doorbell_index, wptr);
}

/* amdgpu_ring.c:204 amdgpu_ring_undo() */
void amdgpu_ring_undo(struct amdgpu_ring *ring)
{
	ring->wptr = ring->wptr_old;
}
