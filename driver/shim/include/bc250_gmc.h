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

#endif /* BC250_GMC_H */
