/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The interrupt ring. See include/bc250_ih.h for what this is, what runs in a DPC and what is
 * deliberately left out.
 *
 * Follows drivers/gpu/drm/amd/amdgpu/navi10_ih.c and amdgpu_ih.c at v6.18, commit
 * 7d0a66e4bb9081d75c82ec4957c50034cb0ea449 (driver/amdgpu-import/reference/).
 */
#include "bc250_ih.h"
#include "bc250_gmc.h"		/* BC250_EINVAL, BC250_ETIME */
#include "bc250_nbio.h"

#include <oss/osssys_5_0_0_offset.h>
#include <oss/osssys_5_0_0_sh_mask.h>
#include "soc15_common.h"
#include "soc15_ih_clientid.h"
#include "irqsrcs_gfx_10_1.h"
#include "irqsrcs_sdma0_5_0.h"
#include "irqsrcs_sdma1_5_0.h"

/* amdgpu_ih.c:48 amdgpu_ih_ring_init(). order_base_2(ring_size / 4); the ring size is a power of
 * two by construction here, so upstream's re-alignment of it is an identity and is left out. */
static u32 bc250_ih_rb_size_field(u32 ring_size)
{
	u32 dwords = ring_size / 4u;
	u32 order = 0;

	while ((1u << order) < dwords)
		order++;
	return order;
}

/* navi10_ih.c:49 navi10_ih_init_register_offset(), ring 0 only: ih1 and ih2 have ring_size 0, so
 * upstream's two other blocks are dead here (see bc250_ih.h).
 *
 * Resolving the offsets once, into the ring, is upstream's own arrangement and not an invention -
 * navi10_ih.c reads them back out of ih->ih_regs everywhere, including in set_rptr. It matters twice
 * over here: it is what lets the DPC trio run against a struct amdgpu_device that has nothing in it
 * but irq.ih and a backend, because SOC15_REG_OFFSET() resolves through adev->reg_offset and a
 * DPC-only device has no reason to carry that table.
 *
 * psp_reg_id is upstream's and is left out: the only reader is the SR-IOV psp_reg_program() path. */
static void bc250_ih_init_register_offset(struct amdgpu_device *adev)
{
	struct amdgpu_ih_regs *r = &adev->irq.ih.ih_regs;

	r->ih_rb_base = SOC15_REG_OFFSET(OSSSYS, 0, mmIH_RB_BASE);
	r->ih_rb_base_hi = SOC15_REG_OFFSET(OSSSYS, 0, mmIH_RB_BASE_HI);
	r->ih_rb_cntl = SOC15_REG_OFFSET(OSSSYS, 0, mmIH_RB_CNTL);
	r->ih_rb_wptr = SOC15_REG_OFFSET(OSSSYS, 0, mmIH_RB_WPTR);
	r->ih_rb_rptr = SOC15_REG_OFFSET(OSSSYS, 0, mmIH_RB_RPTR);
	r->ih_doorbell_rptr = SOC15_REG_OFFSET(OSSSYS, 0, mmIH_DOORBELL_RPTR);
	r->ih_rb_wptr_addr_lo = SOC15_REG_OFFSET(OSSSYS, 0, mmIH_RB_WPTR_ADDR_LO);
	r->ih_rb_wptr_addr_hi = SOC15_REG_OFFSET(OSSSYS, 0, mmIH_RB_WPTR_ADDR_HI);
}

/* navi10_ih.c:213 navi10_ih_rb_cntl().
 *
 * MC_SPACE is 4 and not 1 because the ring is GTT: upstream picks `ih->use_bus_addr ? 1 : 4`, and
 * use_bus_addr is false on an APU loading firmware through the PSP (navi10_ih.c:568-572). The
 * traced value 0xC03101A0 has MC_SPACE = 4, which confirms it on unit A. */
static u32 bc250_ih_rb_cntl(struct amdgpu_device *adev, u32 ih_rb_cntl)
{
	u32 rb_bufsz = bc250_ih_rb_size_field(adev->irq.ih.ring_size);

	ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, MC_SPACE, 4);
	ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 1);
	ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, WPTR_OVERFLOW_ENABLE, 1);
	ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, RB_SIZE, rb_bufsz);
	/* Writeback of the write pointer is what makes the DPC able to read it from memory instead of
	 * from a register; the fast path of bc250_ih_get_wptr() depends on it. */
	ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, WPTR_WRITEBACK_ENABLE, 1);
	ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, MC_SNOOP, 1);
	ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, MC_RO, 0);
	ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, MC_VMID, 0);

	return ih_rb_cntl;
}

/* navi10_ih.c:236 navi10_ih_doorbell_rptr(). */
static u32 bc250_ih_doorbell_rptr(struct amdgpu_device *adev)
{
	struct amdgpu_ih_ring *ih = &adev->irq.ih;
	u32 v = 0;

	if (ih->use_doorbell) {
		v = REG_SET_FIELD(v, IH_DOORBELL_RPTR, OFFSET, ih->doorbell_index);
		v = REG_SET_FIELD(v, IH_DOORBELL_RPTR, ENABLE, 1);
	} else {
		v = REG_SET_FIELD(v, IH_DOORBELL_RPTR, ENABLE, 0);
	}
	return v;
}

/* navi10_ih.c:152 navi10_ih_toggle_ring_interrupts(), ring 0 only.
 *
 * ENABLE_INTR is upstream's "only valid in ring0" field, and ring 0 is the only ring here. */
static void bc250_ih_toggle_interrupts(struct amdgpu_device *adev, bool enable)
{
	struct amdgpu_ih_ring *ih = &adev->irq.ih;
	u32 tmp = RREG32(ih->ih_regs.ih_rb_cntl);

	tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, RB_ENABLE, enable ? 1 : 0);
	tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, RB_GPU_TS_ENABLE, 1);
	tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, ENABLE_INTR, enable ? 1 : 0);
	WREG32(ih->ih_regs.ih_rb_cntl, tmp);

	if (!enable) {
		WREG32(ih->ih_regs.ih_rb_rptr, 0);
		WREG32(ih->ih_regs.ih_rb_wptr, 0);
		ih->rptr = 0;
	}
	ih->enabled = enable;
}

/* navi10_ih.c:263 navi10_ih_enable_ring(), ring 0, non-SR-IOV arm. */
static void bc250_ih_enable_ring(struct amdgpu_device *adev)
{
	struct amdgpu_ih_ring *ih = &adev->irq.ih;
	u32 tmp;

	/* "[39:8] of the 40-bit address of the beginning of the ring buffer" */
	WREG32(ih->ih_regs.ih_rb_base, (u32)(ih->gpu_addr >> 8));
	WREG32(ih->ih_regs.ih_rb_base_hi, (u32)((ih->gpu_addr >> 40) & 0xff));

	tmp = RREG32(ih->ih_regs.ih_rb_cntl);
	tmp = bc250_ih_rb_cntl(adev, tmp);
	tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, RPTR_REARM, adev->irq.msi_enabled ? 1 : 0);
	WREG32(ih->ih_regs.ih_rb_cntl, tmp);

	/* The write-back slot, "whether it's enabled or not". HI is masked to 16 bits upstream, and
	 * note that HI sits at a LOWER register offset than LO - that is AMD's layout, not a slip. */
	WREG32(ih->ih_regs.ih_rb_wptr_addr_lo, lower_32_bits(ih->wptr_addr));
	WREG32(ih->ih_regs.ih_rb_wptr_addr_hi, upper_32_bits(ih->wptr_addr) & 0xFFFF);

	WREG32(ih->ih_regs.ih_rb_wptr, 0);
	WREG32(ih->ih_regs.ih_rb_rptr, 0);

	WREG32(ih->ih_regs.ih_doorbell_rptr, bc250_ih_doorbell_rptr(adev));
}

/* navi10_ih.c:317 navi10_ih_irq_init(), in its order. */
int bc250_ih_hw_init(struct amdgpu_device *adev)
{
	struct amdgpu_ih_ring *ih;

	if (adev == NULL)
		return BC250_EINVAL;
	ih = &adev->irq.ih;
	if (ih->ring == NULL || ih->ring_size == 0 || ih->wptr_cpu == NULL)
		return BC250_EINVAL;      /* bc250_ih_setup() has not run */
	if (ih->ih_regs.ih_rb_cntl == 0)
		return BC250_EINVAL;      /* nor did its register-offset init; offset 0 is not a register */
	if (adev->dummy_page_addr == 0)
		return BC250_EINVAL;      /* nbio_v2_3_ih_control() would point the dummy read at 0 */

	/* Disable first, as upstream does: the block may be live from an earlier bring-up. */
	bc250_ih_toggle_interrupts(adev, false);

	bc250_nbio_ih_control(adev);

	bc250_ih_enable_ring(adev);

	bc250_nbio_ih_doorbell_range(adev, ih->use_doorbell, (int)ih->doorbell_index);

	/* pci_set_master() is upstream's next line and is not ours; see the header. */

	bc250_ih_toggle_interrupts(adev, true);

	return 0;
}

/* navi10_ih.c:386 navi10_ih_irq_disable(). The 1 ms is upstream's "wait and acknowledge irq"; it
 * is the reason this is not DPC-safe. */
void bc250_ih_hw_fini(struct amdgpu_device *adev)
{
	if (adev == NULL || adev->irq.ih.ring == NULL)
		return;

	bc250_ih_toggle_interrupts(adev, false);
	bc250_shim_udelay(1000);
}

/* ---------------------------------------------------------------------------------------------
 * The DPC trio
 * ------------------------------------------------------------------------------------------- */

/* navi10_ih.c:406 navi10_ih_get_wptr(). */
u32 bc250_ih_get_wptr(struct amdgpu_device *adev, bool *overflowed)
{
	struct amdgpu_ih_ring *ih;
	u32 wptr, tmp;

	if (overflowed != NULL)
		*overflowed = false;
	if (adev == NULL || adev->irq.ih.wptr_cpu == NULL)
		return 0;
	ih = &adev->irq.ih;

	/* The fast path, and the reason WPTR_WRITEBACK_ENABLE is set: no register read at all. */
	wptr = *ih->wptr_cpu;
	if (!REG_GET_FIELD(wptr, IH_RB_WPTR, RB_OVERFLOW))
		return wptr & ih->ptr_mask;

	/* "Double check that the overflow wasn't already cleared." The write-back can be stale. */
	wptr = RREG32(ih->ih_regs.ih_rb_wptr);
	if (!REG_GET_FIELD(wptr, IH_RB_WPTR, RB_OVERFLOW))
		return wptr & ih->ptr_mask;

	wptr = REG_SET_FIELD(wptr, IH_RB_WPTR, RB_OVERFLOW, 0);

	/* Upstream: "When a ring buffer overflow happen start parsing interrupt from the last not
	 * overwritten vector (wptr + 32)." One whole entry past the write pointer is the oldest entry
	 * the hardware has not yet trampled. Interrupts between the old rptr and here are lost, which
	 * is what an overflow means; the caller is told so it can count them. */
	ih->rptr = (wptr + 32u) & ih->ptr_mask;
	if (overflowed != NULL)
		*overflowed = true;

	/* The acknowledge is a PULSE: set WPTR_OVERFLOW_CLEAR, then clear it again immediately, or the
	 * bit stays latched and no later overflow is ever reported. Both writes are required. */
	tmp = RREG32(ih->ih_regs.ih_rb_cntl);
	tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 1);
	WREG32(ih->ih_regs.ih_rb_cntl, tmp);
	tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 0);
	WREG32(ih->ih_regs.ih_rb_cntl, tmp);

	return wptr & ih->ptr_mask;
}

/* amdgpu_ih.c:263 amdgpu_ih_decode_iv_helper(), field for field.
 *
 * Deviation: the read pointer is the caller's, see the header. Upstream's le32_to_cpu() is an
 * identity on x86-64 and every Windows target of this driver, so it is not written out. */
int bc250_ih_decode(struct amdgpu_device *adev, u32 *rptr, struct bc250_iv_entry *out)
{
	const volatile u32 *ring;
	u32 dw[8], i, index;

	if (adev == NULL || rptr == NULL || out == NULL)
		return BC250_EINVAL;
	if (adev->irq.ih.ring == NULL)
		return BC250_EINVAL;

	/* wptr/rptr are in bytes */
	index = (*rptr & adev->irq.ih.ptr_mask) >> 2;
	ring = (const volatile u32 *)adev->irq.ih.ring;
	for (i = 0; i < 8; i++)
		dw[i] = ring[index + i];

	out->client_id = dw[0] & 0xff;
	out->src_id = (dw[0] >> 8) & 0xff;
	out->ring_id = (dw[0] >> 16) & 0xff;
	out->vmid = (dw[0] >> 24) & 0xf;
	out->vmid_src = dw[0] >> 31;
	out->timestamp = dw[1] | ((u64)(dw[2] & 0xffff) << 32);
	out->timestamp_src = dw[2] >> 31;
	out->pasid = dw[3] & 0xffff;
	out->node_id = (dw[3] >> 16) & 0xff;
	out->src_data[0] = dw[4];
	out->src_data[1] = dw[5];
	out->src_data[2] = dw[6];
	out->src_data[3] = dw[7];

	*rptr = (*rptr + 32u) & adev->irq.ih.ptr_mask;
	return 0;
}

/* navi10_ih.c:488 navi10_ih_set_rptr(), doorbell arm. The SR-IOV rearm is not taken. */
void bc250_ih_set_rptr(struct amdgpu_device *adev, u32 rptr)
{
	struct amdgpu_ih_ring *ih;

	if (adev == NULL)
		return;
	ih = &adev->irq.ih;
	ih->rptr = rptr & ih->ptr_mask;

	if (ih->use_doorbell) {
		if (ih->rptr_cpu != NULL)
			*ih->rptr_cpu = ih->rptr;
		/* WDOORBELL32, not 64: upstream's navi10_ih.c:499 is a 32-bit store, and this is the
		 * only place in the whole driver that uses the narrow one. See bc250_shim.h. */
		bc250_shim_wdoorbell32(adev, ih->doorbell_index, ih->rptr);
	} else {
		WREG32(ih->ih_regs.ih_rb_rptr, ih->rptr);
	}
}

/* ---------------------------------------------------------------------------------------------
 * Routing
 * ------------------------------------------------------------------------------------------- */

/* gfx_v10_0.c:9195-9197, the only statement of this packing in the kernel:
 *     me_id    = (entry->ring_id & 0x0c) >> 2;
 *     pipe_id  = (entry->ring_id & 0x03) >> 0;
 *     queue_id = (entry->ring_id & 0x70) >> 4;
 *
 * The queue field is not always the queue that caused the vector. Measured on unit A (facts M45,
 * evidence/windows/2026-09-21-E12-run-002/ih-state-afterfini-153128.txt): the eight source 181
 * vectors the UNMAP_QUEUES of a teardown produces carry ring ids 20, 21, 22, 7, 20, 21, 22, 7 for
 * eight distinct queues, which is the queue LAST ACTIVE on each pipe rather than the queue the
 * packet named, and they carry src_data[0] = 1 where a fence carries 0x80000000. So me and pipe can
 * be trusted; the queue field can be used to attribute a vector to one ring only where the vector
 * is known to be a fence. Upstream would mis-attribute here - gfx_v10_0_eop_irq() calls
 * amdgpu_fence_process() on whichever compute ring matches all three fields - but it does not
 * matter to it, because amdgpu_fence_process() only advances a fence sequence that has already been
 * signalled in memory. The routing helpers below look at me only, and are unaffected. */
void bc250_ih_eop_ring_id(const struct bc250_iv_entry *e, u32 *me, u32 *pipe, u32 *queue)
{
	if (e == NULL)
		return;
	if (me != NULL)
		*me = (e->ring_id & 0x0cu) >> 2;
	if (pipe != NULL)
		*pipe = (e->ring_id & 0x03u) >> 0;
	if (queue != NULL)
		*queue = (e->ring_id & 0x70u) >> 4;
}

static bool bc250_ih_is_cp_eop(const struct bc250_iv_entry *e)
{
	return e != NULL && e->client_id == SOC15_IH_CLIENTID_GRBM_CP &&
	       e->src_id == GFX_10_1__SRCID__CP_EOP_INTERRUPT;
}

/* gfx_v10_0_eop_irq(), gfx_v10_0.c:9199-9203: me 0 is the graphics ME. A gfx and a compute EOP are
 * the same client and the same source id; only this tells them apart. */
bool bc250_ih_is_gfx_eop(const struct bc250_iv_entry *e)
{
	u32 me = 0;

	if (!bc250_ih_is_cp_eop(e))
		return false;
	bc250_ih_eop_ring_id(e, &me, NULL, NULL);
	return me == 0;
}

/* gfx_v10_0.c:9204-9217: me 1 and me 2 are MEC1 and MEC2. gfx_v10_0_compute_ring_init() sets
 * ring->me = mec + 1 (gfx_v10_0.c:4685), which is why MEC1 appears as me 1. */
bool bc250_ih_is_compute_eop(const struct bc250_iv_entry *e)
{
	u32 me = 0;

	if (!bc250_ih_is_cp_eop(e))
		return false;
	bc250_ih_eop_ring_id(e, &me, NULL, NULL);
	return me == 1 || me == 2;
}

/* gfx_v10_0.c:4868 registers the KIQ under CP_IB2_INTERRUPT_PKT. The name is the IV's; the enable
 * bit is CPC_INT_CNTL.GENERIC2_INT_ENABLE, which gfx_v10_0_kiq_set_interrupt_state() (:9411) is
 * explicit about - its default arm is BUG() with "kiq only support GENERIC2_INT now". bc250_irq.c
 * sets exactly that bit. */
bool bc250_ih_is_kiq(const struct bc250_iv_entry *e)
{
	return e != NULL && e->client_id == SOC15_IH_CLIENTID_GRBM_CP &&
	       e->src_id == GFX_10_1__SRCID__CP_IB2_INTERRUPT_PKT;
}

/* sdma_v5_0.c:1387-1399 registers one source under two client ids, and
 * sdma_v5_0_process_trap_irq() (:1727) switches on entry->client_id to pick the instance. The two
 * SRCID macros have the same value, 224; both are named here so the pairing is visible. */
bool bc250_ih_is_sdma_trap(const struct bc250_iv_entry *e, u32 *instance)
{
	if (e == NULL)
		return false;
	if (e->client_id == SOC15_IH_CLIENTID_SDMA0 && e->src_id == SDMA0_5_0__SRCID__SDMA_TRAP) {
		if (instance != NULL)
			*instance = 0;
		return true;
	}
	if (e->client_id == SOC15_IH_CLIENTID_SDMA1 && e->src_id == SDMA1_5_0__SRCID__SDMA_TRAP) {
		if (instance != NULL)
			*instance = 1;
		return true;
	}
	return false;
}

/* ---------------------------------------------------------------------------------------------
 * Setup and teardown
 * ------------------------------------------------------------------------------------------- */

/* amdgpu_ih.c:42 amdgpu_ih_ring_init(), the use_bus_addr == false arm: the ring is a GTT buffer and
 * the two shadows are slots in a separate page. Upstream takes them from the global write-back
 * pool; this driver has no such pool, so one GTT page holds both, which changes the addresses the
 * registers carry and nothing else. */
int bc250_ih_setup(struct amdgpu_device *adev, bool msi)
{
	struct amdgpu_ih_ring *ih;
	int r;

	if (adev == NULL)
		return BC250_EINVAL;

	ih = &adev->irq.ih;
	memset(ih, 0, sizeof(*ih));

	/* Before anything else: every register access below and in the DPC goes through these. */
	bc250_ih_init_register_offset(adev);

	r = bc250_shim_mem_alloc(adev, BC250_MEM_GTT, BC250_IH_RING_SIZE, AMDGPU_GPU_PAGE_SIZE,
				 &ih->ring_mem);
	if (r)
		return r;
	if (ih->ring_mem.cpu == NULL)
		return BC250_EINVAL;

	r = bc250_shim_mem_alloc(adev, BC250_MEM_GTT, AMDGPU_GPU_PAGE_SIZE, AMDGPU_GPU_PAGE_SIZE,
				 &ih->wb_mem);
	if (r)
		return r;
	if (ih->wb_mem.cpu == NULL)
		return BC250_EINVAL;

	ih->ring = (u32 *)ih->ring_mem.cpu;
	ih->gpu_addr = ih->ring_mem.mc;
	ih->ring_size = BC250_IH_RING_SIZE;
	ih->ptr_mask = ih->ring_size - 1u;      /* a BYTE mask: upstream's ptr_mask is in bytes */
	ih->rptr = 0;

	/* Two dwords of the page, the write pointer first. */
	ih->wptr_addr = ih->wb_mem.mc;
	ih->wptr_cpu = (volatile u32 *)ih->wb_mem.cpu;
	ih->rptr_addr = ih->wb_mem.mc + 4u;
	ih->rptr_cpu = ((volatile u32 *)ih->wb_mem.cpu) + 1;

	/* nv.c:584 nv_init_doorbell_index(), then navi10_ih.c:577-578 doubles it: the doorbell index
	 * amdgpu keeps is a qword index and the write path wants dwords. */
	ih->use_doorbell = true;
	adev->doorbell_index.ih = AMDGPU_NAVI10_DOORBELL_IH;
	ih->doorbell_index = adev->doorbell_index.ih << 1;

	adev->irq.msi_enabled = msi;
	return 0;
}

void bc250_ih_teardown(struct amdgpu_device *adev)
{
	if (adev == NULL)
		return;
	bc250_shim_mem_free(adev, &adev->irq.ih.ring_mem);
	bc250_shim_mem_free(adev, &adev->irq.ih.wb_mem);
	memset(&adev->irq.ih, 0, sizeof(adev->irq.ih));
}
