/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * BC-250 CU mode. See include/bc250_cu_mode.h for the contract and bc250-win
 * docs/design/cu-mode.md for the design. Not amdgpu: the two registers and their stock and
 * unlocked values come from the bc250-40cu-unlock reference (its patch to gfx_v10_0_get_cu_info()
 * and its technical report); the code below is ours. Register offsets, masks and shifts are names
 * from AMD's headers through the SOC15 macros, as everywhere in the shim.
 */
#include "amdgpu.h"
#include "bc250_cu_mode.h"

#include "gc/gc_10_1_0_offset.h"
#include "gc/gc_10_1_0_sh_mask.h"
#include "soc15_common.h"

#define CC_INACTIVE_MASK	CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_WGPS_MASK
#define CC_INACTIVE_SHIFT	CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_WGPS__SHIFT
#define USER_INACTIVE_MASK	GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_WGPS_MASK
#define USER_INACTIVE_SHIFT	GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_WGPS__SHIFT
#define SPI_WGP_MASK		SPI_PG_ENABLE_STATIC_WGP_MASK__WGP_MASK_MASK
#define SPI_WGP_SHIFT		SPI_PG_ENABLE_STATIC_WGP_MASK__WGP_MASK__SHIFT

static unsigned int wgp_mask(unsigned int wgps)
{
	return wgps >= 32u ? 0xFFFFFFFFu : (1u << wgps) - 1u;
}

/* Which disable bits may exist at all: BC250_CU_WGP_MAX per SA, of which the SA's own WGPs. */
static unsigned int disable_allowed(unsigned int sa_count, unsigned int wgps)
{
	unsigned int sa, allowed = 0;

	for (sa = 0; sa < sa_count && sa < BC250_CU_SA_MAX; sa++)
		allowed |= wgp_mask(wgps) << (sa * BC250_CU_WGP_MAX);
	return allowed;
}

static int topology_ok(unsigned int sa_count, unsigned int wgps)
{
	return sa_count >= 1u && sa_count <= BC250_CU_SA_MAX && wgps >= 1u && wgps <= BC250_CU_WGP_MAX;
}

/* ---- the setting and the boot guard ---------------------------------------------------------- */

void bc250_cu_decide(const struct bc250_cu_request *req, struct bc250_cu_decision *out)
{
	unsigned int disable, encoded;

	out->mode = BC250_CU_MODE_STOCK;
	out->disable = 0;
	out->encoded = 0;
	out->reason = BC250_CU_REASON_NONE;
	out->mark_pending = 0;
	out->force_stock = 0;
	out->confirmed = 0;
	/* Whatever else happens, a stale mark never outlives a start that does not apply 40. */
	out->clear_pending = req->pending != 0;

	if (!req->mode_present || req->mode == BC250_CU_MODE_STOCK)
		return;
	if (req->mode != BC250_CU_MODE_FULL) {
		out->reason = BC250_CU_REASON_INVALID_SETTING;
		return;
	}
	if (!topology_ok(req->sa_count, req->wgps_per_sa)) {
		out->reason = BC250_CU_REASON_TOPOLOGY;
		return;
	}
	disable = req->disable_present ? req->disable : 0;
	if (disable & ~disable_allowed(req->sa_count, req->wgps_per_sa)) {
		out->reason = BC250_CU_REASON_INVALID_DISABLE;
		return;
	}
	encoded = bc250_cu_encode(BC250_CU_MODE_FULL, disable);
	/* The boot guard: a mark left over means the start that set it died, hung or was stopped
	 * before anybody confirmed 40 on it. Stock, durably, and the reason stays for the tool. */
	if (req->pending != 0) {
		out->reason = BC250_CU_REASON_PENDING_UNCONFIRMED;
		out->force_stock = 1;
		return;
	}
	out->mode = BC250_CU_MODE_FULL;
	out->disable = disable;
	out->encoded = encoded;
	if (req->confirmed == encoded) {
		out->confirmed = 1;
		return;
	}
	out->mark_pending = 1;
}

int bc250_cu_hardware_fallback(unsigned int requested_mode, unsigned int applied_mode)
{
	return requested_mode == BC250_CU_MODE_FULL && applied_mode != BC250_CU_MODE_FULL;
}

/* ---- register values and the CU bitmap ------------------------------------------------------- */

unsigned int bc250_cu_active_wgps_cc(unsigned int cc, unsigned int user, unsigned int wgps_per_sa)
{
	unsigned int inactive = ((cc & CC_INACTIVE_MASK) >> CC_INACTIVE_SHIFT) |
				((user & USER_INACTIVE_MASK) >> USER_INACTIVE_SHIFT);

	return ~inactive & wgp_mask(wgps_per_sa);
}

unsigned int bc250_cu_active_wgps_spi(unsigned int spi, unsigned int wgps_per_sa)
{
	return ((spi & SPI_WGP_MASK) >> SPI_WGP_SHIFT) & wgp_mask(wgps_per_sa);
}

unsigned int bc250_cu_targets(unsigned int mode, unsigned int disable_sa, unsigned int wgps_per_sa,
			      unsigned int stock_cc, unsigned int stock_spi,
			      unsigned int *cc, unsigned int *spi)
{
	unsigned int wgps = wgp_mask(wgps_per_sa);
	unsigned int active_cc = bc250_cu_active_wgps_cc(stock_cc, 0, wgps_per_sa);
	unsigned int spi_field = (stock_spi & SPI_WGP_MASK) >> SPI_WGP_SHIFT;
	unsigned int active;

	*cc = stock_cc;
	*spi = stock_spi;
	if (mode == BC250_CU_MODE_STOCK)
		return BC250_CU_REASON_NONE;
	if (mode != BC250_CU_MODE_FULL)
		return BC250_CU_REASON_INVALID_SETTING;
	if (wgps_per_sa == 0 || wgps_per_sa > BC250_CU_WGP_MAX)
		return BC250_CU_REASON_TOPOLOGY;
	/* What the reference measured on its boards and fact M5 on unit A: the SPI dispatches to exactly
	 * the WGPs the harvest mask leaves active, and to none past the SA's last WGP. Anything else is
	 * a firmware state nobody has measured, and no place to start unlocking from. */
	if ((spi_field & ~wgps) != 0 || active_cc == 0 || (spi_field & wgps) != active_cc)
		return BC250_CU_REASON_STOCK_UNEXPECTED;
	/* Only a WGP the firmware left inactive may stay masked: 40 never ends below stock. */
	if ((disable_sa & ~wgps) != 0 || (disable_sa & active_cc) != 0)
		return BC250_CU_REASON_INVALID_DISABLE;
	active = wgps & ~disable_sa;
	*cc = (stock_cc & ~(wgps << CC_INACTIVE_SHIFT)) | ((disable_sa << CC_INACTIVE_SHIFT) & CC_INACTIVE_MASK);
	*spi = (stock_spi & ~(wgps << SPI_WGP_SHIFT)) | ((active << SPI_WGP_SHIFT) & SPI_WGP_MASK);
	return BC250_CU_REASON_NONE;
}

/* gfx_v10_0_get_cu_info() and gfx_v10_0_get_cu_active_bitmap_per_sh() (gfx_v10_0.c:10078-10170 in
 * driver/amdgpu-import/reference), from a WGP mask instead of a register read. The counter loop and
 * the ao_cu_mask shift, 32-bit wrap included, are upstream's, so the stock result is byte for byte
 * the blob fact M5 was derived into (driver/kmd/test/umd_caps_test.c checks it). */
void bc250_cu_info_from_wgps(const unsigned int *active_wgps, unsigned int se_count,
			     unsigned int sh_per_se, unsigned int max_cu_per_sh,
			     struct bc250_cu_info *out)
{
	unsigned int i, j, k, w;

	memset(out, 0, sizeof(*out));
	for (i = 0; i < se_count && i < 4u; i++) {
		for (j = 0; j < sh_per_se && j < 4u; j++) {
			unsigned int wgps = active_wgps[i * sh_per_se + j] & wgp_mask(max_cu_per_sh >> 1);
			unsigned int bitmap = 0, ao_bitmap = 0, mask = 1, counter = 0;

			for (w = 0; w < 16u; w++)
				if (wgps & (1u << w))
					bitmap |= 3u << (2u * w);
			for (k = 0; k < max_cu_per_sh; k++) {
				if (bitmap & mask) {
					if (counter < max_cu_per_sh)
						ao_bitmap |= mask;
					counter++;
				}
				mask <<= 1;
			}
			out->active += counter;
			if (i < 2u && j < 2u)
				out->ao_mask |= ao_bitmap << (i * 16u + j * 8u);
			out->bitmap[i][j] = bitmap;
			out->ao_bitmap[i][j] = ao_bitmap;
		}
	}
}

/* ---- the register sequence ------------------------------------------------------------------- */

static void read_sa(struct amdgpu_device *adev, unsigned int *cc, unsigned int *user, unsigned int *spi)
{
	*cc = RREG32_SOC15(GC, 0, mmCC_GC_SHADER_ARRAY_CONFIG);
	if (user)
		*user = RREG32_SOC15(GC, 0, mmGC_USER_SHADER_ARRAY_CONFIG);
	*spi = RREG32_SOC15(GC, 0, mmSPI_PG_ENABLE_STATIC_WGP_MASK);
}

/* Writes the targets of every SA whose registers differ from them, reads every SA back. Returns
 * nonzero if any SA does not read back its targets. */
static int write_and_verify(struct amdgpu_device *adev, struct bc250_cu_mode_hw *hw,
			    bc250_cu_select_fn select, const unsigned int *have_cc, const unsigned int *have_spi)
{
	unsigned int i, j, k;
	int mismatch = 0;

	for (i = 0; i < hw->se_count; i++) {
		for (j = 0; j < hw->sh_per_se; j++) {
			k = i * hw->sh_per_se + j;
			if (have_cc[k] == hw->target_cc[k] && have_spi[k] == hw->target_spi[k])
				continue;
			select(adev, i, j, 0xffffffff);
			/* The reference's order: the enumeration mask, then the dispatch gate. */
			WREG32_SOC15(GC, 0, mmCC_GC_SHADER_ARRAY_CONFIG, hw->target_cc[k]);
			WREG32_SOC15(GC, 0, mmSPI_PG_ENABLE_STATIC_WGP_MASK, hw->target_spi[k]);
			hw->wrote = 1;
		}
	}
	for (i = 0; i < hw->se_count; i++) {
		for (j = 0; j < hw->sh_per_se; j++) {
			k = i * hw->sh_per_se + j;
			select(adev, i, j, 0xffffffff);
			read_sa(adev, &hw->cc[k], &hw->user[k], &hw->spi[k]);
			if (hw->cc[k] != hw->target_cc[k] || hw->spi[k] != hw->target_spi[k])
				mismatch = 1;
		}
	}
	return mismatch;
}

static void stock_targets(struct bc250_cu_mode_hw *hw)
{
	unsigned int k;

	for (k = 0; k < hw->sa_count; k++) {
		hw->target_cc[k] = hw->stock_cc[k];
		hw->target_spi[k] = hw->stock_spi[k];
	}
}

void bc250_cu_mode_apply(struct amdgpu_device *adev, void *ctx, bc250_cu_select_fn select)
{
	struct bc250_cu_mode_hw *hw = (struct bc250_cu_mode_hw *)ctx;
	unsigned int i, j, k, mode, reason;
	unsigned int have_cc[BC250_CU_SA_MAX], have_spi[BC250_CU_SA_MAX];

	if (hw == NULL || select == NULL)
		return;
	hw->ran = 1;
	hw->wrote = 0;
	hw->applied = 0;
	hw->consistent = 0;
	hw->se_count = adev->gfx.config.max_shader_engines;
	hw->sh_per_se = adev->gfx.config.max_sh_per_se;
	hw->sa_count = hw->se_count * hw->sh_per_se;
	hw->wgps_per_sa = hw->max_cu_per_sh >> 1;
	/* Outside the topology this was written for nothing is read or written. */
	if (hw->se_count == 0 || hw->sh_per_se == 0 || hw->sa_count > BC250_CU_SA_MAX ||
	    hw->wgps_per_sa == 0 || hw->wgps_per_sa > BC250_CU_WGP_MAX || (hw->max_cu_per_sh & 1u)) {
		hw->ran = 0;
		hw->reason = BC250_CU_REASON_TOPOLOGY;
		return;
	}

	/* Not written, observed: power gating would decide on its own which WGPs have power. */
	hw->rlc_pg_cntl = RREG32_SOC15(GC, 0, mmRLC_PG_CNTL);
	hw->rlc_aon_wgp_mask = RREG32_SOC15(GC, 0, mmRLC_PG_ALWAYS_ON_WGP_MASK);

	for (i = 0; i < hw->se_count; i++) {
		for (j = 0; j < hw->sh_per_se; j++) {
			k = i * hw->sh_per_se + j;
			select(adev, i, j, 0xffffffff);
			read_sa(adev, &hw->entry_cc[k], NULL, &hw->entry_spi[k]);
			have_cc[k] = hw->entry_cc[k];
			have_spi[k] = hw->entry_spi[k];
			/* First start of this boot: what the firmware left is stock. A later start of the
			 * same boot is handed the record, because the registers may hold our own writes. */
			if (!hw->have_stock) {
				hw->stock_cc[k] = hw->entry_cc[k];
				hw->stock_spi[k] = hw->entry_spi[k];
			}
		}
	}

	mode = hw->mode == BC250_CU_MODE_FULL ? BC250_CU_MODE_FULL : BC250_CU_MODE_STOCK;
	reason = BC250_CU_REASON_NONE;
	if (mode == BC250_CU_MODE_FULL && hw->rlc_pg_cntl != 0)
		reason = BC250_CU_REASON_POWER_GATING;
	for (k = 0; k < hw->sa_count && reason == BC250_CU_REASON_NONE; k++)
		reason = bc250_cu_targets(mode, (hw->disable >> (k * BC250_CU_WGP_MAX)) & wgp_mask(BC250_CU_WGP_MAX),
					  hw->wgps_per_sa, hw->stock_cc[k], hw->stock_spi[k],
					  &hw->target_cc[k], &hw->target_spi[k]);
	if (reason != BC250_CU_REASON_NONE) {
		mode = BC250_CU_MODE_STOCK;
		stock_targets(hw);
	}

	if (write_and_verify(adev, hw, select, have_cc, have_spi)) {
		if (mode == BC250_CU_MODE_FULL) {
			/* Back to the firmware's pair on every SA, from whatever the failed pass left. */
			reason = BC250_CU_REASON_READBACK;
			mode = BC250_CU_MODE_STOCK;
			stock_targets(hw);
			for (k = 0; k < hw->sa_count; k++) {
				have_cc[k] = hw->cc[k];
				have_spi[k] = hw->spi[k];
			}
			if (write_and_verify(adev, hw, select, have_cc, have_spi))
				reason = BC250_CU_REASON_RESTORE_FAILED;
		} else {
			reason = BC250_CU_REASON_RESTORE_FAILED;
		}
	}
	select(adev, 0xffffffff, 0xffffffff, 0xffffffff);

	hw->reason = reason;
	hw->applied = reason == BC250_CU_REASON_RESTORE_FAILED ? 0 : mode;
	hw->consistent = 1;
	for (k = 0; k < hw->sa_count; k++) {
		unsigned int by_cc = bc250_cu_active_wgps_cc(hw->cc[k], hw->user[k], hw->wgps_per_sa);
		unsigned int by_spi = bc250_cu_active_wgps_spi(hw->spi[k], hw->wgps_per_sa);

		hw->active_wgps[k] = by_cc | by_spi;
		if (by_cc != by_spi)
			hw->consistent = 0;
	}
}
