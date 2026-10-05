/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * driver/shim/include/bc250_psp.h - firmware loading through the PSP (milestone M5, first part).
 *
 * What amdgpu does on the BC-250 (1002:13FE sets AMD_APU_IS_CYAN_SKILLFISH2, which selects
 * AMDGPU_FW_LOAD_PSP and psp_v11_0_8; autoload_supported and boot_time_tmr are false): create the
 * kernel-mode ring of the PSP's trusted OS, tell it where the trusted memory region is
 * (GFX_CMD_ID_SETUP_TMR), and hand it ten firmware images (GFX_CMD_ID_LOAD_IP_FW). Eleven ring
 * submissions, which is what the E03 trace of unit A shows on MP0_SMN_C2PMSG_67.
 *
 * The register part is AMD's psp_v11_0_8.c, imported unmodified. The command part follows
 * amdgpu_psp.c, which cannot be imported as a file (4000 lines of TTM, trusted applications, RAS,
 * sysfs); every function here names the upstream function it follows.
 */
#ifndef BC250_PSP_H
#define BC250_PSP_H

#include "amdgpu_psp.h"
#include "amdgpu_ucode.h"

/* The images in the order amdgpu submits them: psp_load_non_psp_fw() walks adev->firmware.ucode[]
 * by AMDGPU_UCODE_ID, and these are the ids that have a firmware on this chip. */
enum bc250_fw_id {
	BC250_FW_SDMA0 = 0,
	BC250_FW_SDMA1,
	BC250_FW_CP_CE,
	BC250_FW_CP_PFP,
	BC250_FW_CP_ME,
	BC250_FW_CP_MEC1,
	BC250_FW_CP_MEC1_JT,
	BC250_FW_CP_MEC2,
	BC250_FW_CP_MEC2_JT,
	BC250_FW_RLC_G,
	BC250_FW_COUNT
};

/* The eight files behind the ten images (mec and mec2 give two each). */
enum bc250_fw_file {
	BC250_FILE_SDMA = 0,    /* cyan_skillfish2_sdma.bin  */
	BC250_FILE_SDMA1,       /* cyan_skillfish2_sdma1.bin */
	BC250_FILE_CE,          /* cyan_skillfish2_ce.bin    */
	BC250_FILE_PFP,         /* cyan_skillfish2_pfp.bin   */
	BC250_FILE_ME,          /* cyan_skillfish2_me.bin    */
	BC250_FILE_MEC,         /* cyan_skillfish2_mec.bin   */
	BC250_FILE_MEC2,        /* cyan_skillfish2_mec2.bin  */
	BC250_FILE_RLC,         /* cyan_skillfish2_rlc.bin   */
	BC250_FILE_COUNT
};

/* File name without directory, e.g. "cyan_skillfish2_mec.bin". */
const char *bc250_fw_file_name(enum bc250_fw_file file);
enum bc250_fw_file bc250_fw_file_of(enum bc250_fw_id id);
const char *bc250_fw_name(enum bc250_fw_id id);

/* Which bytes of a firmware file make up one image, and the type the PSP is told. Follows
 * amdgpu_ucode_init_single_fw() (load type PSP) and psp_get_fw_type(). Checks the header against
 * the file size. Returns 0 or -EINVAL. */
int bc250_fw_locate(enum bc250_fw_id id, const u8 *file, u32 file_size,
		    u32 *offset, u32 *size, enum psp_gfx_fw_type *type);

struct bc250_psp_inputs {
	/* One page each, zeroed by the owner, reachable by the PSP under the MC address given. */
	void *ring_mem;   u64 ring_mc;          /* 4 KB, PSP_RING_TYPE__KM */
	void *cmd_buf;    u64 cmd_buf_mc;       /* PSP_CMD_BUFFER_SIZE */
	void *fence_buf;  u64 fence_buf_mc;     /* PSP_FENCE_BUFFER_SIZE */
	/* The trusted memory region: VRAM, PSP_TMR_SIZE, aligned to its size. */
	u64 tmr_mc;
};

struct bc250_psp {
	struct psp_context psp;
	struct psp_gfx_cmd_resp cmd;            /* psp->cmd points here */
	int ring_created;
	int tmr_loaded;
};

/* adev must have its register bases and adev->gmc.vram_start / vm_manager.vram_base_offset set
 * (bc250_gmc_setup does both). Touches no register. */
int bc250_psp_setup(struct amdgpu_device *adev, struct bc250_psp *ctx,
		    const struct bc250_psp_inputs *in);

/* psp_ring_create(psp, PSP_RING_TYPE__KM) and psp_ring_stop(). */
int bc250_psp_ring_create(struct bc250_psp *ctx);
int bc250_psp_ring_stop(struct bc250_psp *ctx);

/* psp_tmr_load() / psp_tmr_unload(). */
int bc250_psp_tmr_load(struct bc250_psp *ctx);
int bc250_psp_tmr_unload(struct bc250_psp *ctx);

/* psp_execute_ip_fw_load(): `mc_addr` is where the image bytes are, page aligned, reachable by the
 * PSP. On success *resp (may be NULL) is the PSP's response, with the image's address in the TMR. */
int bc250_psp_load_ip_fw(struct bc250_psp *ctx, enum psp_gfx_fw_type type,
			 u64 mc_addr, u32 size, struct psp_gfx_resp *resp);

/* Status of the last command as the PSP reported it (resp.status), for the owner's log. */
u32 bc250_psp_last_status(const struct bc250_psp *ctx);

#endif /* BC250_PSP_H */
