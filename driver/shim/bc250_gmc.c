/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * GART bring-up for Cyan Skillfish. See include/bc250_gmc.h.
 *
 * Every piece below names the upstream function it mirrors, at kernel tag v6.18, commit
 * 7d0a66e4bb9081d75c82ec4957c50034cb0ea449 (driver/amdgpu-import/PROVENANCE.md), with the one
 * place where unit A's kernel differs called out. The register-writing work itself is the
 * unmodified AMD code in driver/amdgpu-import/; this file only decides what it is given and in
 * which order it runs.
 */
#include "bc250_gmc.h"
#include "nv.h"
#include "gfxhub_v2_0.h"
#include "mmhub_v2_0.h"

/* Our own file, so register offsets come through the SOC15 macros over AMD's headers. */
#include <nbio_2_3_offset.h>
#include "soc15_common.h"

#define BC250_FOUR_GB		0x100000000ULL
#define BC250_GART_ENGINE	17u	/* gmc_v10_0_flush_gpu_tlb: "Use register 17 for GART" */

static u64 bc250_align_up(u64 value, u64 alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

int bc250_gmc_setup(struct amdgpu_device *adev, const struct bc250_gmc_inputs *in,
		    struct amdgpu_bo *gart_bo)
{
	u64 max_mc_address, size_af, size_bf;

	if (adev == NULL || in == NULL || gart_bo == NULL)
		return BC250_EINVAL;

	/* Register bases: the imported cyan_skillfish_reg_base_init() over AMD's
	 * cyan_skillfish_ip_offset.h. Nothing in this driver knows a register address. */
	cyan_skillfish_reg_base_init(adev);

	/* IP versions, amdgpu_discovery.c, case CHIP_CYAN_SKILLFISH without
	 * AMD_APU_IS_CYAN_SKILLFISH2 - the branch unit A takes (dmesg names CYAN_SKILLFISH
	 * 0x1002:0x13FE and lists exactly that branch's fixed IP blocks). */
	adev->ip_versions[GC_HWIP][0] = IP_VERSION(10, 1, 3);
	adev->ip_versions[MMHUB_HWIP][0] = IP_VERSION(2, 0, 3);

	/* amdgpu_device_init(): the polling budget every wait loop in amdgpu uses. */
	adev->usec_timeout = 100000;

	/* gmc_v10_0_set_gfxhub_funcs() / gmc_v10_0_set_mmhub_funcs(): GC 10.1.3 and MMHUB 2.0.3
	 * both fall into the default branch. */
	adev->gfxhub.funcs = &gfxhub_v2_0_funcs;
	adev->mmhub.funcs = &mmhub_v2_0_funcs;

	/* gmc_v10_0_sw_init() calls these before anything touches the hubs: they only fill in
	 * adev->vmhub[] with register offsets and distances. */
	adev->gfxhub.funcs->init(adev);
	adev->mmhub.funcs->init(adev);

	/* gmc_v10_0_mc_init(): VRAM size in MB, from the register amdgpu reads through
	 * nbio_v2_3_get_memsize(). */
	adev->gmc.mc_vram_size =
		(u64)RREG32_SOC15(NBIO, 0, mmRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE) * 1024ULL * 1024ULL;
	adev->gmc.real_vram_size = adev->gmc.mc_vram_size;
	if (adev->gmc.mc_vram_size == 0)
		return BC250_EINVAL;    /* the backend is not reaching the device */

	/* gmc_v10_0_mc_init(): amdgpu_gart_size defaults to -1 and GC 10.1.3 takes the default
	 * branch of the switch. */
	adev->gmc.gart_size = 512ULL << 20;

	/* gmc_v10_0_sw_init(): 48 bit MC address space. */
	adev->gmc.mc_mask = 0xffffffffffffULL;

	/* gmc_v10_0_vram_gtt_location(), in its order. */

	/* amdgpu_gmc_set_agp_default(): AGP is disabled by making the start larger than the end.
	 * amdgpu_gmc_agp_location() is deliberately not called - gmc_v10_0_vram_gtt_location()
	 * guards it with `amdgpu_agp == 1` and that module parameter defaults to -1 (auto). */
	adev->gmc.agp_start = 0xffffffffffffULL;
	adev->gmc.agp_end = 0;
	adev->gmc.agp_size = 0;

	/* amdgpu_gmc_vram_location() with base = the gfxhub's get_fb_location(), which reads
	 * GCMC_VM_FB_LOCATION_BASE. This unit has no xgmi nodes, so fb_* follows vram_*. */
	adev->gmc.vram_start = adev->gfxhub.funcs->get_fb_location(adev);
	adev->gmc.vram_end = adev->gmc.vram_start + adev->gmc.mc_vram_size - 1;
	adev->gmc.fb_start = adev->gmc.vram_start;
	adev->gmc.fb_end = adev->gmc.vram_end;

	/* amdgpu_gmc_gart_location(adev, mc, AMDGPU_GART_PLACEMENT_BEST_FIT) */
	max_mc_address = min(adev->gmc.mc_mask, AMDGPU_GMC_HOLE_START - 1);
	size_bf = adev->gmc.fb_start;
	size_af = max_mc_address + 1 - bc250_align_up(adev->gmc.fb_end + 1, BC250_FOUR_GB);
	if (adev->gmc.gart_size > max(size_bf, size_af))
		adev->gmc.gart_size = max(size_bf, size_af);
	if ((size_bf >= adev->gmc.gart_size && size_bf < size_af) ||
	    (size_af < adev->gmc.gart_size))
		adev->gmc.gart_start = 0;
	else
		adev->gmc.gart_start = max_mc_address - adev->gmc.gart_size + 1;
	adev->gmc.gart_start &= ~(BC250_FOUR_GB - 1);
	adev->gmc.gart_end = adev->gmc.gart_start + adev->gmc.gart_size - 1;

	/* base offset of vram pages: the gfxhub's get_mc_fb_offset(), which reads
	 * GCMC_VM_FB_OFFSET. */
	adev->vm_manager.vram_base_offset = adev->gfxhub.funcs->get_mc_fb_offset(adev);

	/* gmc_v10_0_sw_init(): amdgpu_vm_adjust_size(adev, 256 * 1024, 9, 3, 48). With
	 * amdgpu_vm_size = -1 the clamp has equal bounds (min_vm_size == max_size == 262144 GB), so
	 * the amount of system RAM does not enter into it: max_pfn = 262144 << 18, then
	 * num_level = min(3, DIV_ROUND_UP(fls64(max_pfn) - 1, 9) - 1) = min(3, 3) = 3, block size 9. */
	adev->vm_manager.max_pfn = (u64)(256 * 1024) << 18;
	adev->vm_manager.num_level = 3;
	adev->vm_manager.block_size = 9;
	adev->vm_manager.fragment_size = 9;

	/* gmc.translate_further is assigned nowhere outside gmc_v9_0.c, so on GMC v10 it stays 0. */
	adev->gmc.translate_further = false;

	/* Policy, from the caller: see struct bc250_gmc_inputs. */
	adev->gmc.noretry = in->noretry;

	/* Clock gating is a separate IP callback, not part of the GART path. */
	adev->cg_flags = 0;

	/* The buffers somebody else allocated. gart.table_size is amdgpu's
	 * gart.num_gpu_pages * 8 (gmc_v10_0_gart_init). */
	gart_bo->adev = adev;
	gart_bo->gpu_addr = in->gart_table_mc;
	adev->gart.bo = gart_bo;
	adev->gart.table_size = (adev->gmc.gart_size / 4096ULL) * 8ULL;

	adev->mem_scratch.gpu_addr = in->mem_scratch_mc;
	adev->dummy_page_addr = in->dummy_page_dma;

	return 0;
}

/*
 * gmc_v10_0_flush_gpu_tlb(), reduced to its register traffic.
 *
 *   - the HDP flush it starts with is a no-op on this unit: amdgpu_device_flush_hdp() returns
 *     early when AMD_IS_APU is set, and unit A is an APU;
 *   - the KIQ path is not taken, there is no ring during bring-up;
 *   - the semaphore is used for MMHUB only (gmc_v10_0_use_invalidate_semaphore);
 *   - the dummy read of the request register applies to GFXHUB below GC 10.3, to let the ACK
 *     register clear before it is polled.
 *
 * Deviation from Linux: upstream holds adev->gmc.invalidate_lock across this. The shim has no
 * locks yet; the miniport has to serialize calls itself until it does.
 */
int bc250_gmc_flush_gpu_tlb_observed(struct amdgpu_device *adev, u32 vmid, u32 vmhub,
				   u32 flush_type, bc250_tlb_observer observer)
{
	struct amdgpu_vmhub *hub;
	bool use_semaphore;
	bool semaphore_taken = false;
	u32 inv_req, sem, req, ack, sample = 0;
	unsigned int i;
	int r = 0;

	if (adev == NULL || vmhub >= AMDGPU_MAX_VMHUBS)
		return BC250_EINVAL;
	hub = &adev->vmhub[vmhub];
	if (hub->vmhub_funcs == NULL)
		return BC250_EINVAL;    /* the hub's init() has not run */

	use_semaphore = (vmhub == AMDGPU_MMHUB0(0));
	inv_req = hub->vmhub_funcs->get_invalidate_req(vmid, flush_type);
	sem = hub->vm_inv_eng0_sem + hub->eng_distance * BC250_GART_ENGINE;
	req = hub->vm_inv_eng0_req + hub->eng_distance * BC250_GART_ENGINE;
	ack = hub->vm_inv_eng0_ack + hub->eng_distance * BC250_GART_ENGINE;

	/* "It may lose gpuvm invalidate acknowledge state across power-gating off cycle": take the
	 * semaphore before invalidating and release it afterwards. A read returning 1 is an
	 * acquire - which is also why reading these registers is forbidden outside this path
	 * (facts M25). */
	if (use_semaphore) {
		for (i = 0; i < adev->usec_timeout; i++) {
			if (RREG32(sem) & 0x1) {
				semaphore_taken = true;
				break;
			}
			bc250_shim_udelay(1);
		}
		if (!semaphore_taken) {
			/* Upstream logs and carries on; the invalidation below still has to happen,
			 * and the semaphore still has to be written back to 0 afterwards, because a
			 * read may have acquired it even though none returned 1. */
			dev_err(adev->dev, "Timeout waiting for sem acquire in VM flush!\n");
			r = BC250_ETIME;
		}
	}

	WREG32(req, inv_req);
	if (observer) observer(adev, "after-invalidate-request", inv_req);

	if (vmhub == AMDGPU_GFXHUB(0) &&
	    amdgpu_ip_version(adev, GC_HWIP, 0) < IP_VERSION(10, 3, 0))
	{
		sample = RREG32(req);
		if (observer) observer(adev, "after-invalidate-request-read", sample);
	}

	for (i = 0; i < adev->usec_timeout; i++) {
		sample = RREG32(ack);
		if (sample & (1u << vmid))
			break;
		bc250_shim_udelay(1);
	}
	if (observer) observer(adev, "after-invalidate-ack", sample);
	if (i >= adev->usec_timeout) {
		dev_err(adev->dev, "Timeout waiting for VM flush hub: %d!\n", (int)vmhub);
		if (r == 0)
			r = BC250_ETIME;
	}

	/* Released on every path that reached the invalidation, the acknowledge timeout included:
	 * upstream releases it unconditionally, and leaving it held would wedge every later flush. */
	if (use_semaphore)
		WREG32(sem, 0);

	return r;
}

int bc250_gmc_flush_gpu_tlb(struct amdgpu_device *adev, u32 vmid, u32 vmhub, u32 flush_type)
{
	return bc250_gmc_flush_gpu_tlb_observed(adev, vmid, vmhub, flush_type, NULL);
}

/*
 * amdgpu_vm_flush() (amdgpu_vm.c), reduced to the two steps that touch this hardware when a VMID
 * has to be pointed at a different page directory: the hub's setup_vm_pt_regs, which is
 * gfxhub_v2_0_setup_vm_pt_regs() (gfxhub_v2_0.c:120-132), and the invalidation of that VMID.
 *
 * Deviation from Linux (ADR 0008 stage C): upstream does the second step on the ring, through
 * gmc_v10_0_emit_flush_gpu_tlb() and gfx_v10_0_ring_emit_vm_flush()
 * (driver/amdgpu-import/reference/gfx_v10_0.c:8767), and the first one through the same packets.
 * Here both are MMIO, ahead of the ring write: the miniport serializes every submission under one
 * lock at PASSIVE_LEVEL, so nothing of ours can be executing while these registers move, and two
 * register writes plus one poll are a great deal less to be wrong about than a packet sequence
 * nothing has replayed. If an on-ring flush is ever added, PACKET3_PFP_SYNC_ME (nvd.h:322) belongs
 * with it on a gfx-type ring - upstream emits it there and only there.
 *
 * The value: amdgpu_gmc_pd_addr() is the page directory's physical address with AMDGPU_PTE_VALID
 * and nothing else on this part (shim.c:57-80), so the caller passes the address and this adds the
 * bit. Everything else about the context - depth, block size, the address range, the fault
 * defaults - gfxhub_v2_0_setup_vmid_config() already wrote for VMIDs 1..15 inside
 * bc250_gmc_gart_enable() (gfxhub_v2_0.c:283-330), and none of it changes per submission.
 */
int bc250_gmc_set_vmid_pd(struct amdgpu_device *adev, u32 vmid, u64 pd_phys, u32 flush_type)
{
	if (adev == NULL || adev->gfxhub.funcs == NULL ||
	    adev->gfxhub.funcs->setup_vm_pt_regs == NULL)
		return BC250_EINVAL;
	if (adev->vmhub[AMDGPU_GFXHUB(0)].vmhub_funcs == NULL)
		return BC250_EINVAL;    /* the hub's init() has not run, so its offsets are 0 */

	/* VMID 0 is the system domain (gfxhub_v2_0_enable_system_domain(), gfxhub_v2_0.c:254): page
	 * table depth 0, i.e. the flat GART aperture that every buffer this driver owns is addressed
	 * through. Its root is not ours to move. */
	if (vmid == 0 || vmid >= AMDGPU_NUM_VMID)
		return BC250_EINVAL;

	/* A page directory is a page. Upstream never checks this because the address comes out of a
	 * buffer object; here it comes from VidMm through an escape or a DDI, and the low bits of the
	 * register are not address bits. */
	if ((pd_phys & (AMDGPU_GPU_PAGE_SIZE - 1)) != 0)
		return BC250_EINVAL;

	adev->gfxhub.funcs->setup_vm_pt_regs(adev, vmid, pd_phys | AMDGPU_PTE_VALID);

	/* GFXHUB only: this driver's submissions are CP work, and the MMHUB copy of the context would
	 * only matter to an engine behind it. That also keeps the M25 semaphore hazard out of the
	 * path - bc250_gmc_flush_gpu_tlb() takes the semaphore for MMHUB0 and for nothing else. */
	return bc250_gmc_flush_gpu_tlb(adev, vmid, AMDGPU_GFXHUB(0), flush_type);
}

/*
 * gmc_v10_0_gart_enable(). adev->in_s0ix is false: this is a cold start, not a resume.
 * amdgpu_gtt_mgr_recover() and the DRM_INFO at the end touch no register.
 */
int bc250_gmc_gart_configure_observed(struct amdgpu_device *adev, bc250_tlb_observer observer)
{
	int r;

	if (adev == NULL || adev->gart.bo == NULL)
		return BC250_EINVAL;

	r = adev->gfxhub.funcs->gart_enable(adev);
	if (observer) observer(adev,"startup-after-gfxhub-enable",(u32)r);
	if (r)
		return r;

	r = adev->mmhub.funcs->gart_enable(adev);
	if (observer) observer(adev,"startup-after-mmhub-enable",(u32)r);
	if (r)
		return r;

	/* Upstream: adev->hdp.funcs->init_registers(adev), then amdgpu_device_flush_hdp(adev, NULL).
	 * HDP is not imported yet. Its only write here is HDP_MISC_CNTL, and the flush is a no-op on
	 * an APU. */

	/* value = amdgpu_vm_fault_stop != AMDGPU_VM_FAULT_STOP_ALWAYS; the module parameter defaults
	 * to AMDGPU_VM_FAULT_STOP_NEVER, so value is true: faults go to the default page instead of
	 * stopping the engine. */
	adev->gfxhub.funcs->set_fault_enable_default(adev, true);
	adev->mmhub.funcs->set_fault_enable_default(adev, true);
	if (observer) observer(adev,"startup-after-fault-defaults",0);
	return 0;
}

/* Keep the established complete enable contract. Configuration alone is not
 * translation readiness; a staged caller must complete the required flushes
 * before admitting consumers. No KMD caller uses configuration alone yet. */
int bc250_gmc_gart_enable_observed(struct amdgpu_device *adev, bc250_tlb_observer observer)
{
	int r = bc250_gmc_gart_configure_observed(adev, observer);
	if (r) return r;

	/* Upstream calls these two for their side effect and ignores the result; a flush timeout is
	 * not a reason to fail the bring-up, and on unit A amdgpu did time out here (the pre-driver
	 * sweep had taken the MMHUB semaphore, facts M25) and went on to a working GPU. The code is
	 * logged by bc250_gmc_flush_gpu_tlb itself; a caller that wants it calls that directly. */
	r = bc250_gmc_flush_gpu_tlb(adev, 0, AMDGPU_MMHUB0(0), 0);
	if (observer) observer(adev,"startup-after-mmhub-flush",(u32)r);
	r = bc250_gmc_flush_gpu_tlb_observed(adev, 0, AMDGPU_GFXHUB(0), 0, observer);
	if (observer) observer(adev,"startup-after-gfxhub-flush",(u32)r);

	return 0;
}

int bc250_gmc_gart_enable(struct amdgpu_device *adev)
{
	return bc250_gmc_gart_enable_observed(adev,NULL);
}

/* gmc_v10_0_gart_disable(), with adev->in_s0ix false. */
void bc250_gmc_gart_disable(struct amdgpu_device *adev)
{
	adev->gfxhub.funcs->gart_disable(adev);
	adev->mmhub.funcs->gart_disable(adev);
}
