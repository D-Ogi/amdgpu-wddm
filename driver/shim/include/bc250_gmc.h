/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * GART bring-up for Cyan Skillfish: the hardware-independent part of amdgpu's GMC v10 path
 * (ADR 0002, milestone M4).
 *
 * Everything that actually touches a register lives in the unmodified AMD sources in
 * driver/amdgpu-import/ (gfxhub_v2_0.c, mmhub_v2_0.c). What is here is the glue gmc_v10_0.c
 * provides around them, reduced to the parts that are not Linux: how adev is filled in, in which
 * order the hubs are driven, and the TLB flush. It is compiled into the host replay test
 * (driver/shim/test) and into the kernel miniport, so that the driver runs the very code the
 * replay proves against unit A's amdgpu trace.
 *
 * No statics, no globals, no CRT beyond memset: everything is in the caller's amdgpu_device.
 */
#ifndef BC250_GMC_H
#define BC250_GMC_H

#include "amdgpu.h"

/* Return codes, negative like the kernel's, with Linux's numeric values so that a code seen in a
 * log means the same thing on both sides. */
#define BC250_EINVAL	(-22)
#define BC250_EBUSY	(-16)
#define BC250_ETIME	(-62)

/*
 * The GFXHUB invalidation engines, one owner each.
 *
 * A hub has 18 invalidation engines. Each engine has its own request, acknowledge and semaphore
 * register, eng_distance apart (gfxhub_v2_0_init(), driver/amdgpu-import/gfxhub_v2_0.c:445-463;
 * mmGCVM_INVALIDATE_ENG0_REQ/_ACK/_SEM and mmGCVM_INVALIDATE_ENG1_REQ give the distance). A
 * request starts an invalidation and the engine sets one bit per VMID in its acknowledge register.
 *
 * Upstream gives each ring its own engine in amdgpu_gmc_allocate_vm_inv_eng() out of the 0x1FFF3
 * mask (engines 2 and 3 belong to firmware), uses engine 17 for the CPU path of
 * gmc_v10_0_flush_gpu_tlb(), and keeps adev->gmc.invalidate_lock for that CPU path alone. The
 * reason for both is the same: a request and its acknowledge are the state of one engine, so two
 * writers of one engine read each other's acknowledge.
 *
 * This driver has no engine allocator and no equivalent of invalidate_lock. The engines are
 * therefore named here, once, and each owner has an engine of its own:
 *
 *   0     the SDMA0 paging stream, in SDMA packets (bc250_sdma_paging.c,
 *         bc250_sdma_emit_vm_flush())
 *   1     the gfx ring, in PM4 packets (bc250_gfx_emit_vm_flush(), the EnableRingVmFlush gate of
 *         the miniport)
 *   2, 3  firmware, as upstream
 *   17    every CPU MMIO flush (bc250_gmc_flush_gpu_tlb()), which the miniport serializes under
 *         one lock at PASSIVE_LEVEL
 *
 * A new owner takes a free engine of the upstream mask and gets a line here. Two owners on one
 * engine need a lock that both obey, and no such lock exists.
 */
#define BC250_INV_ENG_SDMA_PAGING	0u
#define BC250_INV_ENG_GFX_RING		1u
#define BC250_INV_ENG_CPU		17u
#define BC250_INV_ENG_COUNT		18u

/*
 * The inputs that no register and no formula gives: addresses of buffers somebody else allocated,
 * and one policy bit.
 *
 * gart_table_mc   MC address of the GART page directory (amdgpu: amdgpu_bo_gpu_offset(gart.bo)).
 * mem_scratch_mc  MC address of the scratch page that unmapped system-aperture accesses land on.
 * dummy_page_dma  DMA address of the dummy page that faulting accesses are redirected to.
 * noretry         true = send no-retry XNACK on a fault instead of retrying. On unit A's kernel
 *                 amdgpu computes true; see driver/shim/README.md for why, and never guess it.
 */
struct bc250_gmc_inputs {
	u64 gart_table_mc;
	u64 mem_scratch_mc;
	u64 dummy_page_dma;
	bool noretry;
};

/*
 * Fill in adev the way amdgpu's GMC v10 sw_init/mc_init path leaves it for this SoC: register
 * bases, IP versions, hub function tables, the memory layout read back from the hardware, the page
 * table geometry, and the three input addresses.
 *
 * adev must be zeroed by the caller, which may set adev->dev and adev->backend first; both survive.
 * gart_bo is storage the caller owns and keeps alive for as long as adev is used; this function
 * fills it in and points adev->gart.bo at it.
 *
 * Reads registers, writes none. Returns 0, or BC250_EINVAL on a bad argument or if the hardware
 * reports no VRAM (which means the backend is not really reaching the device).
 */
int bc250_gmc_setup(struct amdgpu_device *adev, const struct bc250_gmc_inputs *in,
		    struct amdgpu_bo *gart_bo);

/* gmc_v10_0_gart_enable(), register traffic only. Returns 0, or what a hub's gart_enable returned. */
int bc250_gmc_gart_enable(struct amdgpu_device *adev);

/* gmc_v10_0_gart_disable(). */
void bc250_gmc_gart_disable(struct amdgpu_device *adev);

/* gmc_v10_0_flush_gpu_tlb(), register traffic only. vmhub is AMDGPU_GFXHUB(0) or AMDGPU_MMHUB0(0).
 * Returns 0, or BC250_ETIME if the semaphore could not be taken or the acknowledge never came. The
 * MMHUB semaphore is released on every path that took it, timeout included. */
int bc250_gmc_flush_gpu_tlb(struct amdgpu_device *adev, u32 vmid, u32 vmhub, u32 flush_type);
/* Optional synchronous observation, caller serialized; callback must not mutate
 * hardware or reenter the flush. Value is the existing write/read sample. */
typedef void (*bc250_tlb_observer)(struct amdgpu_device *adev, const char *phase, u32 value);
/* Configuration only: both hub enables and fault defaults, no TLB flush.
 * Caller must retain pending translation state and complete both hub flushes
 * before consumers can use new mappings. Not a replacement for full enable. */
int bc250_gmc_gart_configure_observed(struct amdgpu_device *adev, bc250_tlb_observer observer);
int bc250_gmc_gart_enable_observed(struct amdgpu_device *adev, bc250_tlb_observer observer);
int bc250_gmc_flush_gpu_tlb_observed(struct amdgpu_device *adev, u32 vmid, u32 vmhub,
				   u32 flush_type, bc250_tlb_observer observer);


/*
 * Point one VMID's page directory at pd_phys and invalidate that VMID (ADR 0008 stage C).
 *
 * pd_phys is a physical address with no flags in it; the valid bit is added here, which on this
 * part is the whole of what amdgpu_gmc_pd_addr() adds (see shim.c for why). It must be 4 KB
 * aligned. vmid is 1..15: VMID 0 is the GART aperture the driver's own rings, MQDs and fence pages
 * live in, bc250_gmc_gart_enable() programmed its root, and re-pointing it would take that memory
 * out from under the CP, so it is refused.
 *
 * Returns 0, BC250_EINVAL on a bad argument with nothing written, or BC250_ETIME from the
 * invalidation. The caller serializes: this writes registers and polls, and nothing here locks.
 */
int bc250_gmc_set_vmid_pd(struct amdgpu_device *adev, u32 vmid, u64 pd_phys, u32 flush_type);

/*
 * Read one VMID's page-table base pair (LO32 | HI32 << 32) as the hardware holds it, the valid bit
 * included. vmid is 0..15. Returns 0, or BC250_EINVAL with *value 0 before the hub is initialized.
 * The driver reads all sixteen once, before its first write to any of them, to find a VMID that
 * something other than this driver has programmed (driver/kmd/vmid_pool.h).
 */
int bc250_gmc_get_vmid_pd(struct amdgpu_device *adev, u32 vmid, u64 *value);

#endif /* BC250_GMC_H */
