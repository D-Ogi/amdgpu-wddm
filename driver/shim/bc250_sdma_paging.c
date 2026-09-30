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

/* The room check every builder here makes before it writes a dword. A full OS DMA buffer is the
 * expected answer, not a ring overflow: the builder reports INSUFFICIENT and VidMm submits the buffer
 * and calls again with an empty one. amdgpu_ring_alloc() logs an oversized request with dev_err(),
 * which on a real ring means a driver bug; on this throwaway ring it filled game session 208's KMD log
 * with 113 false "ring alloc of 16 dwords exceeds max_dw N" errors (FLUSH_TLB at a buffer's end). So
 * the size is checked here first and the refusal stays quiet. The overflow guard mirrors
 * amdgpu_ring_alloc()'s own, since PagingAligned() would wrap. */
static int PagingReserve(struct amdgpu_ring *ring, unsigned int ndw)
{
	unsigned int align_mask = ring->funcs->align_mask;

	if (ndw > ~0u - align_mask || PagingAligned(ndw) > ring->max_dw)
		return -1;
	return amdgpu_ring_alloc(ring, ndw);
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

	r = PagingReserve(&ring, ndw);
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

	r = PagingReserve(&ring, ndw);
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
	if (PagingReserve(&ring, ndw) != 0) {
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
 * Deviation: invalidate an existing VMID without rewriting its root. The GART
 * wrapper selects VMID0; other VMIDs need caller-owned root/lifetime ordering.
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

int bc250_sdma_paging_invalidate_vmid(struct amdgpu_device *adev, u32 *buffer,
                                     unsigned int buffer_dwords, unsigned int vmid, unsigned int *dwords_written)
{
	struct amdgpu_ring ring;
	const struct amdgpu_vmhub *hub;
	u32 req, ack, value;
	const unsigned int ndw = 3u + 6u + 6u;

	if (dwords_written == NULL)
		return BC250_SDMA_PAGING_EINVAL;
	*dwords_written = 0;
	if (adev == NULL || buffer == NULL || vmid >= 16)
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
	value = hub->vmhub_funcs->get_invalidate_req(vmid, 0);
	PagingRingInit(&ring, adev, buffer, buffer_dwords);
	if (PagingReserve(&ring, ndw) != 0) {
		*dwords_written = PagingAligned(ndw);
		return BC250_SDMA_PAGING_INSUFFICIENT;
	}
	PagingWriteRegister(&ring, req, value);
	/* Upstream: wait for a cycle to reset vm_inv_eng*_ack before polling ACK. */
	PagingWaitRegister(&ring, req, 0, 0);
	PagingWaitRegister(&ring, ack, 1u << vmid, 1u << vmid);
	*dwords_written = (unsigned int)ring.wptr;
	return BC250_SDMA_PAGING_OK;
}

int bc250_sdma_paging_invalidate_gart(struct amdgpu_device *adev, u32 *buffer,
                                     unsigned int buffer_dwords, unsigned int *dwords_written)
{
    return bc250_sdma_paging_invalidate_vmid(adev, buffer, buffer_dwords, 0, dwords_written);
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

/* A queued page-table update must finish its memory writes before a later
 * FLUSH_TLB packet may invalidate cached translations. Reuse the AMD write
 * packet and the same fence/poll pipeline barrier as mapped transfers. */
int bc250_sdma_paging_update_ptes(struct amdgpu_device *adev, u32 *buffer,
                                 unsigned int buffer_dwords, u64 table_mc,
                                 const u64 *ptes, unsigned int count,
                                 u64 scratch_mc, u32 sequence,
                                 unsigned int *dwords_written)
{
    struct amdgpu_ring ring;
    unsigned int ndw;
    if (dwords_written == NULL) return BC250_SDMA_PAGING_EINVAL;
    *dwords_written = 0;
    if (adev == NULL || buffer == NULL || ptes == NULL || count == 0 ||
        count > (SDMA_PKT_WRITE_UNTILED_DW_3_count_mask + 1u) / 2u ||
        (table_mc & 7u) != 0 || table_mc > ~(u64)0 - ((u64)count * 8u - 1u) ||
        (scratch_mc & 3u) != 0 || scratch_mc == 0 || sequence == 0)
        return BC250_SDMA_PAGING_EINVAL;
    // Reserve the complete write and barrier before touching the OS buffer.
    ndw = 4u + 2u * count + 10u;
    PagingRingInit(&ring, adev, buffer, buffer_dwords);
    if (PagingReserve(&ring, ndw) != 0) {
        *dwords_written = PagingAligned(ndw);
        return BC250_SDMA_PAGING_INSUFFICIENT;
    }
    PagingWritePtes(&ring, table_mc, ptes, count);
    PagingPipelineSync(&ring, scratch_mc, sequence);
    *dwords_written = (unsigned int)ring.wptr;
    return BC250_SDMA_PAGING_OK;
}

static void PagingInvalidate(struct amdgpu_ring *ring, u32 req, u32 ack, u32 value)
{
	PagingWriteRegister(ring, req, value);
	PagingWaitRegister(ring, req, 0, 0);
	PagingWaitRegister(ring, ack, 1u, 1u);
}

/* Ranges have already been checked for unsigned end-address overflow. */
static int PagingRangesOverlap(u64 a, unsigned int a_bytes, u64 b, unsigned int b_bytes)
{
    return a <= b + b_bytes - 1u && b <= a + a_bytes - 1u;
}

/* Permanent OS map/unmap: the caller supplies encoded MDL or repeated dummy
 * PTEs. Reserve write, memory barrier and VMID0 invalidation as one unit. */
int bc250_sdma_paging_set_aperture(struct amdgpu_device *adev, u32 *buffer,
                                  unsigned int buffer_dwords, u64 table_mc,
                                  const u64 *ptes, unsigned int count,
                                  u64 scratch_mc, u32 sequence,
                                  unsigned int *dwords_written)
{
    struct amdgpu_ring ring;
    const struct amdgpu_vmhub *hub;
    unsigned int ndw;
    u32 req,ack,value;
    const u64 limit=0x1000000000000ull;
    if (!dwords_written) return BC250_SDMA_PAGING_EINVAL;
    *dwords_written=0;
    if (!adev || !buffer || !ptes || !count ||
        count>(SDMA_PKT_WRITE_UNTILED_DW_3_count_mask+1u)/2u ||
        (table_mc&7u) || table_mc>limit-(u64)count*8u ||
        !scratch_mc || (scratch_mc&3u) || scratch_mc>limit-4u || !sequence ||
        PagingRangesOverlap(table_mc,count*8u,scratch_mc,4))
        return BC250_SDMA_PAGING_EINVAL;
    hub=&adev->vmhub[AMDGPU_GFXHUB(0)];
    if (!hub->vmhub_funcs || !hub->vmhub_funcs->get_invalidate_req)
        return BC250_SDMA_PAGING_EINVAL;
    req=hub->vm_inv_eng0_req;ack=hub->vm_inv_eng0_ack;
    if (req>SDMA_PKT_SRBM_WRITE_ADDR_addr_mask || ack>(~0u>>2))
        return BC250_SDMA_PAGING_EINVAL;
    ndw=29u+2u*count;
    PagingRingInit(&ring,adev,buffer,buffer_dwords);
    if (PagingReserve(&ring, ndw)!=0) {
        *dwords_written=PagingAligned(ndw);
        return BC250_SDMA_PAGING_INSUFFICIENT;
    }
    value=hub->vmhub_funcs->get_invalidate_req(0,0);
    PagingWritePtes(&ring,table_mc,ptes,count);
    PagingPipelineSync(&ring,scratch_mc,sequence);
    PagingInvalidate(&ring,req,ack,value);
    *dwords_written=(unsigned int)ring.wptr;
    return BC250_SDMA_PAGING_OK;
}

int bc250_sdma_paging_mapped_transfer(struct amdgpu_device *adev, u32 *buffer,
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
	    map->fill > 1 || (map->fill && ((map->dst_mc | map->bytes) & 3u)) ||
	    map->bytes == 0 || map->bytes > SDMA_PKT_COPY_LINEAR_COUNT_count_mask + 1u ||
	    map->src_mc > ~(u64)0 - (map->bytes - 1u) || map->dst_mc > ~(u64)0 - (map->bytes - 1u))
		return BC250_SDMA_PAGING_EINVAL;
    if (map->staging_mc && (map->fill || map->bytes>4096 || (map->staging_mc&4095) ||
        map->staging_mc>~(u64)0-4095 || map->scratch_mc>~(u64)0-3 || map->first_sequence>~0u-3 ||
        PagingRangesOverlap(map->staging_mc,4096,map->src_mc,map->bytes) ||
        PagingRangesOverlap(map->staging_mc,4096,map->dst_mc,map->bytes) ||
        PagingRangesOverlap(map->staging_mc,4096,map->scratch_mc,4) ||
        PagingRangesOverlap(map->staging_mc,4096,map->table_mc,map->page_count*8)))
        return BC250_SDMA_PAGING_EINVAL;
	hub = &adev->vmhub[AMDGPU_GFXHUB(0)];
	if (hub->vmhub_funcs == NULL || hub->vmhub_funcs->get_invalidate_req == NULL)
		return BC250_SDMA_PAGING_EINVAL;
	req = hub->vm_inv_eng0_req; ack = hub->vm_inv_eng0_ack;
	if (req > SDMA_PKT_SRBM_WRITE_ADDR_addr_mask || ack > (~0u >> 2))
		return BC250_SDMA_PAGING_EINVAL;
	/* Two PTE writes, three fence/polls, two invalidations and one copy.
	 * Reserve the WHOLE transaction before touching the caller's buffer. */
	ndw = 2u * (4u + 2u * map->page_count) + 3u * 10u + 2u * 15u + (map->fill ? 5u : 7u);
	if (map->staging_mc) ndw+=7u+10u;
	PagingRingInit(&ring, adev, buffer, buffer_dwords);
	if (PagingReserve(&ring, ndw) != 0) {
		*dwords_written = PagingAligned(ndw);
		return BC250_SDMA_PAGING_INSUFFICIENT;
	}
	value = hub->vmhub_funcs->get_invalidate_req(0, 0);
	PagingWritePtes(&ring, map->table_mc, map->ptes, map->page_count);
	PagingPipelineSync(&ring, map->scratch_mc, map->first_sequence);
	PagingInvalidate(&ring, req, ack, value);
	if (map->fill) (void)bc250_sdma_emit_fill(&ring, map->dst_mc, map->pattern, map->bytes);
    else if (map->staging_mc) {
        (void)bc250_sdma_emit_copy_linear(&ring,map->src_mc,map->staging_mc,map->bytes);
        PagingPipelineSync(&ring,map->scratch_mc,map->first_sequence+1u);
        (void)bc250_sdma_emit_copy_linear(&ring,map->staging_mc,map->dst_mc,map->bytes);
    } else (void)bc250_sdma_emit_copy_linear(&ring, map->src_mc, map->dst_mc, map->bytes);
	PagingPipelineSync(&ring, map->scratch_mc, map->first_sequence + (map->staging_mc?2u:1u));
	PagingWritePtes(&ring, map->table_mc, NULL, map->page_count);
	PagingPipelineSync(&ring, map->scratch_mc, map->first_sequence + (map->staging_mc?3u:2u));
	PagingInvalidate(&ring, req, ack, value);
	*dwords_written = (unsigned int)ring.wptr;
	return BC250_SDMA_PAGING_OK;
}

int bc250_sdma_paging_copy_bytes(struct amdgpu_device *adev, u32 *buffer,
                              unsigned int buffer_dwords, u64 src_mc, u64 dst_mc,
                              unsigned int bytes, u64 staging_mc, u64 marker_mc,
                              u32 first_sequence, unsigned int *dwords_written)
{
    struct amdgpu_ring ring;
    unsigned int ndw,required;
    if (dwords_written==NULL) return BC250_SDMA_PAGING_EINVAL;
    *dwords_written=0;
    // The private staging page holds up to4KiB, including unaligned byte ranges.
    // No source/destination may alias any part of that reserved page.
    if (!adev || !buffer || !bytes || bytes>4096u || (staging_mc&4095u) || (marker_mc&3u) ||
        staging_mc>~(u64)0-4095u || marker_mc>~(u64)0-3u ||
        !first_sequence || first_sequence==~0u) return BC250_SDMA_PAGING_EINVAL;
    if (src_mc>~(u64)0-(bytes-1u) || dst_mc>~(u64)0-(bytes-1u) ||
        PagingRangesOverlap(staging_mc,4096,src_mc,bytes) ||
        PagingRangesOverlap(staging_mc,4096,dst_mc,bytes) ||
        PagingRangesOverlap(marker_mc,4,src_mc,bytes) ||
        PagingRangesOverlap(marker_mc,4,dst_mc,bytes) ||
        PagingRangesOverlap(marker_mc,4,staging_mc,4096)) return BC250_SDMA_PAGING_EINVAL;
    ndw=2u*bc250_sdma_copy_linear_size(bytes)+2u*10u;
    required=PagingAligned(ndw);
    if (buffer_dwords<required) {
        *dwords_written=required;
        return BC250_SDMA_PAGING_INSUFFICIENT;
    }
    // Bound the temporary ring to this transaction, independent of caller size.
    PagingRingInit(&ring,adev,buffer,required);
    if (PagingReserve(&ring, ndw)!=0) {
        *dwords_written=required;
        return BC250_SDMA_PAGING_INSUFFICIENT;
    }
    // Existing AMD copy emission; the intermediate page makes both copies
    // disjoint even when the original table ranges overlap. Finish each write
    // phase before consuming/reusing staging. Caller supplies fresh markers.
    (void)bc250_sdma_emit_copy_linear(&ring,src_mc,staging_mc,bytes);
    PagingPipelineSync(&ring,marker_mc,first_sequence);
    (void)bc250_sdma_emit_copy_linear(&ring,staging_mc,dst_mc,bytes);
    PagingPipelineSync(&ring,marker_mc,first_sequence+1u);
    *dwords_written=(unsigned int)ring.wptr;
    return BC250_SDMA_PAGING_OK;
}

int bc250_sdma_paging_copy_ptes(struct amdgpu_device *adev, u32 *buffer,
                              unsigned int buffer_dwords, u64 src_mc, u64 dst_mc,
                              unsigned int entries, u64 staging_mc, u64 marker_mc,
                              u32 first_sequence, unsigned int *dwords_written)
{
    if (dwords_written==NULL) return BC250_SDMA_PAGING_EINVAL;
    *dwords_written=0;
    if (!entries || entries>4096u/8u || ((src_mc|dst_mc)&7u)) return BC250_SDMA_PAGING_EINVAL;
    return bc250_sdma_paging_copy_bytes(adev,buffer,buffer_dwords,src_mc,dst_mc,
        entries*8u,staging_mc,marker_mc,first_sequence,dwords_written);
}
