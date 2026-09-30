/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * driver/shim/include/amdgpu.h - the slice of the Linux/amdgpu API that our imported AMD IP-block
 * code needs (ADR 0002). It is deliberately the smallest thing the imports in
 * driver/amdgpu-import/ compile against: only the declarations those files actually touch.
 *
 * Two kinds of content live here and are marked as such:
 *
 *   [amdgpu] copied from AMD's MIT-licensed kernel sources, with the file it came from named.
 *            Kernel tag v6.18, commit 7d0a66e4bb9081d75c82ec4957c50034cb0ea449.
 *            Copyright the respective AMD copyright holders, MIT (see driver/amdgpu-import/PROVENANCE.md).
 *   [shim]   ours. Where the behaviour deviates from Linux, the comment says why (ADR 0002 rule:
 *            every deviation from Linux gets a comment stating the reason).
 *
 * Nothing GPL-only is copied in: helpers that live in GPL-2.0 kernel headers (lower_32_bits,
 * min/max, ARRAY_SIZE) are written here from their documented behaviour, not copied.
 *
 * The header compiles both as plain user-mode C (host replay test) and with the WDK kernel flags
 * the miniport uses; BC250_SHIM_KERNEL selects the kernel variant.
 */
#ifndef BC250_SHIM_AMDGPU_H
#define BC250_SHIM_AMDGPU_H

#include <stddef.h>         /* NULL, size_t - present in both the SDK and the WDK km CRT */
#include <string.h>         /* memcpy, memset - the km CRT has both */
#include "bc250_shim.h"

/* ---------------------------------------------------------------------------------------------
 * [shim] Fixed-width and kernel-style integer types.
 *
 * The WDK's km CRT has no <stdint.h> and no <stdbool.h>, so in kernel mode the names come from the
 * compiler's own types. `bool` is one byte in both modes so that a struct laid out by the shim has
 * the same shape in the host test and in the miniport.
 * ------------------------------------------------------------------------------------------- */
#if defined(BC250_SHIM_KERNEL)
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef __int64            int64_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned __int64   uint64_t;
#else
#include <stdint.h>
#endif

#ifndef bool
typedef unsigned char bc250_shim_bool;
#define bool  bc250_shim_bool
#define true  1
#define false 0
#endif

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* [shim] Linux's type for a bus resource address. Only amdgpu_doorbell.h names it. */
typedef uint64_t resource_size_t;

/* [shim] GCC attribute used by cyan_skillfish_ip_offset.h on its IP_BASE structs. */
#ifndef __maybe_unused
#define __maybe_unused
#endif

/* ---------------------------------------------------------------------------------------------
 * [shim] Small helpers that live in GPL-2.0 kernel headers upstream and are therefore rewritten
 * here rather than copied. min/max are plain ternaries instead of Linux's GNU statement
 * expressions (MSVC has none); the imports only ever pass plain member reads, so the double
 * evaluation a ternary macro implies cannot bite.
 * ------------------------------------------------------------------------------------------- */
#define lower_32_bits(n) ((u32)((u64)(n) & 0xFFFFFFFFu))
#define upper_32_bits(n) ((u32)(((u64)(n) >> 32) & 0xFFFFFFFFu))

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* [shim] log2.h's order_base_2(n) = ceil(log2(n)), with order_base_2(0) == 0 and
 * order_base_2(1) == 0. The GFX10 MQD builders call it on queue and buffer sizes, which are powers
 * of two, but the general definition is kept so that a non-power-of-two never rounds the wrong way.
 * Written from the documented behaviour, not copied: log2.h is GPL-2.0. */
static __inline u32 bc250_order_base_2(u64 n)
{
	u32 order = 0;

	if (n < 2)
		return 0;
	n--;
	while (n) {
		n >>= 1;
		order++;
	}
	return order;
}
#ifndef order_base_2
#define order_base_2(n) bc250_order_base_2((u64)(n))
#endif

/* [shim] bitops.h's hweight32: how many bits are set. Same reason as order_base_2. */
static __inline u32 bc250_hweight32(u32 v)
{
	u32 n = 0;

	while (v) {
		v &= v - 1u;
		n++;
	}
	return n;
}
#ifndef hweight32
#define hweight32(v) bc250_hweight32((u32)(v))
#endif

/* [shim] Logging. The imports call dev_err/dev_info/dev_warn with adev->dev first; the shim does
 * not model a struct device and passes the handle through unchanged. */
#define dev_err(dev, ...)  bc250_shim_log(2, (void *)(dev), __VA_ARGS__)
#define dev_warn(dev, ...) bc250_shim_log(1, (void *)(dev), __VA_ARGS__)
#define dev_info(dev, ...) bc250_shim_log(0, (void *)(dev), __VA_ARGS__)
#define DRM_ERROR(...)     bc250_shim_log(2, NULL, __VA_ARGS__)
#define DRM_WARN(...)      bc250_shim_log(1, NULL, __VA_ARGS__)
#define DRM_INFO(...)      bc250_shim_log(0, NULL, __VA_ARGS__)
#define pr_err(...)        bc250_shim_log(2, NULL, __VA_ARGS__)

/* ---------------------------------------------------------------------------------------------
 * [amdgpu] drivers/gpu/drm/amd/amdgpu/amdgpu.h - IP block identifiers and versions.
 * ------------------------------------------------------------------------------------------- */
enum amd_hw_ip_block_type {
	GC_HWIP = 1,
	HDP_HWIP,
	SDMA0_HWIP,
	SDMA1_HWIP,
	SDMA2_HWIP,
	SDMA3_HWIP,
	SDMA4_HWIP,
	SDMA5_HWIP,
	SDMA6_HWIP,
	SDMA7_HWIP,
	LSDMA_HWIP,
	MMHUB_HWIP,
	ATHUB_HWIP,
	NBIO_HWIP,
	MP0_HWIP,
	MP1_HWIP,
	UVD_HWIP,
	VCN_HWIP = UVD_HWIP,
	JPEG_HWIP = VCN_HWIP,
	VCN1_HWIP,
	VCE_HWIP,
	VPE_HWIP,
	DF_HWIP,
	DCE_HWIP,
	OSSSYS_HWIP,
	SMUIO_HWIP,
	PWR_HWIP,
	NBIF_HWIP,
	THM_HWIP,
	CLK_HWIP,
	UMC_HWIP,
	RSMU_HWIP,
	XGMI_HWIP,
	DCI_HWIP,
	PCIE_HWIP,
	ISP_HWIP,
	MAX_HWIP
};

#define HWIP_MAX_INSTANCE	44

#define IP_VERSION_FULL(mj, mn, rv, var, srev) \
	(((mj) << 24) | ((mn) << 16) | ((rv) << 8) | ((var) << 4) | (srev))
#define IP_VERSION(mj, mn, rv)		IP_VERSION_FULL(mj, mn, rv, 0, 0)

/* [amdgpu] amdgpu.h: register access flags. */
#define AMDGPU_REGS_NO_KIQ	(1<<1)
#define AMDGPU_REGS_RLC		(1<<2)

/* [amdgpu] amdgpu.h: field accessors over the *_sh_mask.h names. */
#define REG_FIELD_SHIFT(reg, field) reg##__##field##__SHIFT
#define REG_FIELD_MASK(reg, field) reg##__##field##_MASK

#define REG_SET_FIELD(orig_val, reg, field, field_val)			\
	(((orig_val) & ~REG_FIELD_MASK(reg, field)) |			\
	 (REG_FIELD_MASK(reg, field) & ((field_val) << REG_FIELD_SHIFT(reg, field))))

#define REG_GET_FIELD(value, reg, field)				\
	(((value) & REG_FIELD_MASK(reg, field)) >> REG_FIELD_SHIFT(reg, field))

/* [amdgpu] amdgpu_vm.h: which slot of adev->vmhub[] a hub lives in. */
#define AMDGPU_MAX_VMHUBS			13
#define AMDGPU_GFXHUB_START			0
#define AMDGPU_MMHUB0_START			8
#define AMDGPU_MMHUB1_START			12
#define AMDGPU_GFXHUB(x)			(AMDGPU_GFXHUB_START + (x))
#define AMDGPU_MMHUB0(x)			(AMDGPU_MMHUB0_START + (x))
#define AMDGPU_MMHUB1(x)			(AMDGPU_MMHUB1_START + (x))

/* [amdgpu] amdgpu_ids.h */
#define AMDGPU_NUM_VMID	16

/* [amdgpu] amdgpu_gmc.h: VA hole for 48 bit addresses. */
#define AMDGPU_GMC_HOLE_START	0x0000800000000000ULL
#define AMDGPU_GMC_HOLE_END	0xffff800000000000ULL

/* [amdgpu] amdgpu_vm.h:57-118, the page-table entry flags. The first six are what a GART PTE can
 * carry and are what driver/shim/bc250_gart.c uses; the rest belong to a multi-level VM page table
 * and are used only by driver/shim/bc250_pte.c, which builds entries for VMIDs 1..15. The GFX9 and
 * GFX12 variants are not taken over: neither applies to GC 10.1.3. */
#define AMDGPU_PTE_VALID	(1ULL << 0)
#define AMDGPU_PTE_SYSTEM	(1ULL << 1)
#define AMDGPU_PTE_SNOOPED	(1ULL << 2)
#define AMDGPU_PTE_TMZ		(1ULL << 3)
#define AMDGPU_PTE_EXECUTABLE	(1ULL << 4)
#define AMDGPU_PTE_READABLE	(1ULL << 5)
#define AMDGPU_PTE_WRITEABLE	(1ULL << 6)

/* amdgpu_vm.h:70. Five bits: the page covered by the entry is 1 << (12 + frag) bytes, and every
 * entry inside that range must carry the same flags and be physically contiguous
 * (amdgpu_vm_pt.c:738-743). */
#define AMDGPU_PTE_FRAG(x)	(((u64)(x) & 0x1fULL) << 7)

#define AMDGPU_PTE_PRT		(1ULL << 51)    /* amdgpu_vm.h:73, "T" in the NAVI10 format */
#define AMDGPU_PDE_PTE		(1ULL << 54)    /* amdgpu_vm.h:76, "P": this directory entry is a page */
#define AMDGPU_PTE_LOG		(1ULL << 55)    /* amdgpu_vm.h:78 */
#define AMDGPU_PTE_TF		(1ULL << 56)    /* amdgpu_vm.h:81, translate further */
#define AMDGPU_PTE_NOALLOC	(1ULL << 58)    /* amdgpu_vm.h:84 */

/* The memory type lives in bits 48..50 on GFX10. The enumerators themselves (MTYPE_UC and the rest)
 * are AMD's, in third_party/linux-amdgpu/navi10_enum.h, and are not repeated here. */
#define AMDGPU_PTE_MTYPE_NV10_SHIFT(mtype)	((u64)(mtype) << 48)
#define AMDGPU_PTE_MTYPE_NV10_MASK		AMDGPU_PTE_MTYPE_NV10_SHIFT(7ULL)
#define AMDGPU_PTE_MTYPE_NV10(flags, mtype)			\
	(((u64)(flags) & (~AMDGPU_PTE_MTYPE_NV10_MASK)) |	\
	  AMDGPU_PTE_MTYPE_NV10_SHIFT(mtype))

/* [amdgpu] amdgpu_gmc.c:169 amdgpu_gmc_set_pte_pde(): the address bits a PTE carries. Bits below 12
 * are the flags above; bits 48 and up are the memory type and the reserved fields. */
#define AMDGPU_PTE_ADDR_MASK	0x0000FFFFFFFFF000ULL

/* The same for a page DIRECTORY entry, whose address field reaches six bits lower (bits 47:6 in the
 * NAVI10 PDE format, gmc_v10_0.c:462). Upstream states it as an assertion rather than a mask -
 * BUG_ON(*addr & 0xFFFF00000000003FULL) at gmc_v10_0.c:474 - and this is that condition written the
 * other way round. */
#define AMDGPU_PDE_ADDR_MASK	0x0000FFFFFFFFFFC0ULL

/* [amdgpu] amd_shared.h - the two clock-gating flags and the state enum mmhub_v2_0.c uses.
 * amd_shared.h itself pulls in DRM headers, so only these three definitions are taken over. */
#define AMD_CG_SUPPORT_MC_LS	(1ULL << 8)
#define AMD_CG_SUPPORT_MC_MGCG	(1ULL << 9)

enum amd_clockgating_state {
	AMD_CG_STATE_GATE = 0,
	AMD_CG_STATE_UNGATE,
};

/* ---------------------------------------------------------------------------------------------
 * [amdgpu] amdgpu_gmc.h / amdgpu_gfxhub.h / amdgpu_mmhub.h - the hub descriptors.
 * struct amdgpu_vmhub is taken over in full so that the field names and the meaning of the
 * register-distance fields stay exactly those of the imported code.
 * ------------------------------------------------------------------------------------------- */
struct amdgpu_device;
struct cs_section_def;          /* driver/amdgpu-import/clearstate_defs.h */

struct amdgpu_vmhub_funcs {
	void (*print_l2_protection_fault_status)(struct amdgpu_device *adev,
						 uint32_t status);
	uint32_t (*get_invalidate_req)(unsigned int vmid, uint32_t flush_type);
};

struct amdgpu_vmhub {
	uint32_t	ctx0_ptb_addr_lo32;
	uint32_t	ctx0_ptb_addr_hi32;
	uint32_t	vm_inv_eng0_sem;
	uint32_t	vm_inv_eng0_req;
	uint32_t	vm_inv_eng0_ack;
	uint32_t	vm_context0_cntl;
	uint32_t	vm_l2_pro_fault_status;
	uint32_t	vm_l2_pro_fault_cntl;

	/*
	 * store the register distances between two continuous context domain
	 * and invalidation engine.
	 */
	uint32_t	ctx_distance;
	uint32_t	ctx_addr_distance; /* include LO32/HI32 */
	uint32_t	eng_distance;
	uint32_t	eng_addr_distance; /* include LO32/HI32 */

	uint32_t        vm_cntx_cntl;
	uint32_t	vm_cntx_cntl_vm_fault;
	uint32_t	vm_l2_bank_select_reserved_cid2;

	uint32_t	vm_contexts_disable;

	bool		sdma_invalidation_workaround;

	const struct amdgpu_vmhub_funcs *vmhub_funcs;
};

struct amdgpu_gfxhub_funcs {
	u64 (*get_fb_location)(struct amdgpu_device *adev);
	u64 (*get_mc_fb_offset)(struct amdgpu_device *adev);
	void (*setup_vm_pt_regs)(struct amdgpu_device *adev, uint32_t vmid,
			uint64_t page_table_base);
	int (*gart_enable)(struct amdgpu_device *adev);

	void (*gart_disable)(struct amdgpu_device *adev);
	void (*set_fault_enable_default)(struct amdgpu_device *adev, bool value);
	void (*init)(struct amdgpu_device *adev);
	int (*get_xgmi_info)(struct amdgpu_device *adev);
	void (*utcl2_harvest)(struct amdgpu_device *adev);
	void (*mode2_save_regs)(struct amdgpu_device *adev);
	void (*mode2_restore_regs)(struct amdgpu_device *adev);
	void (*halt)(struct amdgpu_device *adev);
};

struct amdgpu_gfxhub {
	const struct amdgpu_gfxhub_funcs *funcs;
};

struct amdgpu_mmhub_funcs {
	u64 (*get_fb_location)(struct amdgpu_device *adev);
	u64 (*get_mc_fb_offset)(struct amdgpu_device *adev);
	void (*init)(struct amdgpu_device *adev);
	int (*gart_enable)(struct amdgpu_device *adev);
	void (*set_fault_enable_default)(struct amdgpu_device *adev,
			bool value);
	void (*gart_disable)(struct amdgpu_device *adev);
	int (*set_clockgating)(struct amdgpu_device *adev,
			       enum amd_clockgating_state state);
	void (*get_clockgating)(struct amdgpu_device *adev, u64 *flags);
	void (*setup_vm_pt_regs)(struct amdgpu_device *adev, uint32_t vmid,
				uint64_t page_table_base);
	void (*update_power_gating)(struct amdgpu_device *adev,
				bool enable);
};

struct amdgpu_mmhub {
	const struct amdgpu_mmhub_funcs *funcs;
};

/* ---------------------------------------------------------------------------------------------
 * [shim] struct amdgpu_device and its sub-structures, cut down to the fields the imports read.
 *
 * Deviation from Linux (ADR 0002): amdgpu's buffer objects are TTM objects. The shim has no memory
 * manager, so struct amdgpu_bo carries only what amdgpu_gmc_pd_addr needs: the MC address of the
 * buffer and the device it belongs to. Whoever allocates the GART table fills it in.
 * ------------------------------------------------------------------------------------------- */
struct amdgpu_bo {
	struct amdgpu_device *adev;
	u64 gpu_addr;                 /* MC address of the buffer, i.e. amdgpu_bo_gpu_offset() */
};

/* [amdgpu] drivers/gpu/drm/amd/amdgpu/amdgpu_doorbell.h, imported unmodified: `struct
 * amdgpu_doorbell_index` and the AMDGPU_NAVI10_DOORBELL_* assignment this chip uses. It is the only
 * import this header includes, and it is included here rather than in the .c files because
 * struct amdgpu_device carries a doorbell_index by value, exactly as upstream does.
 *
 * The header has a nameless union (MSVC C4201). It is switched off around this include only, not
 * from the build line, so that /W4 stays strict for every other line in the translation unit. The
 * import itself is untouched, which is the rule that matters. */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4201)
#endif
#include "amdgpu_doorbell.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/* [amdgpu] amdgpu.h:997 and amdgpu_device.h struct amdgpu_mmio_remap: where in BAR5 the HDP flush
 * registers are aliased, so that a flush is one store to a page that can be handed to user space.
 * amdgpu programs this in nv_common_hw_init() long before the GFX block comes up, and
 * amdgpu_device_flush_hdp() writes through it; see driver/shim/bc250_nbio.c. */
struct amdgpu_mmio_remap {
	u32 reg_offset;         /* byte offset inside BAR5 */
	u64 bus_addr;           /* its physical address, for user-space mapping */
};

/* [uapi] include/uapi/linux/kfd_ioctl.h:738-739, enum kfd_mmio_remap. Two offsets inside the
 * remapped page; the names are KFD's because KFD is what the page is exposed for. */
#define KFD_MMIO_REMAP_HDP_MEM_FLUSH_CNTL	0
#define KFD_MMIO_REMAP_HDP_REG_FLUSH_CNTL	4

/* [amdgpu] amdgpu_ih.h:39 struct amdgpu_ih_ring, reduced to ring 0 of the hardware IH.
 *
 * Upstream carries three rings plus a software one; navi10_ih_sw_init() zeroes ring_size for ih1
 * and ih2 unconditionally (navi10_ih.c:580-581), so only this one has ever existed on this family.
 * The fields upstream keeps for the BO, the DMA mapping and the IP block are gone: the shim owns no
 * memory, so the two bc250_mem allocations are what stands in for them.
 *
 * ptr_mask, rptr and the write pointer are all BYTE offsets, as upstream's are. */
/* [amdgpu] amdgpu_ih.h:29 struct amdgpu_ih_regs, minus the two ring-1/ring-2 members this part
 * never has and minus psp_reg_id, which only the SR-IOV path uses.
 *
 * These are dword indices, resolved ONCE by bc250_ih_setup() through navi10_ih_init_register_offset
 * (navi10_ih.c:49). Upstream carries them on the ring for its own reasons; here it is also what lets
 * the DPC trio run without reaching into adev->reg_offset, which the miniport's DPC-only
 * struct amdgpu_device does not have. See the contract at the top of bc250_ih.h. */
struct amdgpu_ih_regs {
	u32	ih_rb_base;
	u32	ih_rb_base_hi;
	u32	ih_rb_cntl;
	u32	ih_rb_wptr;
	u32	ih_rb_rptr;
	u32	ih_doorbell_rptr;
	u32	ih_rb_wptr_addr_lo;
	u32	ih_rb_wptr_addr_hi;
};

struct amdgpu_ih_ring {
	struct amdgpu_ih_regs	ih_regs;

	u32			*ring;          /* CPU side of the GTT ring */
	u64			gpu_addr;       /* what the IH block walks the GART to reach */
	u32			ring_size;
	u32			ptr_mask;       /* ring_size - 1, in bytes */
	u32			rptr;

	u64			wptr_addr;      /* MC address of the write-pointer slot */
	volatile u32		*wptr_cpu;
	u64			rptr_addr;
	volatile u32		*rptr_cpu;

	bool			use_doorbell;
	u32			doorbell_index; /* dword index, i.e. amdgpu's qword index << 1 */
	bool			enabled;

	struct bc250_mem	ring_mem;
	struct bc250_mem	wb_mem;
};

/* [amdgpu] amdgpu_irq.h:64 struct amdgpu_irq, reduced to what this driver has: one ring and the
 * one flag that reaches a register (RPTR_REARM). The source table, the handlers and the Linux irq
 * domain are the miniport's ISR/DPC, not the shim's. */
struct amdgpu_irq {
	struct amdgpu_ih_ring	ih;
	bool			msi_enabled;
};

/* [amdgpu] amdgpu.h */
#define AMDGPU_GPU_PAGE_SIZE	4096

/* [amdgpu] amdgpu.h: (PAGE_SIZE / AMDGPU_GPU_PAGE_SIZE), how many GPU pages one CPU page covers.
 * Upstream this can exceed 1 on architectures with large pages; on x86-64, and on every Windows
 * target this driver has, the CPU page is also 4 KB, so it is 1. The loops in bc250_gart.c keep the
 * upstream shape anyway, because that is what makes them comparable with amdgpu_gart_map(). */
#define AMDGPU_GPU_PAGES_IN_CPU_PAGE	1

/* [amdgpu] amdgpu_ring.h:49 enum amdgpu_ring_priority_level. Unlike the ring types below, these
 * values DO reach the hardware: they land in the MQD's cp_hqd_pipe_priority, which unit A's own
 * MQD dump shows as 2 for the one high-priority compute queue. */
enum amdgpu_ring_priority_level {
	AMDGPU_RING_PRIO_0,
	AMDGPU_RING_PRIO_1,
	AMDGPU_RING_PRIO_DEFAULT = 1,
	AMDGPU_RING_PRIO_2,
	AMDGPU_RING_PRIO_MAX
};

/* [amdgpu] amdgpu_gfx.h:52-58. Note that a queue which is not high priority does NOT get
 * AMDGPU_GFX_PIPE_PRIO_NORMAL: amdgpu_ring_to_mqd_prop() memsets the whole prop to zero and only
 * the high-priority branch assigns, so an ordinary queue keeps 0. Unit A's MQDs show 0, not 1. */
enum amdgpu_gfx_pipe_priority {
	AMDGPU_GFX_PIPE_PRIO_NORMAL = AMDGPU_RING_PRIO_1,
	AMDGPU_GFX_PIPE_PRIO_HIGH = AMDGPU_RING_PRIO_2
};

#define AMDGPU_GFX_QUEUE_PRIORITY_MAXIMUM	15

/* [amdgpu] amdgpu_gfx.h:121 enum amdgpu_unmap_queues_action. Only the one the teardown uses is
 * named: the other three belong to the scheduler's preempt paths, which do not exist here. The
 * value agrees with the comment in the imported nvd.h at PACKET3_UNMAP_QUEUES_ACTION, "1 -
 * RESET_QUEUES". */
#define BC250_RESET_QUEUES	1

/* [amdgpu] amdgpu_ring.h: the ring types the shim uses. The numeric values do not reach the
 * hardware; gfx10_kiq_map_queues() switches on them to pick eng_sel. */
enum amdgpu_ring_type {
	AMDGPU_RING_TYPE_GFX,
	AMDGPU_RING_TYPE_COMPUTE,
	AMDGPU_RING_TYPE_SDMA,
	AMDGPU_RING_TYPE_KIQ
};

/* [amdgpu] amdgpu_ring.h:62-65. The flags gfx_v10_0_ring_emit_fence() and its KIQ twin branch on,
 * taken over with upstream's names so that bc250_gfx_emit_fence() reads as the transcription it is.
 * TC_WB_ONLY and EXEC are upstream's and are not here: neither gfx10 fence emitter looks at them
 * (gfx_v10_0.c:8712 and :8780 read only these two), so carrying them would suggest a behaviour this
 * driver does not have. */
#define AMDGPU_FENCE_FLAG_64BIT		(1 << 0)
#define AMDGPU_FENCE_FLAG_INT		(1 << 1)

/* [amdgpu] amdgpu_ring.h, cut down. Upstream carries scheduler, fence and IB callbacks; the shim
 * submits nothing but bring-up packets, so only the three members the write path reads are here. */
struct amdgpu_ring_funcs {
	enum amdgpu_ring_type	type;
	u32			align_mask;
	u32			nop;
};

/* [amdgpu] amdgpu_ring.h, cut down to the fields amdgpu_ring_alloc/write/write_multiple/commit and
 * the GFX10 MQD builders actually read, plus the allocations the shim owns on the ring's behalf.
 *
 * Deviation from Linux (ADR 0002): there is no scheduler, no fence context, no IB pool and no BO.
 * `ring_mem`, `mqd_mem` and `eop_mem` hold what bc250_shim_mem_alloc() handed out, so that
 * bc250_gfx_hw_fini() can give it all back without a separate bookkeeping structure. */
struct amdgpu_ring {
	struct amdgpu_device		*adev;
	const struct amdgpu_ring_funcs	*funcs;

	u32		*ring;          /* CPU mapping of the ring buffer */
	u64		gpu_addr;       /* MC address of the ring buffer */
	u32		ring_size;      /* bytes */
	u64		wptr;           /* in dwords for CP rings, see bc250_ring.c */
	u64		wptr_old;
	u32		max_dw;
	int		count_dw;
	u32		buf_mask;       /* ring_size / 4 - 1 */
	u64		ptr_mask;

	bool		use_doorbell;
	u32		doorbell_index;

	u64		rptr_gpu_addr;  /* writeback slot the CP reports the read pointer in */
	volatile u32	*rptr_cpu_addr; /* GFX10 reports a 32-bit dword read pointer */
	bool		track_rptr;     /* opt-in GFX job capacity check, not bring-up/SDMA */
	u64		wptr_gpu_addr;  /* writeback slot the CP polls for the write pointer */
	void		*wptr_cpu_addr;

	u64		eop_gpu_addr;
	u64		mqd_gpu_addr;
	void		*mqd_ptr;

	u32		me;
	u32		pipe;
	u32		queue;

	struct bc250_mem ring_mem;
	struct bc250_mem mqd_mem;
	struct bc250_mem eop_mem;
};

/* [amdgpu] amdgpu_gfx.h */
struct amdgpu_kiq {
	struct amdgpu_ring ring;
};

/* [amdgpu] amdgpu.h: the input to the MQD builders. Taken over in full so that
 * gfx_v10_0_compute_mqd_init() and gfx_v10_0_gfx_mqd_init() can be transcribed field for field. */
struct amdgpu_mqd_prop {
	uint64_t mqd_gpu_addr;
	uint64_t hqd_base_gpu_addr;
	uint64_t rptr_gpu_addr;
	uint64_t wptr_gpu_addr;
	uint32_t queue_size;
	bool use_doorbell;
	uint32_t doorbell_index;
	uint64_t eop_gpu_addr;
	uint32_t hqd_pipe_priority;
	uint32_t hqd_queue_priority;
	bool allow_tunneling;
	bool hqd_active;
	uint64_t shadow_addr;
	uint64_t gds_bkup_addr;
	uint64_t csa_addr;
	uint64_t fence_address;
	bool tmz_queue;
	bool kernel_queue;
};

/* [amdgpu] amdgpu_gfx.h, the fields of struct amdgpu_gfx_config this bring-up reads or writes. */
struct amdgpu_gfx_config {
	unsigned max_shader_engines;
	unsigned max_sh_per_se;
	unsigned max_backends_per_se;
	unsigned max_hw_contexts;
	unsigned backend_enable_mask;
	unsigned gb_addr_config;
	unsigned num_rbs;
	uint32_t num_sc_per_sh;
	uint32_t num_packer_per_sc;
	uint32_t pa_sc_tile_steering_override;
	uint64_t tcc_disabled_mask;
};

struct amdgpu_gart {
	struct amdgpu_bo *bo;
	u64 table_size;
};

struct amdgpu_gmc {
	u64 fb_start;
	u64 fb_end;
	u64 vram_start;
	u64 vram_end;
	u64 agp_start;
	u64 agp_end;
	u64 agp_size;
	u64 gart_start;
	u64 gart_end;
	u64 gart_size;
	u64 mc_vram_size;
	u64 real_vram_size;
	u64 mc_mask;
	bool translate_further;
	bool noretry;

	/* [amdgpu] gmc_v10_0_sw_init(): the LDS and scratch apertures the SH_MEM_BASES writes of
	 * gfx_v10_0_constants_init() are built from. */
	u64 shared_aperture_start;
	u64 shared_aperture_end;
	u64 private_aperture_start;
	u64 private_aperture_end;
};

/* [amdgpu] amdgpu_ids.h: how many VMIDs a hub hands out. gfx_v10_0_constants_init() walks
 * adev->vm_manager.id_mgr[AMDGPU_GFXHUB(0)].num_ids. */
struct amdgpu_vmid_mgr {
	unsigned int num_ids;
};

struct amdgpu_vm_manager {
	u64 max_pfn;
	u64 vram_base_offset;
	uint32_t num_level;
	uint32_t block_size;
	uint32_t fragment_size;

	unsigned int first_kfd_vmid;
	struct amdgpu_vmid_mgr id_mgr[AMDGPU_MAX_VMHUBS];
};

/* [shim] the two fields soc15_common.h's RLC macros test, plus the clear-state buffer that
 * gfx_v10_0_init_csb() points the RLC at. `cs_data` is the imported gfx10_cs_data table. */
struct amdgpu_rlc {
	const void *funcs;
	bool rlcg_reg_access_supported;

	const struct cs_section_def *cs_data;
	u32 *cs_ptr;                    /* CPU mapping of the clear-state buffer */
	u64 clear_state_gpu_addr;
	u32 clear_state_size;           /* in dwords, what RLC_CSIB_LENGTH gets */
	struct bc250_mem clear_state_mem;
};

/* [amdgpu] amdgpu_gfx.h: struct amdgpu_me and struct amdgpu_mec, cut down to the four counts this
 * sequence reads. Upstream carries the firmware objects, the ring arrays and the queue bitmaps on
 * them as well. The names are upstream's, so that `adev->gfx.me.num_pipe_per_me` in a transcribed
 * function is spelled the way it is in gfx_v10_0.c.
 *
 * What unit A's values are, and how the trace says so:
 *   me.num_me = 1, me.num_pipe_per_me = 1     gfx_v10_0_set_priv_inst_fault_state() walks both and
 *                                             writes only CP_INT_CNTL_RING0; pipe 1 would be
 *                                             CP_INT_CNTL_RING1, which the trace never touches.
 *   mec.num_mec = 1, mec.num_pipe_per_mec = 4 the same loop in set_priv_reg_fault_state() writes
 *                                             CP_ME1_PIPE0..3_INT_CNTL and nothing on ME2. */
struct amdgpu_me {
	u32 num_me;
	u32 num_pipe_per_me;
};

struct amdgpu_mec {
	u32 num_mec;
	u32 num_pipe_per_mec;
};

struct amdgpu_gfx {
	uint32_t xcc_mask;
	struct amdgpu_rlc rlc;
	struct amdgpu_me  me;
	struct amdgpu_mec mec;

	struct amdgpu_gfx_config config;

	/* [shim] Upstream has adev->gfx.kiq[AMDGPU_MAX_XCC] and heap-allocated ring arrays. This part
	 * has one XCC, one gfx ring and eight compute queues (me 1, pipes 0-3, queues 0-1, which is
	 * what the E03 trace shows), so they are plain members and the upstream spelling
	 * adev->gfx.kiq[0].ring still works. */
	struct amdgpu_kiq	kiq[1];
	struct amdgpu_ring	gfx_ring[1];
	struct amdgpu_ring	compute_ring[8];
	u32			num_gfx_rings;
	u32			num_compute_rings;

	/* [shim] two Linux module parameters the sequence branches on, carried on adev so that the
	 * stage functions keep upstream's signatures. Set by bc250_gfx_setup() from
	 * struct bc250_gfx_inputs; see that struct for what the trace says about each. */
	bool			async_gfx_ring;
	bool			pp_gfxoff;

	/* [shim] the writeback page the rings' read- and write-pointer slots are cut from. */
	struct bc250_mem	wb_mem;

	/* [shim] a second GTT page, for the fence slots bc250_gfx_emit_fence() writes into. It stands
	 * in for upstream's adev->wb pool (amdgpu_device_wb_get()), which the fence driver takes a slot
	 * from per ring. It is deliberately NOT allocated by bc250_gfx_setup(): nothing in the traced
	 * bring-up window needs it, and allocating it there would move every MC address the bring-up
	 * programs. bc250_gfx_fence_page_alloc() is a separate call for that reason. */
	struct bc250_mem	fence_mem;

	/* [shim] the compute dispatch's two buffers: the shader program and the memory it writes.
	 * Allocated by bc250_gfx_dispatch_setup() and by nothing else, for the same reason as
	 * fence_mem above - the traced bring-up must not see them. See bc250_dispatch.h. */
	struct bc250_mem	dispatch_shader;
	struct bc250_mem	dispatch_dst;

	/* [shim] one GTT page the driver builds an indirect buffer in, for the submission that proves
	 * PACKET3_INDIRECT_BUFFER before any page table of a process is involved (ADR 0008 stage C).
	 * Allocated by bc250_gfx_ib_page_alloc() alone, for the third time for the same reason: an
	 * allocation the traced bring-up does not make cannot move an address it programs. */
	struct bc250_mem	ib_mem;

	/* upstream adev->gfx.mec_bitmap[0].queue_bitmap, which is a bitmap over
	 * AMDGPU_MAX_COMPUTE_QUEUES; eight queues fit in a u64 and gfx10_kiq_set_resources()
	 * already folds it into a 64-bit queue_mask. */
	u64			mec_queue_bitmap;

	/* [shim] BC-250 CU mode (bc250_cu_mode.h). Installed by the miniport's startup for the whole
	 * device start (a retained-power resume re-runs the constants stage); bc250_get_cu_tcc_info()
	 * calls it where the unlock reference writes, with the select transcription as the third
	 * argument. Host replays leave it NULL, so the traced register stream is unchanged. */
	void			(*cu_mode_hook)(struct amdgpu_device *adev, void *ctx,
						void (*select)(struct amdgpu_device *adev, u32 se, u32 sh,
							       u32 instance));
	void			*cu_mode_ctx;
	/* [shim] The same device start's check after the RLC stage (bc250_cu_mode_after_rlc()), called
	 * by bc250_gfx_rlc_resume() once amdgpu's sequence has turned power gating off. NULL in replays. */
	void			(*cu_mode_rlc_hook)(struct amdgpu_device *adev, void *ctx,
						    void (*select)(struct amdgpu_device *adev, u32 se, u32 sh,
								   u32 instance));
};

/* [amdgpu] amdgpu_sdma.h: struct amdgpu_sdma_instance and struct amdgpu_sdma, cut down to the ring
 * and the instance count. Upstream also carries the firmware image, its header, the fence and the
 * IP-dump buffer per instance; the firmware is the PSP's business on this part and the rest has no
 * counterpart here.
 *
 * Two instances: the E03 trace programs SDMA0 and SDMA1 and nothing beyond (the register block
 * repeats at +0x600 dwords, and SDMA2 would be another +0x600, never written). */
#define AMDGPU_MAX_SDMA_INSTANCES	2

struct amdgpu_sdma_instance {
	struct amdgpu_ring ring;
};

struct amdgpu_sdma {
	struct amdgpu_sdma_instance instance[AMDGPU_MAX_SDMA_INSTANCES];
	int num_instances;

	/* [shim] the writeback page the two rings' read- and write-pointer slots are cut from, the
	 * same arrangement as adev->gfx.wb_mem. */
	struct bc250_mem wb_mem;

	/* [shim] the scratch and fence page, and NOT allocated by bc250_sdma_setup() - see
	 * bc250_sdma_fence_page_alloc(). It stands to the SDMA rings as adev->gfx.fence_mem does to
	 * the CP ones, and for the same reason: an allocation the bring-up does not make cannot
	 * move an address the bring-up programs into a register. */
	struct bc250_mem fence_mem;
};

/* [amdgpu] amdgpu.h: struct amdgpu_mem_scratch, the scratch page whose MC address becomes
 * GCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR. */
struct amdgpu_mem_scratch {
	u64 gpu_addr;
};

/* [shim] adev->firmware, cut down to the one member psp_v11_0_8_ring_destroy() names: the buffer
 * object of the PSP ring. The ring's memory belongs to whoever set the PSP context up. */
struct amdgpu_firmware {
	struct amdgpu_bo *rbuf;
};

struct amdgpu_device {
	void			*dev;           /* opaque owner handle, only passed to the log macros */

	uint32_t		*reg_offset[MAX_HWIP][HWIP_MAX_INSTANCE];
	uint32_t		ip_versions[MAX_HWIP][HWIP_MAX_INSTANCE];

	struct amdgpu_gfx	gfx;
	struct amdgpu_sdma	sdma;
	struct amdgpu_gmc	gmc;
	struct amdgpu_gart	gart;
	struct amdgpu_gfxhub	gfxhub;
	struct amdgpu_mmhub	mmhub;
	struct amdgpu_vmhub	vmhub[AMDGPU_MAX_VMHUBS];
	struct amdgpu_vm_manager vm_manager;
	struct amdgpu_mem_scratch mem_scratch;
	struct amdgpu_firmware	firmware;

	/* [amdgpu] amdgpu_doorbell.h, both imported unmodified. `doorbell.base` is the physical
	 * address of the doorbell BAR, which only the miniport can know; the self-ring aperture in
	 * bc250_nbio.c is the one thing here that reads it. */
	/* [amdgpu] amdgpu.h: the physical address of BAR5. The shim never dereferences it - register
	 * access goes through bc250_shim_rreg/wreg - and it is here only so that
	 * bc250_nbio_set_reg_remap() can form rmmio_remap.bus_addr, the address of the page the HDP
	 * flush registers are aliased into. The miniport fills it from its translated resources. */
	u64			     rmmio_base;
	struct amdgpu_mmio_remap     rmmio_remap;
	struct amdgpu_doorbell	     doorbell;
	struct amdgpu_doorbell_index doorbell_index;
	struct amdgpu_irq	irq;               /* M6: the interrupt ring, see bc250_ih.h */

	u64			dummy_page_addr;   /* DMA address of the dummy page (dma_addr_t upstream) */
	u64			cg_flags;
	uint32_t		usec_timeout;      /* budget of every poll loop, amdgpu's default is 100000 */

	void			*backend;          /* the backend's own state, see bc250_shim.h */
};

/* [amdgpu] amdgpu.h: the register accessors, redirected to the shim backend. `reg` is a DWORD
 * index, exactly as SOC15_REG_OFFSET produces it; the backend multiplies by 4 for a byte offset.
 * Both macros use the `adev` in scope, the same way amdgpu's do. */
#define RREG32(reg)		bc250_shim_rreg(adev, (u32)(reg))
#define WREG32(reg, v)		bc250_shim_wreg(adev, (u32)(reg), (u32)(v))
#define RREG32_NO_KIQ(reg)	RREG32(reg)
#define WREG32_NO_KIQ(reg, v)	WREG32((reg), (v))

/* [shim] SR-IOV is not a case this driver has: the BC-250 GPU is a bare-metal function. The three
 * accessors exist because soc15_common.h names them; amdgpu_sriov_vf is a real function and not a
 * constant so that `if (amdgpu_sriov_vf(adev))` in the imports does not become a constant
 * conditional (MSVC C4127) and so that no imported file has to be touched. */
bool amdgpu_sriov_vf(struct amdgpu_device *adev);
bool amdgpu_sriov_runtime(struct amdgpu_device *adev);
bool amdgpu_sriov_fullaccess(struct amdgpu_device *adev);
void amdgpu_sriov_wreg(struct amdgpu_device *adev, u32 offset, u32 value,
		       u32 acc_flags, u32 hwip, u32 xcc_id);
u32 amdgpu_sriov_rreg(struct amdgpu_device *adev, u32 offset, u32 acc_flags,
		      u32 hwip, u32 xcc_id);

/* [amdgpu] amdgpu.h ("static inline" there; __inline is how MSVC spells it in C mode) */
static __inline uint32_t amdgpu_ip_version(const struct amdgpu_device *adev,
				  uint8_t ip, uint8_t inst)
{
	/* This considers only major/minor/rev and ignores
	 * subrevision/variant fields.
	 */
	return adev->ip_versions[ip][inst] & ~0xFFU;
}

/* [shim] amdgpu_object.c upstream: unpin, unmap and free a kernel buffer object. The shim owns no
 * memory (see struct amdgpu_bo), so this only clears the three references, which is the part of the
 * upstream contract a caller can observe. Implemented in driver/shim/bc250_psp.c. */
void amdgpu_bo_free_kernel(struct amdgpu_bo **bo, u64 *gpu_addr, void **cpu_addr);

/* [amdgpu] amdgpu_gmc.c: MC address of a VRAM buffer -> physical address. */
u64 amdgpu_gmc_vram_mc2pa(struct amdgpu_device *adev, u64 mc_addr);

/* [amdgpu] amdgpu_gmc.c: MC address of the page directory root, with its flags.
 * The shim implements the GMC v10 variant of it, see driver/shim/shim.c. */
u64 amdgpu_gmc_pd_addr(struct amdgpu_bo *bo);

/* ---------------------------------------------------------------------------------------------
 * [amdgpu] amdgpu_ring.h / amdgpu_ring.c - the ring write path.
 *
 * amdgpu_ring_write() and amdgpu_ring_write_multiple() are static inlines upstream and are
 * transcribed here so that the packet emitters read exactly as they do in gfx_v10_0.c. The rest is
 * in driver/shim/bc250_ring.c. Deviation from Linux (ADR 0002): upstream amdgpu_ring_alloc() takes
 * ring->funcs->begin_use and amdgpu_ring_commit() takes end_use; the shim has neither power
 * management nor a scheduler, so those hooks do not exist.
 * ------------------------------------------------------------------------------------------- */
int  amdgpu_ring_alloc(struct amdgpu_ring *ring, unsigned int ndw);
int  bc250_ring_has_space(const struct amdgpu_ring *ring, unsigned int ndw);
void amdgpu_ring_commit(struct amdgpu_ring *ring);
void amdgpu_ring_undo(struct amdgpu_ring *ring);
void amdgpu_ring_insert_nop(struct amdgpu_ring *ring, uint32_t count);
void amdgpu_ring_clear_ring(struct amdgpu_ring *ring);

/* [amdgpu] amdgpu_ring.h:487 amdgpu_ring_write() */
static __inline void amdgpu_ring_write(struct amdgpu_ring *ring, uint32_t v)
{
	ring->ring[ring->wptr++ & ring->buf_mask] = v;
	ring->wptr &= ring->ptr_mask;
	ring->count_dw--;
}

/* [amdgpu] amdgpu_ring.h:494 amdgpu_ring_write_multiple() */
static __inline void amdgpu_ring_write_multiple(struct amdgpu_ring *ring,
						const u32 *src, int count_dw)
{
	unsigned int occupied, chunk1, chunk2;

	occupied = (unsigned int)(ring->wptr & ring->buf_mask);
	chunk1 = ring->buf_mask + 1u - occupied;
	chunk1 = (chunk1 >= (unsigned int)count_dw) ? (unsigned int)count_dw : chunk1;
	chunk2 = (unsigned int)count_dw - chunk1;

	if (chunk1)
		memcpy(&ring->ring[occupied], src, (size_t)chunk1 * 4u);

	if (chunk2)
		memcpy(ring->ring, src + chunk1, (size_t)chunk2 * 4u);

	ring->wptr += (u64)count_dw;
	ring->wptr &= ring->ptr_mask;
	ring->count_dw -= count_dw;
}

#endif /* BC250_SHIM_AMDGPU_H */
