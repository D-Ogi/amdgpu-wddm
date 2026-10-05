/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * See include/bc250_sdma_paging.h. The room check and the throwaway-ring plumbing here are new;
 * every packet dword comes from bc250_sdma_emit_copy_linear()/emit_fill() (driver/shim/bc250_sdma_copy.c,
 * M95, already checked dword for dword by driver/shim/test/sdma_copy_packets.c) - this file writes
 * no opcode, field shift or mask of its own.
 */
#include "bc250_sdma_paging.h"
#include "bc250_sdma.h"
#include "navi10_sdma_pkt_open.h"

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

int bc250_sdma_paging_copy(struct amdgpu_device *adev, u32 *buffer, unsigned int buffer_dwords,
                           u64 src_mc, u64 dst_mc, unsigned int bytes, unsigned int *dwords_written)
{
	struct amdgpu_ring ring;
	unsigned int ndw;
	int r;

	if (adev == NULL || buffer == NULL || bytes == 0 || dwords_written == NULL)
		return BC250_SDMA_PAGING_EINVAL;

	ndw = bc250_sdma_copy_linear_size(bytes);
	PagingRingInit(&ring, adev, buffer, buffer_dwords);

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

int bc250_sdma_paging_fill(struct amdgpu_device *adev, u32 *buffer, unsigned int buffer_dwords,
                           u64 dst_mc, u32 pattern, unsigned int bytes, unsigned int *dwords_written)
{
	struct amdgpu_ring ring;
	unsigned int ndw;
	int r;

	if (adev == NULL || buffer == NULL || bytes == 0 || dwords_written == NULL)
		return BC250_SDMA_PAGING_EINVAL;

	ndw = bc250_sdma_fill_size(bytes);
	PagingRingInit(&ring, adev, buffer, buffer_dwords);

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

/* PROVENANCE: Linux amdgpu MIT, reference/sdma_v5_0.c:sdma_v5_0_vm_write_pte,
 * v6.18 commit7d0a66e4bb9081d75c82ec4957c50034cb0ea449. Same WRITE_LINEAR layout;
 * accept an explicit value array for noncontiguous OS pages instead of value+incr.
 * Build only: no live table is changed, no submission or TLB invalidation occurs.
 * The eventual caller must order mapping, TLB invalidation, copy, unmap and fence. */
static void PagingWritePtes(struct amdgpu_ring *ring, u64 table_mc, const u64 *ptes, unsigned int count)
{
    unsigned int i;
    amdgpu_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_WRITE) |
                           SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_WRITE_LINEAR));
    amdgpu_ring_write(ring, lower_32_bits(table_mc));
    amdgpu_ring_write(ring, upper_32_bits(table_mc));
    amdgpu_ring_write(ring, count * 2u - 1u);
    for (i = 0; i < count; ++i) {
        u64 value = ptes != NULL ? ptes[i] : 0;
        amdgpu_ring_write(ring, lower_32_bits(value));
        amdgpu_ring_write(ring, upper_32_bits(value));
    }
}

int bc250_sdma_paging_write_ptes(struct amdgpu_device *adev, u32 *buffer,
                                unsigned int buffer_dwords, u64 table_mc,
                                const u64 *ptes, unsigned int count,
                                unsigned int *dwords_written)
{
	struct amdgpu_ring ring;
	unsigned int ndw;

	if (dwords_written == NULL)
		return BC250_SDMA_PAGING_EINVAL;
	*dwords_written = 0;
	if (adev == NULL || buffer == NULL || ptes == NULL || count == 0 ||
	    count > (SDMA_PKT_WRITE_UNTILED_DW_3_count_mask + 1u) / 2u ||
	    (table_mc & 7u) != 0 || table_mc > ~(u64)0 - ((u64)count * 8u - 1u))
		return BC250_SDMA_PAGING_EINVAL;
	ndw = 4u + count * 2u;
	PagingRingInit(&ring, adev, buffer, buffer_dwords);
	if (amdgpu_ring_alloc(&ring, ndw) != 0) {
		*dwords_written = PagingAligned(ndw);
		return BC250_SDMA_PAGING_INSUFFICIENT;
	}
	PagingWritePtes(&ring, table_mc, ptes, count);
	*dwords_written = (unsigned int)ring.wptr;
	return BC250_SDMA_PAGING_OK;
}

/* PROVENANCE: Linux amdgpu MIT, sdma_v5_0_ring_emit_wreg/reg_wait/
 * reg_write_reg_wait and gmc_v10_0_emit_flush_gpu_tlb at v6.18,
 * commit7d0a66e4bb9081d75c82ec4957c50034cb0ea449.
 * SDMA5 uses GFXHUB (sdma_v5_0_set_ring_funcs); no MMHUB semaphore there.
 * Deviation: invalidate VMID0's existing GART without rewriting its root.
 * Engine0 is reserved for this SDMA0 paging stream; CPU flushes use17.
 * amdgpu_gmc_allocate_vm_inv_eng's0x1FFF3 pool starts at0 and excludes
 * firmware engines2/3 and CPU17. Future ring flushes must use other engines. */
static void PagingWriteRegister(struct amdgpu_ring *ring, u32 reg, u32 value)
{
	amdgpu_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_SRBM_WRITE) |
	                         SDMA_PKT_SRBM_WRITE_HEADER_BYTE_EN(0xfu));
	amdgpu_ring_write(ring, reg);
	amdgpu_ring_write(ring, value);
}

static void PagingWaitRegister(struct amdgpu_ring *ring, u32 reg, u32 value, u32 mask)
{
	amdgpu_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_POLL_REGMEM) |
	                         SDMA_PKT_POLL_REGMEM_HEADER_HDP_FLUSH(0) |
	                         SDMA_PKT_POLL_REGMEM_HEADER_FUNC(3));
	amdgpu_ring_write(ring, reg << 2);
	amdgpu_ring_write(ring, 0);
	amdgpu_ring_write(ring, value);
	amdgpu_ring_write(ring, mask);
	amdgpu_ring_write(ring, SDMA_PKT_POLL_REGMEM_DW5_RETRY_COUNT(0xfff) |
	                         SDMA_PKT_POLL_REGMEM_DW5_INTERVAL(10));
}

int bc250_sdma_paging_invalidate_gart(struct amdgpu_device *adev, u32 *buffer,
                                     unsigned int buffer_dwords, unsigned int *dwords_written)
{
	struct amdgpu_ring ring;
	const struct amdgpu_vmhub *hub;
	u32 req, ack, value;
	const unsigned int ndw = 3u + 6u + 6u;

	if (dwords_written == NULL)
		return BC250_SDMA_PAGING_EINVAL;
	*dwords_written = 0;
	if (adev == NULL || buffer == NULL)
		return BC250_SDMA_PAGING_EINVAL;
	hub = &adev->vmhub[AMDGPU_GFXHUB(0)];
	if (hub->vmhub_funcs == NULL || hub->vmhub_funcs->get_invalidate_req == NULL)
		return BC250_SDMA_PAGING_EINVAL;
	/* Engine0 offsets come from the initialized imported GFXHUB table. No
	 * CPU MMIO here, and no acknowledge/semaphore reads during construction. */
	req = hub->vm_inv_eng0_req;
	ack = hub->vm_inv_eng0_ack;
	if (req > SDMA_PKT_SRBM_WRITE_ADDR_addr_mask || ack > (~0u >> 2))
		return BC250_SDMA_PAGING_EINVAL;
	value = hub->vmhub_funcs->get_invalidate_req(0, 0);
	PagingRingInit(&ring, adev, buffer, buffer_dwords);
	if (amdgpu_ring_alloc(&ring, ndw) != 0) {
		*dwords_written = PagingAligned(ndw);
		return BC250_SDMA_PAGING_INSUFFICIENT;
	}
	PagingWriteRegister(&ring, req, value);
	/* Upstream: wait for a cycle to reset vm_inv_eng*_ack before polling ACK. */
	PagingWaitRegister(&ring, req, 0, 0);
	PagingWaitRegister(&ring, ack, 1u, 1u); /* VMID0 */
	*dwords_written = (unsigned int)ring.wptr;
	return BC250_SDMA_PAGING_OK;
}

/* sdma_v5_0_ring_emit_pipeline_sync waits for the preceding scheduler fence.
 * Within one paging transaction we emit an explicit noninterrupting fence first,
 * then the same memory poll. The caller must provide three fresh marker values
 * and a retained scratch slot, separate from the OS completion fence. */
static void PagingPipelineSync(struct amdgpu_ring *ring, u64 scratch_mc, u32 sequence)
{
	(void)bc250_sdma_emit_fence(ring, scratch_mc, sequence, 0);
	amdgpu_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_POLL_REGMEM) |
	                         SDMA_PKT_POLL_REGMEM_HEADER_HDP_FLUSH(0) |
	                         SDMA_PKT_POLL_REGMEM_HEADER_FUNC(3) |
	                         SDMA_PKT_POLL_REGMEM_HEADER_MEM_POLL(1));
	amdgpu_ring_write(ring, lower_32_bits(scratch_mc));
	amdgpu_ring_write(ring, upper_32_bits(scratch_mc));
	amdgpu_ring_write(ring, sequence);
	amdgpu_ring_write(ring, 0xffffffffu);
	amdgpu_ring_write(ring, SDMA_PKT_POLL_REGMEM_DW5_RETRY_COUNT(0xfff) |
	                         SDMA_PKT_POLL_REGMEM_DW5_INTERVAL(4));
}

static void PagingInvalidate(struct amdgpu_ring *ring, u32 req, u32 ack, u32 value)
{
	PagingWriteRegister(ring, req, value);
	PagingWaitRegister(ring, req, 0, 0);
	PagingWaitRegister(ring, ack, 1u, 1u);
}

int bc250_sdma_paging_mapped_copy(struct amdgpu_device *adev, u32 *buffer,
                                 unsigned int buffer_dwords,
                                 const struct bc250_sdma_paging_mapping *map,
                                 unsigned int *dwords_written)
{
	struct amdgpu_ring ring;
	const struct amdgpu_vmhub *hub;
	unsigned int ndw;
	u32 req, ack, value;

	if (dwords_written == NULL)
		return BC250_SDMA_PAGING_EINVAL;
	*dwords_written = 0;
	if (adev == NULL || buffer == NULL || map == NULL || map->ptes == NULL ||
	    map->page_count == 0 || map->page_count > (SDMA_PKT_WRITE_UNTILED_DW_3_count_mask + 1u) / 2u ||
	    (map->table_mc & 7u) != 0 || map->table_mc > ~(u64)0 - ((u64)map->page_count * 8u - 1u) ||
	    (map->scratch_mc & 3u) != 0 || map->first_sequence == 0 || map->first_sequence > ~0u - 2u ||
	    map->bytes == 0 || map->bytes > SDMA_PKT_COPY_LINEAR_COUNT_count_mask + 1u ||
	    map->src_mc > ~(u64)0 - (map->bytes - 1u) || map->dst_mc > ~(u64)0 - (map->bytes - 1u))
		return BC250_SDMA_PAGING_EINVAL;
	hub = &adev->vmhub[AMDGPU_GFXHUB(0)];
	if (hub->vmhub_funcs == NULL || hub->vmhub_funcs->get_invalidate_req == NULL)
		return BC250_SDMA_PAGING_EINVAL;
	req = hub->vm_inv_eng0_req; ack = hub->vm_inv_eng0_ack;
	if (req > SDMA_PKT_SRBM_WRITE_ADDR_addr_mask || ack > (~0u >> 2))
		return BC250_SDMA_PAGING_EINVAL;
	/* Two PTE writes, three fence/polls, two invalidations and one copy.
	 * Reserve the WHOLE transaction before touching the caller's buffer. */
	ndw = 2u * (4u + 2u * map->page_count) + 3u * 10u + 2u * 15u + 7u;
	PagingRingInit(&ring, adev, buffer, buffer_dwords);
	if (amdgpu_ring_alloc(&ring, ndw) != 0) {
		*dwords_written = PagingAligned(ndw);
		return BC250_SDMA_PAGING_INSUFFICIENT;
	}
	value = hub->vmhub_funcs->get_invalidate_req(0, 0);
	PagingWritePtes(&ring, map->table_mc, map->ptes, map->page_count);
	PagingPipelineSync(&ring, map->scratch_mc, map->first_sequence);
	PagingInvalidate(&ring, req, ack, value);
	(void)bc250_sdma_emit_copy_linear(&ring, map->src_mc, map->dst_mc, map->bytes);
	PagingPipelineSync(&ring, map->scratch_mc, map->first_sequence + 1u);
	PagingWritePtes(&ring, map->table_mc, NULL, map->page_count);
	PagingPipelineSync(&ring, map->scratch_mc, map->first_sequence + 2u);
	PagingInvalidate(&ring, req, ack, value);
	*dwords_written = (unsigned int)ring.wptr;
	return BC250_SDMA_PAGING_OK;
}
