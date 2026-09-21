/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * driver/shim/include/amdgpu_psp.h - the slice of amdgpu_psp.h that the imported psp_v11_0_8.c and
 * our driver of the PSP sequence (driver/shim/bc250_psp.c) compile against.
 *
 *   [amdgpu] copied from drivers/gpu/drm/amd/amdgpu/amdgpu_psp.h, MIT, kernel tag v6.18, commit
 *            7d0a66e4bb9081d75c82ec4957c50034cb0ea449. Copyright the respective AMD copyright
 *            holders (see driver/amdgpu-import/PROVENANCE.md).
 *   [shim]   ours; deviations from Linux say why (ADR 0002).
 *
 * The command and ring structures themselves are AMD's interface header psp_gfx_if.h, imported
 * unmodified.
 */
#ifndef BC250_SHIM_AMDGPU_PSP_H
#define BC250_SHIM_AMDGPU_PSP_H

#include "amdgpu.h"
#include "psp_gfx_if.h"

/* [amdgpu] */
#define PSP_FENCE_BUFFER_SIZE	0x1000
#define PSP_CMD_BUFFER_SIZE	0x1000
#define PSP_1_MEG		0x100000
#define PSP_TMR_ALIGNMENT	0x100000
/* [shim] PSP_TMR_SIZE(adev) upstream: 8 MB on Aldebaran, 4 MB everywhere else. */
#define PSP_TMR_SIZE(adev)	0x400000

/* [amdgpu] Command register bit 31 set to indicate readiness */
#define MBOX_TOS_READY_FLAG (GFX_FLAG_RESPONSE)
#define MBOX_TOS_READY_MASK (GFX_CMD_RESPONSE_MASK | GFX_CMD_STATUS_MASK)

/* [amdgpu] Values to check for a successful GFX_CMD response wait. Check against
 * both status bits and response state - helps to detect a command failure
 * or other unexpected cases like a device drop reading all 0xFFs
 */
#define MBOX_TOS_RESP_FLAG (GFX_FLAG_RESPONSE)
#define MBOX_TOS_RESP_MASK (GFX_CMD_RESPONSE_MASK | GFX_CMD_STATUS_MASK)

/* [amdgpu] */
enum psp_ring_type {
	PSP_RING_TYPE__INVALID = 0,
	/*
	 * These values map to the way the PSP kernel identifies the
	 * rings.
	 */
	PSP_RING_TYPE__UM = 1, /* User mode ring (formerly called RBI) */
	PSP_RING_TYPE__KM = 2  /* Kernel mode ring (formerly called GPCOM) */
};

/* [amdgpu] */
struct psp_ring {
	enum psp_ring_type		ring_type;
	struct psp_gfx_rb_frame		*ring_mem;
	uint64_t			ring_mem_mc_addr;
	void				*ring_mem_handle;
	uint32_t			ring_size;
	uint32_t			ring_wptr;
};

/* [shim] BIT() lives in a GPL-2.0 header upstream; written from its meaning. */
#ifndef BIT
#define BIT(n) (1u << (n))
#endif

/* [amdgpu] */
#define PSP_WAITREG_CHANGED BIT(0) /* check if the value has changed */
#define PSP_WAITREG_NOVERBOSE BIT(1) /* No error verbose */

struct psp_context;

/* [amdgpu] struct psp_funcs, cut down to the members psp_v11_0_8.c fills in. The 11.0.8 PSP of
 * Cyan Skillfish has no boot loader interface for the driver (its firmware is started by the
 * platform), so ring handling is all there is. */
struct psp_funcs {
	int (*ring_create)(struct psp_context *psp,
			   enum psp_ring_type ring_type);
	int (*ring_stop)(struct psp_context *psp,
			    enum psp_ring_type ring_type);
	int (*ring_destroy)(struct psp_context *psp,
			    enum psp_ring_type ring_type);
	uint32_t (*ring_get_wptr)(struct psp_context *psp);
	void (*ring_set_wptr)(struct psp_context *psp, uint32_t value);
};

/* [shim] struct psp_context, cut down to what the ring and the command path use. Field names are
 * upstream's. Deviations: fence_value is atomic_t upstream (the owner serializes here, as it does
 * for the whole shim); the buffers are not TTM objects, their CPU pointers and MC addresses are
 * filled in by whoever owns the memory; psp_timeout lives in amdgpu_device upstream. */
struct psp_context {
	struct amdgpu_device		*adev;
	struct psp_ring			km_ring;
	struct psp_gfx_cmd_resp		*cmd;          /* where a command is built */

	const struct psp_funcs		*funcs;

	/* tmr buffer */
	uint64_t			tmr_mc_addr;
	uint32_t			tmr_size;      /* amdgpu_bo_size(psp->tmr_bo) upstream */

	/* fence buffer */
	uint64_t			fence_buf_mc_addr;
	void				*fence_buf;

	/* cmd buffer */
	uint64_t			cmd_buf_mc_addr;
	struct psp_gfx_cmd_resp		*cmd_buf_mem;

	uint32_t			fence_value;
	int				psp_timeout;   /* polls of the fence per command, amdgpu: 20000 */
};

/* [amdgpu] amdgpu_psp.c */
int psp_wait_for(struct psp_context *psp, uint32_t reg_index,
		 uint32_t field_val, uint32_t mask, uint32_t flags);
int psp_ring_cmd_submit(struct psp_context *psp,
			uint64_t cmd_buf_mc_addr,
			uint64_t fence_mc_addr,
			int index);

/* [shim] mdelay() and the two errno values psp_v11_0_8.c and the command path use. The delay is a
 * busy wait like the rest of the shim's; the one caller waits 20 ms once per ring create or stop. */
#ifndef mdelay
#define mdelay(ms) bc250_shim_udelay((unsigned int)(ms) * 1000u)
#endif
/* Linux's numbers, whatever the C library of the day says (the UCRT has ETIME = 137): the owner
 * compares against BC250_ETIME and BC250_EINVAL of bc250_gmc.h, which are Linux's too. */
#undef ETIME
#define ETIME 62
#undef EINVAL
#define EINVAL 22

#endif /* BC250_SHIM_AMDGPU_PSP_H */
