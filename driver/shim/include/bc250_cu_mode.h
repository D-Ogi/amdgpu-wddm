/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * BC-250 CU mode: 24 compute units (the firmware's harvest, stock) or 40 (all five WGPs of every
 * shader array). Not amdgpu. docs/design/cu-mode.md in bc250-win is the design; this header is
 * the part that has no Windows in it, so the host test (test/cu_mode_test.c) runs exactly what
 * the miniport runs.
 *
 * The mechanism is the one the bc250-40cu-unlock reference applies to amdgpu's
 * gfx_v10_0_get_cu_info(): per shader array, under GRBM_GFX_INDEX,
 *   CC_GC_SHADER_ARRAY_CONFIG.INACTIVE_WGPS   what the driver enumerates (the harvest mask)
 *   SPI_PG_ENABLE_STATIC_WGP_MASK.WGP_MASK    which WGPs the SPI dispatches waves to
 * Both, never one of them: the reference measured that either alone changes nothing, and a CU
 * count that disagrees with the dispatch mask sizes scratch for fewer waves than can run.
 *
 * Three pieces, all pure except the last:
 *   bc250_cu_decide()        the setting and the boot guard: what this start applies and what the
 *                            caller must persist before and after
 *   bc250_cu_targets() ...   the register values of one shader array, and the CU bitmap amdgpu
 *                            would derive from what the registers read back
 *   bc250_cu_mode_apply()    the register sequence, through the shim's RREG32/WREG32, called from
 *                            bc250_get_cu_tcc_info() where the reference writes, and its check
 *                            bc250_cu_mode_after_rlc() at the end of the RLC stage
 */
#ifndef BC250_CU_MODE_H
#define BC250_CU_MODE_H

#define BC250_CU_MODE_STOCK	24u
#define BC250_CU_MODE_FULL	40u
#define BC250_CU_SA_MAX		4u	/* 2 SE x 2 SA on this part; the arrays below are sized for it */
#define BC250_CU_WGP_MAX	5u	/* per SA (max_cu_per_sh 10); a disable mask has this many bits per
					 * SA, 20 in all, so bc250_cu_encode() keeps it in 28 bits */

/* Why the applied mode is not the requested one, or what the guard did. Shared with the escape
 * (BC250_ESCAPE_CU_MODE) and the application, which prints the names. */
enum bc250_cu_reason {
	BC250_CU_REASON_NONE = 0,
	BC250_CU_REASON_INVALID_SETTING = 1,	/* CuMode is neither 24 nor 40: stock, setting kept */
	BC250_CU_REASON_INVALID_DISABLE = 2,	/* CuDisableWgp names bits outside the topology */
	BC250_CU_REASON_PENDING_UNCONFIRMED = 3,/* an earlier start applied 40 and never confirmed it */
	BC250_CU_REASON_REGISTRY = 4,		/* the pending mark could not be made durable */
	BC250_CU_REASON_NOT_THIS_DEVICE = 5,	/* PCI id is not 1002:13FE */
	BC250_CU_REASON_POWER_GATING = 6,	/* RLC power gating on: an enable other than the PSP load's static
						 * one at the constants stage, or any after the RLC stage */
	BC250_CU_REASON_STOCK_UNEXPECTED = 7,	/* stock CC and SPI disagree, or SPI names absent WGPs */
	BC250_CU_REASON_READBACK = 8,		/* a written value did not read back: stock restored */
	BC250_CU_REASON_RESTORE_FAILED = 9,	/* ...and stock did not read back either */
	BC250_CU_REASON_NOT_RUN = 10,		/* GFX bring-up did not reach the constants stage */
	BC250_CU_REASON_TOPOLOGY = 11,		/* SE/SA/CU counts outside what this code was written for */
	BC250_CU_REASON_COUNT
};

/* ---- the setting and the boot guard ---------------------------------------------------------- */

/* The encoded request: mode in the low byte, disable mask above it. Pending and Confirmed hold it,
 * so a confirmation of "40" never covers "40 with WGP 4 of SA 1 masked" or the other way round. */
static __inline unsigned int bc250_cu_encode(unsigned int mode, unsigned int disable)
{
	return (mode & 0xFFu) | (disable << 8);
}

struct bc250_cu_request {
	int		mode_present;	/* CuMode exists as a REG_DWORD */
	unsigned int	mode;
	int		disable_present;
	unsigned int	disable;	/* CuDisableWgp: bit sa * BC250_CU_WGP_MAX + wgp, 40 only */
	unsigned int	pending;	/* CuModePending, 0 when absent */
	unsigned int	confirmed;	/* CuModeConfirmed, 0 when absent */
	unsigned int	sa_count;	/* shader arrays of the part */
	unsigned int	wgps_per_sa;
};

struct bc250_cu_decision {
	unsigned int	mode;		/* 24 or 40: what this start applies */
	unsigned int	disable;	/* only with 40 */
	unsigned int	encoded;	/* bc250_cu_encode(mode, disable) for 40, else 0 */
	unsigned int	reason;		/* enum bc250_cu_reason */
	int		mark_pending;	/* write Pending = encoded, durably, BEFORE any register write */
	int		clear_pending;	/* delete Pending */
	int		force_stock;	/* write CuMode = 24: the automatic fallback, durable */
	int		confirmed;	/* 40 was confirmed on an earlier start: nothing pending this time */
};

void bc250_cu_decide(const struct bc250_cu_request *req, struct bc250_cu_decision *out);

/* After a start that applied mode 40 fell back in hardware (readback, stock check, power gating):
 * the pending mark goes, CuMode goes to 24 and the reason is kept. Pure bookkeeping, for symmetry
 * with bc250_cu_decide() and so that the host test sees the same rule the driver runs. */
int bc250_cu_hardware_fallback(unsigned int requested_mode, unsigned int applied_mode);

/* The power-gating enables of an RLC_PG_CNTL value: GFX_POWER_GATING_ENABLE, DYN_PER_WGP_PG_ENABLE,
 * STATIC_PER_WGP_PG_ENABLE and GFX_PIPELINE_PG_ENABLE. 40 does not run with any of them on after the
 * RLC stage (bc250_cu_mode_after_rlc); at the constants stage only STATIC_PER_WGP_PG_ENABLE is
 * accepted, the state the PSP-started RLC leaves on a cold start (fact M35, E11: 0x8). The other bits
 * are not enables; bit 23, which our RLC start sets (SMU handshake off), is one. */
unsigned int bc250_cu_pg_enables(unsigned int rlc_pg_cntl);

/* ---- register values and the CU bitmap ------------------------------------------------------- */

/* The WGPs of one SA that amdgpu counts: ~(CC | USER).INACTIVE_WGPS within the SA's WGPs
 * (gfx_v10_0_get_wgp_active_bitmap_per_sh()). */
unsigned int bc250_cu_active_wgps_cc(unsigned int cc, unsigned int user, unsigned int wgps_per_sa);
/* The WGPs of one SA the SPI dispatches to. */
unsigned int bc250_cu_active_wgps_spi(unsigned int spi, unsigned int wgps_per_sa);

/* Target values for one SA. Mode 24 is the stock pair. Mode 40 clears INACTIVE_WGPS and sets the
 * SPI mask for every WGP of the SA except the disabled ones; a disable bit may only name a WGP the
 * firmware left inactive, so the result is never below stock. Returns a reason (NONE on success),
 * in which case *cc and *spi are the stock pair. */
unsigned int bc250_cu_targets(unsigned int mode, unsigned int disable_sa, unsigned int wgps_per_sa,
			      unsigned int stock_cc, unsigned int stock_spi,
			      unsigned int *cc, unsigned int *spi);

/* What the caps blob reports, computed the way gfx_v10_0_get_cu_info() computes it from a per-SA
 * active WGP mask (one WGP is two CUs). Index sa = se * sh_per_se + sh. */
struct bc250_cu_info {
	unsigned int	active;			/* cu_active_number */
	unsigned int	ao_mask;		/* cu_ao_mask, with amdgpu's 32-bit wrap */
	unsigned int	bitmap[4][4];		/* cu_bitmap[se][sh] */
	unsigned int	ao_bitmap[4][4];	/* cu_ao_bitmap[se][sh] */
};
void bc250_cu_info_from_wgps(const unsigned int *active_wgps, unsigned int se_count,
			     unsigned int sh_per_se, unsigned int max_cu_per_sh,
			     struct bc250_cu_info *out);

/* ---- the register sequence ------------------------------------------------------------------- */

/* One run of the constants stage. The caller fills the inputs at PASSIVE_LEVEL before the stage
 * (the registry is not reachable from inside it) and reads the outputs after it. */
struct bc250_cu_mode_hw {
	/* in */
	unsigned int	mode;			/* 24 or 40, from bc250_cu_decide() */
	unsigned int	disable;
	int		have_stock;		/* stock_* below come from this boot's record */
	unsigned int	stock_cc[BC250_CU_SA_MAX], stock_spi[BC250_CU_SA_MAX];
	/* out */
	int		ran;
	unsigned int	sa_count, wgps_per_sa, se_count, sh_per_se, max_cu_per_sh;
	unsigned int	applied;		/* the mode the registers read back as */
	unsigned int	reason;
	int		wrote;			/* at least one register write reached the part */
	unsigned int	entry_cc[BC250_CU_SA_MAX], entry_spi[BC250_CU_SA_MAX];
	unsigned int	target_cc[BC250_CU_SA_MAX], target_spi[BC250_CU_SA_MAX];
	unsigned int	cc[BC250_CU_SA_MAX], user[BC250_CU_SA_MAX], spi[BC250_CU_SA_MAX];
	unsigned int	rlc_pg_cntl, rlc_aon_wgp_mask;	/* at the constants stage */
	int		after_rlc_ran;			/* bc250_cu_mode_after_rlc() ran for this stage run */
	unsigned int	rlc_pg_cntl_after;		/* after the RLC stage: what 40 runs with */
	/* per SA: what the CC view and the SPI view count, and the union the caps report. The union
	 * is the safe side: scratch is sized for at least every CU that can receive a wave. */
	unsigned int	active_wgps[BC250_CU_SA_MAX];
	int		consistent;		/* CC view == SPI view on every SA */
};

struct amdgpu_device;
typedef void (*bc250_cu_select_fn)(struct amdgpu_device *adev, unsigned int se, unsigned int sh,
				   unsigned int instance);

/* The register sequence, for bc250_get_cu_tcc_info(): read every SA, keep this boot's stock,
 * write both registers of every SA if they differ from the targets, read them back, restore stock
 * on any mismatch, leave GRBM_GFX_INDEX broadcasting. `select` is the shim's gfx_v10_0_select_se_sh
 * transcription, passed in so that there is one copy of it. */
void bc250_cu_mode_apply(struct amdgpu_device *adev, void *ctx, bc250_cu_select_fn select);

/* For the end of bc250_gfx_rlc_resume(), after amdgpu's "disable PG" write and the RLC start: reads
 * RLC_PG_CNTL again. If 40 was applied and a PG enable is still on, every SA goes back to stock,
 * verified as in bc250_cu_mode_apply(), with reason POWER_GATING (RESTORE_FAILED if stock does not
 * read back). Nothing is written otherwise. */
void bc250_cu_mode_after_rlc(struct amdgpu_device *adev, void *ctx, bc250_cu_select_fn select);

#endif
