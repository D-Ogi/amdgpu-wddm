/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * driver/shim/bc250_psp.c - firmware loading through the PSP, see include/bc250_psp.h.
 *
 * Register traffic: psp_v11_0_8.c, imported unmodified (driver/amdgpu-import). Everything else in
 * this file follows drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c and amdgpu_ucode.c of kernel tag v6.18,
 * commit 7d0a66e4bb9081d75c82ec4957c50034cb0ea449 (MIT, Copyright Advanced Micro Devices, Inc.);
 * functions marked [amdgpu] are copied from there, the others name the function they follow and
 * say where they deviate (ADR 0002).
 *
 * The host test driver/shim/test/replay_psp.c runs this file against a model of the PSP and
 * compares the register traffic with amdgpu's own on unit A (E03 trace).
 */
#include <string.h>

#include "bc250_psp.h"
#include "psp_v11_0_8.h"

#define BC250_PAGE_SIZE 0x1000u

/* ---- names ----------------------------------------------------------------------------------- */

static const char *const g_file_names[BC250_FILE_COUNT] = {
	"cyan_skillfish2_sdma.bin", "cyan_skillfish2_sdma1.bin", "cyan_skillfish2_ce.bin",
	"cyan_skillfish2_pfp.bin", "cyan_skillfish2_me.bin", "cyan_skillfish2_mec.bin",
	"cyan_skillfish2_mec2.bin", "cyan_skillfish2_rlc.bin",
};

static const struct {
	const char *name;
	enum bc250_fw_file file;
	enum psp_gfx_fw_type type;      /* psp_get_fw_type() */
} g_fw[BC250_FW_COUNT] = {
	{ "SDMA0",   BC250_FILE_SDMA,  GFX_FW_TYPE_SDMA0 },
	{ "SDMA1",   BC250_FILE_SDMA1, GFX_FW_TYPE_SDMA1 },
	{ "CP_CE",   BC250_FILE_CE,    GFX_FW_TYPE_CP_CE },
	{ "CP_PFP",  BC250_FILE_PFP,   GFX_FW_TYPE_CP_PFP },
	{ "CP_ME",   BC250_FILE_ME,    GFX_FW_TYPE_CP_ME },
	{ "CP_MEC1", BC250_FILE_MEC,   GFX_FW_TYPE_CP_MEC },
	{ "CP_MEC1_JT", BC250_FILE_MEC, GFX_FW_TYPE_CP_MEC_ME1 },
	{ "CP_MEC2", BC250_FILE_MEC2,  GFX_FW_TYPE_CP_MEC },
	{ "CP_MEC2_JT", BC250_FILE_MEC2, GFX_FW_TYPE_CP_MEC_ME2 },
	{ "RLC_G",   BC250_FILE_RLC,   GFX_FW_TYPE_RLC_G },
};

const char *bc250_fw_file_name(enum bc250_fw_file file)
{
	return (unsigned int)file < BC250_FILE_COUNT ? g_file_names[file] : "?";
}

enum bc250_fw_file bc250_fw_file_of(enum bc250_fw_id id)
{
	return (unsigned int)id < BC250_FW_COUNT ? g_fw[id].file : BC250_FILE_COUNT;
}

const char *bc250_fw_name(enum bc250_fw_id id)
{
	return (unsigned int)id < BC250_FW_COUNT ? g_fw[id].name : "?";
}

/*
 * Follows amdgpu_ucode_init_single_fw(), branch `load_type == AMDGPU_FW_LOAD_PSP`:
 *   CP_MEC1/2      ucode_size_bytes - jt_size * 4, from ucode_array_offset_bytes
 *   CP_MEC1/2_JT   jt_size * 4, from ucode_array_offset_bytes + jt_offset * 4
 *   everything else on this chip (SDMA0/1, CE, PFP, ME, RLC_G with an rlc v2.0 header)
 *                  ucode_size_bytes, from ucode_array_offset_bytes
 * Upstream trusts request_firmware(); a driver that takes the file from a caller checks every
 * number against the file size first.
 */
int bc250_fw_locate(enum bc250_fw_id id, const u8 *file, u32 file_size,
		    u32 *offset, u32 *size, enum psp_gfx_fw_type *type)
{
	const struct common_firmware_header *header = (const struct common_firmware_header *)file;
	const struct gfx_firmware_header_v1_0 *cp_hdr = (const struct gfx_firmware_header_v1_0 *)file;
	u32 array_offset, ucode_size, jt_offset, jt_size;

	if ((unsigned int)id >= BC250_FW_COUNT || file == NULL || offset == NULL || size == NULL || type == NULL)
		return -EINVAL;
	if (file_size < sizeof(struct gfx_firmware_header_v1_0))
		return -EINVAL;
	array_offset = le32_to_cpu(header->ucode_array_offset_bytes);
	ucode_size = le32_to_cpu(header->ucode_size_bytes);
	if (le32_to_cpu(header->size_bytes) != file_size || le32_to_cpu(header->header_size_bytes) > array_offset ||
	    array_offset > file_size || ucode_size == 0 || ucode_size > file_size - array_offset)
		return -EINVAL;

	*type = g_fw[id].type;
	switch (id) {
	case BC250_FW_CP_MEC1:
	case BC250_FW_CP_MEC2:
	case BC250_FW_CP_MEC1_JT:
	case BC250_FW_CP_MEC2_JT:
		if (le16_to_cpu(header->header_version_major) != 1)
			return -EINVAL;         /* jt_offset/jt_size are gfx v1.0 fields */
		jt_offset = le32_to_cpu(cp_hdr->jt_offset);
		jt_size = le32_to_cpu(cp_hdr->jt_size);
		if (jt_size == 0 || jt_size > ucode_size / 4 || jt_offset > ucode_size / 4 - jt_size)
			return -EINVAL;
		if (id == BC250_FW_CP_MEC1 || id == BC250_FW_CP_MEC2) {
			*offset = array_offset;
			*size = ucode_size - jt_size * 4;
		} else {
			*offset = array_offset + jt_offset * 4;
			*size = jt_size * 4;
		}
		break;
	default:
		*offset = array_offset;
		*size = ucode_size;
		break;
	}
	return *size != 0 ? 0 : -EINVAL;
}

/* ---- what psp_v11_0_8.c needs from amdgpu_psp.c and amdgpu_object.c ---------------------------- */

/* [amdgpu] amdgpu_psp.c, minus the `no_hw_access` early-out (a removed device; the owner of this
 * shim stops the sequence itself when the device goes away). */
int psp_wait_for(struct psp_context *psp, uint32_t reg_index, uint32_t reg_val,
		 uint32_t mask, uint32_t flags)
{
	bool check_changed = (flags & PSP_WAITREG_CHANGED) != 0;
	bool verbose = !(flags & PSP_WAITREG_NOVERBOSE);
	uint32_t val = 0;
	uint32_t i;
	struct amdgpu_device *adev = psp->adev;

	for (i = 0; i < adev->usec_timeout; i++) {
		val = RREG32(reg_index);
		if (check_changed) {
			if (val != reg_val)
				return 0;
		} else {
			if ((val & mask) == reg_val)
				return 0;
		}
		bc250_shim_udelay(1);
	}

	if (verbose)
		dev_err(adev->dev,
			"psp reg (0x%x) wait timed out, mask: %x, read: %x exp: %x",
			reg_index, mask, val, reg_val);

	return -ETIME;
}

/* See amdgpu.h: the shim owns no memory, so freeing is forgetting. */
void amdgpu_bo_free_kernel(struct amdgpu_bo **bo, u64 *gpu_addr, void **cpu_addr)
{
	if (bo != NULL)
		*bo = NULL;
	if (gpu_addr != NULL)
		*gpu_addr = 0;
	if (cpu_addr != NULL)
		*cpu_addr = NULL;
}

/* ---- the ring ---------------------------------------------------------------------------------- */

/*
 * [amdgpu] amdgpu_psp.c: psp_ring_cmd_submit(), with two deviations. amdgpu_device_flush_hdp() is
 * left out: upstream it returns at once on an APU (the frame is not written through the HDP
 * aperture there, and not here either; the E03 trace shows no HDP write between two submissions).
 * The ring pointers are volatile because the frame is read by another processor.
 */
int psp_ring_cmd_submit(struct psp_context *psp,
			uint64_t cmd_buf_mc_addr,
			uint64_t fence_mc_addr,
			int index)
{
	unsigned int psp_write_ptr_reg = 0;
	volatile struct psp_gfx_rb_frame *write_frame;
	struct psp_ring *ring = &psp->km_ring;
	volatile struct psp_gfx_rb_frame *ring_buffer_start = ring->ring_mem;
	volatile struct psp_gfx_rb_frame *ring_buffer_end = ring_buffer_start +
		ring->ring_size / sizeof(struct psp_gfx_rb_frame) - 1;
	struct amdgpu_device *adev = psp->adev;
	uint32_t ring_size_dw = ring->ring_size / 4;
	uint32_t rb_frame_size_dw = sizeof(struct psp_gfx_rb_frame) / 4;
	uint32_t i;

	/* KM (GPCOM) prepare write pointer */
	psp_write_ptr_reg = psp->funcs->ring_get_wptr(psp);

	/* Update KM RB frame pointer to new frame */
	/* write_frame ptr increments by size of rb_frame in bytes */
	/* psp_write_ptr_reg increments by size of rb_frame in DWORDs */
	if ((psp_write_ptr_reg % ring_size_dw) == 0)
		write_frame = ring_buffer_start;
	else
		write_frame = ring_buffer_start + (psp_write_ptr_reg / rb_frame_size_dw);
	/* Check invalid write_frame ptr address */
	if ((write_frame < ring_buffer_start) || (ring_buffer_end < write_frame)) {
		dev_err(adev->dev,
			"write_frame is pointing to address out of bounds, wptr register 0x%x\n",
			psp_write_ptr_reg);
		return -EINVAL;
	}

	/* Initialize KM RB frame */
	for (i = 0; i < rb_frame_size_dw; i++)
		((volatile uint32_t *)write_frame)[i] = 0;

	/* Update KM RB frame */
	write_frame->cmd_buf_addr_hi = upper_32_bits(cmd_buf_mc_addr);
	write_frame->cmd_buf_addr_lo = lower_32_bits(cmd_buf_mc_addr);
	write_frame->fence_addr_hi = upper_32_bits(fence_mc_addr);
	write_frame->fence_addr_lo = lower_32_bits(fence_mc_addr);
	write_frame->fence_value = (uint32_t)index;

	/* Update the write Pointer in DWORDs */
	psp_write_ptr_reg = (psp_write_ptr_reg + rb_frame_size_dw) % ring_size_dw;
	psp->funcs->ring_set_wptr(psp, psp_write_ptr_reg);
	return 0;
}

/*
 * Follows psp_cmd_submit_buf(). Deviations: the fence is polled in two stages instead of
 * psp_timeout polls of usleep_range(10, 100): 400 polls 50 us apart (20 ms; amdgpu's eleven
 * commands took between 0.1 and 6 ms each on unit A), then 100 polls 10 ms apart, which a backend
 * that can sleep turns into sleeping, so that a PSP that does not answer costs its caller about a
 * second of waiting and not a second of spinning with a lock held. A status other than zero is an error
 * here: upstream only warns, because "some version of PSP FW doesn't write 0 to that field"; a
 * bring-up step wants to stop at the first command the PSP did not accept, and unit A's PSP does
 * write the field (the host test cannot show that, the hardware run does).
 */
#define BC250_PSP_FAST_POLLS 400
#define BC250_PSP_SLOW_POLLS 100

static int bc250_psp_cmd_submit_buf(struct bc250_psp *ctx, struct psp_gfx_cmd_resp *cmd)
{
	struct psp_context *psp = &ctx->psp;
	volatile uint32_t *fence = (volatile uint32_t *)psp->fence_buf;
	int timeout = BC250_PSP_FAST_POLLS + BC250_PSP_SLOW_POLLS;
	uint32_t index;
	int ret;

	memset(psp->cmd_buf_mem, 0, PSP_CMD_BUFFER_SIZE);
	memcpy(psp->cmd_buf_mem, cmd, sizeof(struct psp_gfx_cmd_resp));

	index = ++psp->fence_value;
	ret = psp_ring_cmd_submit(psp, psp->cmd_buf_mc_addr, psp->fence_buf_mc_addr, (int)index);
	if (ret) {
		psp->fence_value--;
		return ret;
	}

	while (*fence != index) {
		if (--timeout == 0)
			break;
		bc250_shim_udelay(timeout > BC250_PSP_SLOW_POLLS ? 50 : 10000);
	}

	memcpy(&cmd->resp, &psp->cmd_buf_mem->resp, sizeof(struct psp_gfx_resp));

	if (timeout == 0) {
		dev_err(psp->adev->dev, "psp gfx command 0x%X: no fence after %d polls\n",
			cmd->cmd_id, BC250_PSP_FAST_POLLS + BC250_PSP_SLOW_POLLS);
		return -ETIME;
	}
	if (cmd->resp.status != 0) {
		dev_err(psp->adev->dev, "psp gfx command 0x%X failed and response status is (0x%X)\n",
			cmd->cmd_id, cmd->resp.status);
		return -EINVAL;
	}
	return 0;
}

/* acquire_psp_cmd_buf(), without the mutex: the owner serializes. */
static struct psp_gfx_cmd_resp *acquire_cmd(struct bc250_psp *ctx)
{
	memset(&ctx->cmd, 0, sizeof(ctx->cmd));
	return &ctx->cmd;
}

/* ---- the steps --------------------------------------------------------------------------------- */

/* What psp_early_init(), psp_sw_init(), psp_ring_init() and the start of psp_load_fw() leave
 * behind, for this chip. */
int bc250_psp_setup(struct amdgpu_device *adev, struct bc250_psp *ctx,
		    const struct bc250_psp_inputs *in)
{
	struct psp_context *psp;
	const u64 page_mask = BC250_PAGE_SIZE - 1;

	if (adev == NULL || ctx == NULL || in == NULL)
		return -EINVAL;
	if (in->ring_mem == NULL || in->cmd_buf == NULL || in->fence_buf == NULL)
		return -EINVAL;
	if (((in->ring_mc | in->cmd_buf_mc | in->fence_buf_mc) & page_mask) != 0)
		return -EINVAL;         /* psp_gfx_if.h: "must be 4 KB aligned" */
	if (in->tmr_mc == 0 || (in->tmr_mc % PSP_TMR_SIZE(adev)) != 0)
		return -EINVAL;         /* psp_tmr_init(): "naturally aligned" */
	if (adev->reg_offset[MP0_HWIP][0] == NULL || adev->gmc.vram_start == 0 ||
	    in->tmr_mc < adev->gmc.vram_start || in->tmr_mc + PSP_TMR_SIZE(adev) - 1 > adev->gmc.vram_end)
		return -EINVAL;         /* register bases and the VRAM window come from bc250_gmc_setup */
	if (adev->usec_timeout == 0)
		return -EINVAL;

	memset(ctx, 0, sizeof(*ctx));
	psp = &ctx->psp;
	psp->adev = adev;
	psp->cmd = &ctx->cmd;
	psp->psp_timeout = 20000;                       /* psp_early_init() */
	psp_v11_0_8_set_psp_funcs(psp);                 /* IP_VERSION(11, 0, 8) with AMD_APU_IS_CYAN_SKILLFISH2 */

	psp->km_ring.ring_type = PSP_RING_TYPE__KM;     /* psp_ring_init() */
	psp->km_ring.ring_mem = (struct psp_gfx_rb_frame *)in->ring_mem;
	psp->km_ring.ring_mem_mc_addr = in->ring_mc;
	psp->km_ring.ring_size = 0x1000;

	psp->cmd_buf_mem = (struct psp_gfx_cmd_resp *)in->cmd_buf;
	psp->cmd_buf_mc_addr = in->cmd_buf_mc;
	psp->fence_buf = in->fence_buf;
	psp->fence_buf_mc_addr = in->fence_buf_mc;
	memset(psp->fence_buf, 0, PSP_FENCE_BUFFER_SIZE);       /* psp_load_fw() */

	psp->tmr_mc_addr = in->tmr_mc;
	psp->tmr_size = PSP_TMR_SIZE(adev);
	return 0;
}

int bc250_psp_ring_create(struct bc250_psp *ctx)
{
	int ret = ctx->psp.funcs->ring_create(&ctx->psp, PSP_RING_TYPE__KM);

	if (ret == 0)
		ctx->ring_created = 1;
	return ret;
}

int bc250_psp_ring_stop(struct bc250_psp *ctx)
{
	int ret = ctx->psp.funcs->ring_stop(&ctx->psp, PSP_RING_TYPE__KM);

	if (ret == 0)
		ctx->ring_created = 0;
	return ret;
}

/* psp_prep_tmr_cmd_buf() + psp_tmr_load(). */
int bc250_psp_tmr_load(struct bc250_psp *ctx)
{
	struct psp_context *psp = &ctx->psp;
	struct psp_gfx_cmd_resp *cmd = acquire_cmd(ctx);
	uint64_t tmr_pa = amdgpu_gmc_vram_mc2pa(psp->adev, psp->tmr_mc_addr);   /* amdgpu_gmc_vram_pa() */
	int ret;

	cmd->cmd_id = GFX_CMD_ID_SETUP_TMR;
	cmd->cmd.cmd_setup_tmr.buf_phy_addr_lo = lower_32_bits(psp->tmr_mc_addr);
	cmd->cmd.cmd_setup_tmr.buf_phy_addr_hi = upper_32_bits(psp->tmr_mc_addr);
	cmd->cmd.cmd_setup_tmr.buf_size = psp->tmr_size;
	cmd->cmd.cmd_setup_tmr.bitfield.virt_phy_addr = 1;
	cmd->cmd.cmd_setup_tmr.system_phy_addr_lo = lower_32_bits(tmr_pa);
	cmd->cmd.cmd_setup_tmr.system_phy_addr_hi = upper_32_bits(tmr_pa);
	dev_info(psp->adev->dev, "reserve 0x%x from 0x%llx for PSP TMR\n", psp->tmr_size, psp->tmr_mc_addr);

	ret = bc250_psp_cmd_submit_buf(ctx, cmd);
	if (ret == 0)
		ctx->tmr_loaded = 1;
	return ret;
}

/* psp_prep_tmr_unload_cmd_buf() + psp_tmr_unload(). */
int bc250_psp_tmr_unload(struct bc250_psp *ctx)
{
	struct psp_gfx_cmd_resp *cmd = acquire_cmd(ctx);
	int ret;

	cmd->cmd_id = GFX_CMD_ID_DESTROY_TMR;
	dev_info(ctx->psp.adev->dev, "free PSP TMR buffer\n");
	ret = bc250_psp_cmd_submit_buf(ctx, cmd);
	if (ret == 0)
		ctx->tmr_loaded = 0;
	return ret;
}

/* psp_prep_load_ip_fw_cmd_buf() + psp_execute_ip_fw_load(). */
int bc250_psp_load_ip_fw(struct bc250_psp *ctx, enum psp_gfx_fw_type type,
			 u64 mc_addr, u32 size, struct psp_gfx_resp *resp)
{
	struct psp_gfx_cmd_resp *cmd = acquire_cmd(ctx);
	int ret;

	if (size == 0 || (mc_addr & (BC250_PAGE_SIZE - 1)) != 0)
		return -EINVAL;
	cmd->cmd_id = GFX_CMD_ID_LOAD_IP_FW;
	cmd->cmd.cmd_load_ip_fw.fw_phy_addr_lo = lower_32_bits(mc_addr);
	cmd->cmd.cmd_load_ip_fw.fw_phy_addr_hi = upper_32_bits(mc_addr);
	cmd->cmd.cmd_load_ip_fw.fw_size = size;
	cmd->cmd.cmd_load_ip_fw.fw_type = type;

	ret = bc250_psp_cmd_submit_buf(ctx, cmd);
	if (resp != NULL)
		*resp = cmd->resp;
	return ret;
}

u32 bc250_psp_last_status(const struct bc250_psp *ctx)
{
	return ctx->cmd.resp.status;
}
