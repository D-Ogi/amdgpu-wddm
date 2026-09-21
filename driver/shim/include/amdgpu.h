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

/* [amdgpu] amdgpu_vm.h */
#define AMDGPU_PTE_VALID	(1ULL << 0)

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
};

struct amdgpu_vm_manager {
	u64 max_pfn;
	u64 vram_base_offset;
	uint32_t num_level;
	uint32_t block_size;
	uint32_t fragment_size;
};

/* [shim] only the two fields soc15_common.h's RLC macros test. */
struct amdgpu_rlc {
	const void *funcs;
	bool rlcg_reg_access_supported;
};

struct amdgpu_gfx {
	uint32_t xcc_mask;
	struct amdgpu_rlc rlc;
};

/* [amdgpu] amdgpu.h: struct amdgpu_mem_scratch, the scratch page whose MC address becomes
 * GCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR. */
struct amdgpu_mem_scratch {
	u64 gpu_addr;
};

struct amdgpu_device {
	void			*dev;           /* opaque owner handle, only passed to the log macros */

	uint32_t		*reg_offset[MAX_HWIP][HWIP_MAX_INSTANCE];
	uint32_t		ip_versions[MAX_HWIP][HWIP_MAX_INSTANCE];

	struct amdgpu_gfx	gfx;
	struct amdgpu_gmc	gmc;
	struct amdgpu_gart	gart;
	struct amdgpu_gfxhub	gfxhub;
	struct amdgpu_mmhub	mmhub;
	struct amdgpu_vmhub	vmhub[AMDGPU_MAX_VMHUBS];
	struct amdgpu_vm_manager vm_manager;
	struct amdgpu_mem_scratch mem_scratch;

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

/* [amdgpu] amdgpu_gmc.c: MC address of a VRAM buffer -> physical address. */
u64 amdgpu_gmc_vram_mc2pa(struct amdgpu_device *adev, u64 mc_addr);

/* [amdgpu] amdgpu_gmc.c: MC address of the page directory root, with its flags.
 * The shim implements the GMC v10 variant of it, see driver/shim/shim.c. */
u64 amdgpu_gmc_pd_addr(struct amdgpu_bo *bo);

#endif /* BC250_SHIM_AMDGPU_H */
