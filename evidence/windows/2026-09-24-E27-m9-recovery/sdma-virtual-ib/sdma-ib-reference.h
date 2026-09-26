/* PROVENANCE: imported AMD Linux v6.18 sdma_v5_0.c, MIT. */
struct amdgpu_job { unsigned int vmid; };
struct amdgpu_ib { u64 gpu_addr; u32 length_dw; };
static u64 reference_csa;
#define AMDGPU_JOB_GET_VMID(job) ((job)->vmid)
#define amdgpu_sdma_get_csa_mc_addr(ring, vmid) (reference_csa)
#define sdma_v5_0_ring_insert_nop amdgpu_ring_insert_nop
#pragma warning(push)
#pragma warning(disable:4100)
static void reference_sdma_emit_ib(struct amdgpu_ring *ring,
				   struct amdgpu_job *job,
				   struct amdgpu_ib *ib,
				   uint32_t flags)
{
	unsigned vmid = AMDGPU_JOB_GET_VMID(job);
	uint64_t csa_mc_addr = amdgpu_sdma_get_csa_mc_addr(ring, vmid);

	/* An IB packet must end on a 8 DW boundary--the next dword
	 * must be on a 8-dword boundary. Our IB packet below is 6
	 * dwords long, thus add x number of NOPs, such that, in
	 * modular arithmetic,
	 * wptr + 6 + x = 8k, k >= 0, which in C is,
	 * (wptr + 6 + x) % 8 = 0.
	 * The expression below, is a solution of x.
	 */
	sdma_v5_0_ring_insert_nop(ring, (2 - lower_32_bits(ring->wptr)) & 7);

	amdgpu_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_INDIRECT) |
			  SDMA_PKT_INDIRECT_HEADER_VMID(vmid & 0xf));
	/* base must be 32 byte aligned */
	amdgpu_ring_write(ring, lower_32_bits(ib->gpu_addr) & 0xffffffe0);
	amdgpu_ring_write(ring, upper_32_bits(ib->gpu_addr));
	amdgpu_ring_write(ring, ib->length_dw);
	amdgpu_ring_write(ring, lower_32_bits(csa_mc_addr));
	amdgpu_ring_write(ring, upper_32_bits(csa_mc_addr));
}
#pragma warning(pop)
#undef AMDGPU_JOB_GET_VMID
#undef amdgpu_sdma_get_csa_mc_addr
#undef sdma_v5_0_ring_insert_nop
