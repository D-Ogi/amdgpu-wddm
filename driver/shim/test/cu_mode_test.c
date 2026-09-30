/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Host test of the CU mode (driver/shim/bc250_cu_mode.c, docs/design/cu-mode.md): the setting and
 * the boot guard (bc250_cu_decide), the register values (bc250_cu_targets), the CU bitmap the caps
 * report (bc250_cu_info_from_wgps), and the register sequence itself (bc250_cu_mode_apply) against a
 * model of the four banked shader arrays behind GRBM_GFX_INDEX. Every register the sequence touches
 * must be in the miniport's Gfx allow table (driver/kmd/regs.generated.h), or the lab start would
 * stop at the first refused access.
 *
 *   pwsh driver\shim\test\run_cu_mode.ps1
 *
 * Stock values are unit A's (the reference's measurement, fact M5's derivation): CC 0xfff80000
 * (WGPs 3 and 4 inactive), SPI 0x7 on every shader array.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amdgpu.h"
#include "bc250_cu_mode.h"
#include "nv.h"                         /* cyan_skillfish_reg_base_init() */

#include "gc/gc_10_1_0_offset.h"
#include "gc/gc_10_1_0_sh_mask.h"
#include "soc15_common.h"

/* The miniport's allow tables. The DCN table's element type lives in bc250kmd.h, which is kernel
 * only; this is its layout, for the one table the test does not read. */
typedef struct { const char *Name; unsigned long Offset; } BC250_DCN_REG_INFO;
#define BC250_REGS_WITH_TABLES
#include "../../kmd/regs.generated.h"

#define STOCK_CC	0xfff80000u
#define STOCK_SPI	0x7u
#define FULL_CC		0xffe00000u
#define FULL_SPI	0x1Fu
#define SA		4u

static int g_failures, g_checks;

#define CHECK(cond) \
	do { g_checks++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)
#define CHECK_EQ(a, b) \
	do { unsigned long long a_ = (a), b_ = (b); g_checks++; if (a_ != b_) { \
		fprintf(stderr, "FAIL %s:%d: %s == 0x%llX, expected %s == 0x%llX\n", __FILE__, __LINE__, #a, a_, #b, b_); \
		g_failures++; } } while (0)

/* ---- the register model ------------------------------------------------------------------ */

static struct amdgpu_device *g_adev;
static unsigned int g_idx_grbm, g_idx_cc, g_idx_user, g_idx_spi, g_idx_pg, g_idx_aon;

static struct {
	unsigned int grbm;
	unsigned int cc[SA], user[SA], spi[SA];
	unsigned int pg_cntl, aon;
	unsigned int stuck_spi_sa;	/* SA + 1 whose SPI ignores writes, 0 = none */
	unsigned int drop_after;	/* CC/SPI writes after this many are dropped, 0 = never */
	unsigned int banked_writes;	/* CC/SPI writes that reached the model */
	unsigned int refused;		/* accesses outside the Gfx allow table */
} m;

static void model_reset(unsigned int cc, unsigned int spi)
{
	unsigned int k;

	memset(&m, 0, sizeof(m));
	m.grbm = 0xE0000000u;
	for (k = 0; k < SA; k++) {
		m.cc[k] = cc;
		m.user[k] = 0;
		m.spi[k] = spi;
	}
	m.aon = 0;
}

static int allowed(unsigned int dword_index)
{
	unsigned int i;

	for (i = 0; i < BC250_MMIO_GFX_ALLOW_COUNT; i++)
		if (g_MmioGfxAllow[i] == dword_index * 4u)
			return 1;
	return 0;
}

/* The SAs a GRBM_GFX_INDEX value addresses: one, or all of them for a broadcast. */
static unsigned int selected(unsigned int *list)
{
	unsigned int se_bc = REG_GET_FIELD(m.grbm, GRBM_GFX_INDEX, SE_BROADCAST_WRITES);
	unsigned int sa_bc = REG_GET_FIELD(m.grbm, GRBM_GFX_INDEX, SA_BROADCAST_WRITES);
	unsigned int se = REG_GET_FIELD(m.grbm, GRBM_GFX_INDEX, SE_INDEX);
	unsigned int sh = REG_GET_FIELD(m.grbm, GRBM_GFX_INDEX, SA_INDEX);
	unsigned int i, j, n = 0;

	for (i = 0; i < 2u; i++)
		for (j = 0; j < 2u; j++)
			if ((se_bc || i == se) && (sa_bc || j == sh))
				list[n++] = i * 2u + j;
	return n;
}

unsigned int bc250_shim_rreg(struct amdgpu_device *adev, unsigned int dword_index)
{
	unsigned int list[SA], n;

	(void)adev;
	if (!allowed(dword_index))
		m.refused++;
	n = selected(list);
	/* A broadcast read answers from the first SA, as the hardware's SE0/SA0. */
	if (dword_index == g_idx_grbm) return m.grbm;
	if (dword_index == g_idx_cc) return n ? m.cc[list[0]] : 0;
	if (dword_index == g_idx_user) return n ? m.user[list[0]] : 0;
	if (dword_index == g_idx_spi) return n ? m.spi[list[0]] : 0;
	if (dword_index == g_idx_pg) return m.pg_cntl;
	if (dword_index == g_idx_aon) return m.aon;
	fprintf(stderr, "read of unmodelled register 0x%05X\n", dword_index * 4u);
	g_failures++;
	return 0;
}

void bc250_shim_wreg(struct amdgpu_device *adev, unsigned int dword_index, unsigned int value)
{
	unsigned int list[SA], n, k;

	(void)adev;
	if (!allowed(dword_index))
		m.refused++;
	if (dword_index == g_idx_grbm) {
		m.grbm = value;
		return;
	}
	if (dword_index != g_idx_cc && dword_index != g_idx_spi) {
		fprintf(stderr, "write of register 0x%05X, which the CU mode must never write\n", dword_index * 4u);
		g_failures++;
		return;
	}
	m.banked_writes++;
	if (m.drop_after && m.banked_writes > m.drop_after)
		return;
	n = selected(list);
	for (k = 0; k < n; k++) {
		if (dword_index == g_idx_cc)
			m.cc[list[k]] = value;
		else if (m.stuck_spi_sa != list[k] + 1u)
			m.spi[list[k]] = value;
	}
}

void bc250_shim_udelay(unsigned int usec) { (void)usec; }
void bc250_shim_wdoorbell64(struct amdgpu_device *adev, unsigned int index, unsigned long long value)
{
	(void)adev; (void)index; (void)value;
}
void bc250_shim_log(int level, void *dev, const char *fmt, ...) { (void)level; (void)dev; (void)fmt; }
int bc250_shim_mem_alloc(struct amdgpu_device *adev, enum bc250_mem_domain domain,
			 unsigned int size, unsigned int align, struct bc250_mem *out)
{
	(void)adev; (void)domain; (void)size; (void)align; (void)out;
	return -1;
}
void bc250_shim_mem_free(struct amdgpu_device *adev, struct bc250_mem *mem) { (void)adev; (void)mem; }

/* gfx_v10_0_select_se_sh(), as bc250_gfx.c transcribes it (there it is static). */
static void select_se_sh(struct amdgpu_device *adev, unsigned int se, unsigned int sh, unsigned int instance)
{
	u32 data;

	if (instance == 0xffffffff)
		data = REG_SET_FIELD(0, GRBM_GFX_INDEX, INSTANCE_BROADCAST_WRITES, 1);
	else
		data = REG_SET_FIELD(0, GRBM_GFX_INDEX, INSTANCE_INDEX, instance);
	if (se == 0xffffffff)
		data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SE_BROADCAST_WRITES, 1);
	else
		data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SE_INDEX, se);
	if (sh == 0xffffffff)
		data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SA_BROADCAST_WRITES, 1);
	else
		data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SA_INDEX, sh);
	WREG32_SOC15(GC, 0, mmGRBM_GFX_INDEX, data);
}

static unsigned int broadcast_index(void)
{
	u32 data = REG_SET_FIELD(0, GRBM_GFX_INDEX, INSTANCE_BROADCAST_WRITES, 1);

	data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SE_BROADCAST_WRITES, 1);
	return REG_SET_FIELD(data, GRBM_GFX_INDEX, SA_BROADCAST_WRITES, 1);
}

static void run(struct bc250_cu_mode_hw *hw, unsigned int mode, unsigned int disable,
		const unsigned int *stock_cc, const unsigned int *stock_spi)
{
	unsigned int k;

	memset(hw, 0, sizeof(*hw));
	hw->mode = mode;
	hw->disable = disable;
	hw->max_cu_per_sh = 10;
	if (stock_cc) {
		hw->have_stock = 1;
		for (k = 0; k < SA; k++) {
			hw->stock_cc[k] = stock_cc[k];
			hw->stock_spi[k] = stock_spi[k];
		}
	}
	bc250_cu_mode_apply(g_adev, hw, select_se_sh);
}

static void check_registers(unsigned int cc, unsigned int spi)
{
	unsigned int k;

	for (k = 0; k < SA; k++) {
		CHECK_EQ(m.cc[k], cc);
		CHECK_EQ(m.spi[k], spi);
	}
}

/* ---- cases ------------------------------------------------------------------------------- */

static struct bc250_cu_request request(int present, unsigned int mode)
{
	struct bc250_cu_request r;

	memset(&r, 0, sizeof(r));
	r.mode_present = present;
	r.mode = mode;
	r.sa_count = SA;
	r.wgps_per_sa = 5;
	return r;
}

static void test_decide(void)
{
	struct bc250_cu_request r;
	struct bc250_cu_decision d;

	r = request(0, 0);
	bc250_cu_decide(&r, &d);
	CHECK_EQ(d.mode, 24); CHECK_EQ(d.reason, BC250_CU_REASON_NONE);
	CHECK(!d.mark_pending && !d.clear_pending && !d.force_stock);

	r = request(1, 24);
	bc250_cu_decide(&r, &d);
	CHECK_EQ(d.mode, 24); CHECK_EQ(d.reason, BC250_CU_REASON_NONE);

	r = request(1, 32);
	bc250_cu_decide(&r, &d);
	CHECK_EQ(d.mode, 24); CHECK_EQ(d.reason, BC250_CU_REASON_INVALID_SETTING);
	CHECK(!d.force_stock && !d.mark_pending);

	r = request(1, 0);
	bc250_cu_decide(&r, &d);
	CHECK_EQ(d.reason, BC250_CU_REASON_INVALID_SETTING);

	/* A fresh 40: pending before anything is written. */
	r = request(1, 40);
	bc250_cu_decide(&r, &d);
	CHECK_EQ(d.mode, 40); CHECK_EQ(d.encoded, 40); CHECK(d.mark_pending); CHECK(!d.confirmed);
	CHECK(!d.clear_pending);

	/* The boot guard: the mark of a start nobody confirmed. */
	r.pending = 40;
	bc250_cu_decide(&r, &d);
	CHECK_EQ(d.mode, 24); CHECK_EQ(d.reason, BC250_CU_REASON_PENDING_UNCONFIRMED);
	CHECK(d.force_stock); CHECK(d.clear_pending); CHECK(!d.mark_pending);

	/* Confirmed on an earlier start: nothing pending. */
	r = request(1, 40);
	r.confirmed = 40;
	bc250_cu_decide(&r, &d);
	CHECK_EQ(d.mode, 40); CHECK(d.confirmed); CHECK(!d.mark_pending);

	/* A confirmation covers only the request it was made for. */
	r.disable_present = 1;
	r.disable = 1u << 4;		/* WGP 4 of SA 0 */
	bc250_cu_decide(&r, &d);
	CHECK_EQ(d.mode, 40); CHECK_EQ(d.encoded, 40u | (0x10u << 8)); CHECK(d.mark_pending); CHECK(!d.confirmed);

	/* Disable bits past the fourth SA. */
	r.disable = 1u << 20;
	bc250_cu_decide(&r, &d);
	CHECK_EQ(d.mode, 24); CHECK_EQ(d.reason, BC250_CU_REASON_INVALID_DISABLE);

	/* 24 with a stale mark: the mark goes. */
	r = request(1, 24);
	r.pending = 40;
	bc250_cu_decide(&r, &d);
	CHECK_EQ(d.mode, 24); CHECK(d.clear_pending); CHECK(!d.force_stock);

	r = request(1, 40);
	r.wgps_per_sa = 6;
	bc250_cu_decide(&r, &d);
	CHECK_EQ(d.mode, 24); CHECK_EQ(d.reason, BC250_CU_REASON_TOPOLOGY);

	CHECK(bc250_cu_hardware_fallback(40, 24));
	CHECK(bc250_cu_hardware_fallback(40, 0));
	CHECK(!bc250_cu_hardware_fallback(40, 40));
	CHECK(!bc250_cu_hardware_fallback(24, 24));
	CHECK(!bc250_cu_hardware_fallback(24, 0));
}

static void test_targets(void)
{
	unsigned int cc, spi;

	CHECK_EQ(bc250_cu_active_wgps_cc(STOCK_CC, 0, 5), 0x7);
	CHECK_EQ(bc250_cu_active_wgps_cc(FULL_CC, 0, 5), 0x1F);
	CHECK_EQ(bc250_cu_active_wgps_cc(FULL_CC, 1u << 16, 5), 0x1E);	/* USER masks too */
	CHECK_EQ(bc250_cu_active_wgps_spi(STOCK_SPI, 5), 0x7);

	CHECK_EQ(bc250_cu_targets(24, 0, 5, STOCK_CC, STOCK_SPI, &cc, &spi), BC250_CU_REASON_NONE);
	CHECK_EQ(cc, STOCK_CC); CHECK_EQ(spi, STOCK_SPI);
	/* The reference's pair. */
	CHECK_EQ(bc250_cu_targets(40, 0, 5, STOCK_CC, STOCK_SPI, &cc, &spi), BC250_CU_REASON_NONE);
	CHECK_EQ(cc, FULL_CC); CHECK_EQ(spi, FULL_SPI);
	/* WGP 4 stays masked: 36 CUs on this SA's share. */
	CHECK_EQ(bc250_cu_targets(40, 0x10, 5, STOCK_CC, STOCK_SPI, &cc, &spi), BC250_CU_REASON_NONE);
	CHECK_EQ(cc, 0xfff00000u); CHECK_EQ(spi, 0xFu);
	/* Never below stock. */
	CHECK_EQ(bc250_cu_targets(40, 0x1, 5, STOCK_CC, STOCK_SPI, &cc, &spi), BC250_CU_REASON_INVALID_DISABLE);
	CHECK_EQ(cc, STOCK_CC); CHECK_EQ(spi, STOCK_SPI);
	/* A firmware state nobody measured. */
	CHECK_EQ(bc250_cu_targets(40, 0, 5, STOCK_CC, 0x3, &cc, &spi), BC250_CU_REASON_STOCK_UNEXPECTED);
	CHECK_EQ(bc250_cu_targets(40, 0, 5, STOCK_CC, 0x27, &cc, &spi), BC250_CU_REASON_STOCK_UNEXPECTED);
	CHECK_EQ(bc250_cu_targets(40, 0, 5, 0xffff0000u, 0x0, &cc, &spi), BC250_CU_REASON_STOCK_UNEXPECTED);
	CHECK_EQ(bc250_cu_targets(33, 0, 5, STOCK_CC, STOCK_SPI, &cc, &spi), BC250_CU_REASON_INVALID_SETTING);
}

static void test_info(void)
{
	const unsigned int stock[SA] = { 7, 7, 7, 7 }, full[SA] = { 0x1F, 0x1F, 0x1F, 0x1F };
	const unsigned int mixed[SA] = { 0x1F, 0x0F, 0x1F, 0x1F };
	struct bc250_cu_info info;
	unsigned int i, j;

	/* Stock is byte for byte the measured template (umd_caps_test.c checks the template's side). */
	bc250_cu_info_from_wgps(stock, 2, 2, 10, &info);
	CHECK_EQ(info.active, 24); CHECK_EQ(info.ao_mask, 0x3F3F3F3Fu);
	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++) {
			CHECK_EQ(info.bitmap[i][j], i < 2 && j < 2 ? 0x3Fu : 0u);
			CHECK_EQ(info.ao_bitmap[i][j], i < 2 && j < 2 ? 0x3Fu : 0u);
		}
	bc250_cu_info_from_wgps(full, 2, 2, 10, &info);
	CHECK_EQ(info.active, 40); CHECK_EQ(info.ao_mask, 0xFFFFFFFFu);	/* amdgpu's wrap */
	CHECK_EQ(info.bitmap[0][0], 0x3FF); CHECK_EQ(info.bitmap[1][1], 0x3FF); CHECK_EQ(info.ao_bitmap[1][0], 0x3FF);
	bc250_cu_info_from_wgps(mixed, 2, 2, 10, &info);
	CHECK_EQ(info.active, 38); CHECK_EQ(info.bitmap[0][1], 0xFF);
}

static void check_broadcast_left(void)
{
	CHECK_EQ(m.grbm, broadcast_index());
	CHECK_EQ(m.refused, 0);
}

static void test_apply(void)
{
	struct bc250_cu_mode_hw hw;
	unsigned int stock_cc[SA], stock_spi[SA], k;

	for (k = 0; k < SA; k++) { stock_cc[k] = STOCK_CC; stock_spi[k] = STOCK_SPI; }

	/* Cold boot, 24: read only. */
	model_reset(STOCK_CC, STOCK_SPI);
	run(&hw, 24, 0, NULL, NULL);
	CHECK(hw.ran); CHECK_EQ(hw.applied, 24); CHECK_EQ(hw.reason, BC250_CU_REASON_NONE);
	CHECK_EQ(m.banked_writes, 0); CHECK(!hw.wrote); CHECK(hw.consistent);
	CHECK_EQ(hw.stock_cc[3], STOCK_CC); CHECK_EQ(hw.stock_spi[2], STOCK_SPI);
	CHECK_EQ(hw.active_wgps[0], 0x7);
	check_registers(STOCK_CC, STOCK_SPI);
	check_broadcast_left();

	/* Cold boot, 40. */
	model_reset(STOCK_CC, STOCK_SPI);
	run(&hw, 40, 0, NULL, NULL);
	CHECK_EQ(hw.applied, 40); CHECK_EQ(hw.reason, BC250_CU_REASON_NONE); CHECK(hw.wrote); CHECK(hw.consistent);
	CHECK_EQ(m.banked_writes, 8);
	for (k = 0; k < SA; k++) CHECK_EQ(hw.active_wgps[k], 0x1F);
	check_registers(FULL_CC, FULL_SPI);
	check_broadcast_left();

	/* 40 with one WGP of SA 3 kept masked. */
	model_reset(STOCK_CC, STOCK_SPI);
	run(&hw, 40, 0x10u << 15, NULL, NULL);
	CHECK_EQ(hw.applied, 40); CHECK_EQ(m.cc[3], 0xfff00000u); CHECK_EQ(m.spi[3], 0xF); CHECK_EQ(m.spi[0], FULL_SPI);
	CHECK_EQ(hw.active_wgps[3], 0xF);

	/* 40 with a disable bit on a stock-active WGP: stock, nothing written. */
	model_reset(STOCK_CC, STOCK_SPI);
	run(&hw, 40, 0x1, NULL, NULL);
	CHECK_EQ(hw.applied, 24); CHECK_EQ(hw.reason, BC250_CU_REASON_INVALID_DISABLE); CHECK_EQ(m.banked_writes, 0);

	/* RLC power gating on at the constants stage with an enable the PSP load does not leave: 40
	 * refused, nothing written, alone and next to the static one. */
	{
		static const unsigned int enables[] = { 0x1, 0x4, 0x10, 0x1 | 0x8, 0x4 | 0x8, 0x10 | 0x8 };
		unsigned int e;

		for (e = 0; e < sizeof(enables) / sizeof(enables[0]); e++) {
			model_reset(STOCK_CC, STOCK_SPI);
			m.pg_cntl = enables[e] | 0x00800000u;
			run(&hw, 40, 0, NULL, NULL);
			CHECK_EQ(hw.applied, 24); CHECK_EQ(hw.reason, BC250_CU_REASON_POWER_GATING);
			CHECK_EQ(m.banked_writes, 0);
			CHECK_EQ(hw.rlc_pg_cntl, enables[e] | 0x00800000u);
			check_registers(STOCK_CC, STOCK_SPI);
			check_broadcast_left();
		}
	}
	CHECK_EQ(bc250_cu_pg_enables(0xFFFFFFFFu), 0x1Du);

	/* A warm device restart on unit A (KMD 175 and 176, 2026-09-30): only bit 23, set by our own RLC
	 * start (SMU handshake off). Not a PG enable: 40 applies, and after the RLC stage stays. */
	model_reset(STOCK_CC, STOCK_SPI);
	m.pg_cntl = 0x00800000u;
	run(&hw, 40, 0, NULL, NULL);
	CHECK_EQ(hw.applied, 40); CHECK_EQ(hw.reason, BC250_CU_REASON_NONE); CHECK_EQ(hw.rlc_pg_cntl, 0x00800000u);
	CHECK(!hw.after_rlc_ran);
	bc250_cu_mode_after_rlc(g_adev, &hw, select_se_sh);
	CHECK(hw.after_rlc_ran); CHECK_EQ(hw.rlc_pg_cntl_after, 0x00800000u);
	CHECK_EQ(hw.applied, 40); CHECK_EQ(hw.reason, BC250_CU_REASON_NONE); CHECK_EQ(m.banked_writes, 8);
	check_registers(FULL_CC, FULL_SPI);
	check_broadcast_left();

	/* A cold start (E11: the PSP-started RLC leaves 0x8 until the RLC stage writes 0): 40 applies at
	 * the constants stage, and holds because the RLC stage turned PG off. */
	model_reset(STOCK_CC, STOCK_SPI);
	m.pg_cntl = 0x8;
	run(&hw, 40, 0, NULL, NULL);
	CHECK_EQ(hw.applied, 40); CHECK_EQ(hw.reason, BC250_CU_REASON_NONE); CHECK_EQ(hw.rlc_pg_cntl, 0x8);
	check_registers(FULL_CC, FULL_SPI);
	m.pg_cntl = 0x00800000u;
	bc250_cu_mode_after_rlc(g_adev, &hw, select_se_sh);
	CHECK_EQ(hw.applied, 40); CHECK_EQ(hw.reason, BC250_CU_REASON_NONE); CHECK_EQ(hw.rlc_pg_cntl_after, 0x00800000u);
	CHECK_EQ(m.banked_writes, 8);
	check_registers(FULL_CC, FULL_SPI);
	check_broadcast_left();

	/* The same, but an enable is still on after the RLC stage: every SA back to stock, verified,
	 * reason POWER_GATING. Each enable alone. */
	{
		static const unsigned int after[] = { 0x1, 0x4, 0x8, 0x10 };
		unsigned int e;

		for (e = 0; e < sizeof(after) / sizeof(after[0]); e++) {
			model_reset(STOCK_CC, STOCK_SPI);
			m.pg_cntl = 0x8;
			run(&hw, 40, 0, NULL, NULL);
			CHECK_EQ(hw.applied, 40);
			m.pg_cntl = after[e] | 0x00800000u;
			bc250_cu_mode_after_rlc(g_adev, &hw, select_se_sh);
			CHECK_EQ(hw.applied, 24); CHECK_EQ(hw.reason, BC250_CU_REASON_POWER_GATING);
			CHECK_EQ(hw.rlc_pg_cntl, 0x8); CHECK_EQ(hw.rlc_pg_cntl_after, after[e] | 0x00800000u);
			CHECK_EQ(m.banked_writes, 16); CHECK(hw.consistent);
			for (k = 0; k < SA; k++) CHECK_EQ(hw.active_wgps[k], 0x7);
			check_registers(STOCK_CC, STOCK_SPI);
			check_broadcast_left();
			CHECK(bc250_cu_hardware_fallback(40, hw.applied));
		}
	}

	/* ...and stock does not read back either: unknown, as for a failed readback. */
	model_reset(STOCK_CC, STOCK_SPI);
	m.pg_cntl = 0x8;
	run(&hw, 40, 0, NULL, NULL);
	m.pg_cntl = 0x1;
	m.stuck_spi_sa = 2;
	bc250_cu_mode_after_rlc(g_adev, &hw, select_se_sh);
	CHECK_EQ(hw.applied, 0); CHECK_EQ(hw.reason, BC250_CU_REASON_RESTORE_FAILED); CHECK(!hw.consistent);
	CHECK_EQ(hw.active_wgps[1], 0x1F);
	check_broadcast_left();

	/* After the RLC stage with 24 applied or the stage refused: read only, whatever PG says. */
	model_reset(STOCK_CC, STOCK_SPI);
	m.pg_cntl = 0x8;
	run(&hw, 24, 0, NULL, NULL);
	m.pg_cntl = 0x1D;
	bc250_cu_mode_after_rlc(g_adev, &hw, select_se_sh);
	CHECK(hw.after_rlc_ran); CHECK_EQ(hw.applied, 24); CHECK_EQ(hw.reason, BC250_CU_REASON_NONE);
	CHECK_EQ(m.banked_writes, 0);
	check_broadcast_left();

	/* Stock the reference never saw: refused, nothing written. */
	model_reset(STOCK_CC, 0x3);
	run(&hw, 40, 0, NULL, NULL);
	CHECK_EQ(hw.applied, 24); CHECK_EQ(hw.reason, BC250_CU_REASON_STOCK_UNEXPECTED); CHECK_EQ(m.banked_writes, 0);

	/* SA 2's SPI does not take the write: every SA back to stock. */
	model_reset(STOCK_CC, STOCK_SPI);
	m.stuck_spi_sa = 3;
	run(&hw, 40, 0, NULL, NULL);
	CHECK_EQ(hw.applied, 24); CHECK_EQ(hw.reason, BC250_CU_REASON_READBACK); CHECK(hw.consistent);
	check_registers(STOCK_CC, STOCK_SPI);
	for (k = 0; k < SA; k++) CHECK_EQ(hw.active_wgps[k], 0x7);
	check_broadcast_left();

	/* Writes stop halfway and the restore cannot land either: unknown, and the caps follow what
	 * the registers read (the union of both views, the safe side for scratch). */
	model_reset(STOCK_CC, STOCK_SPI);
	m.drop_after = 3;			/* SA 0 both, SA 1 CC only */
	run(&hw, 40, 0, NULL, NULL);
	CHECK_EQ(hw.applied, 0); CHECK_EQ(hw.reason, BC250_CU_REASON_RESTORE_FAILED); CHECK(!hw.consistent);
	CHECK_EQ(hw.active_wgps[0], 0x1F); CHECK_EQ(hw.active_wgps[1], 0x1F); CHECK_EQ(hw.active_wgps[2], 0x7);
	check_broadcast_left();

	/* Warm restart of the same boot, 40 left in the registers, 24 asked, the record at hand. */
	model_reset(FULL_CC, FULL_SPI);
	run(&hw, 24, 0, stock_cc, stock_spi);
	CHECK_EQ(hw.applied, 24); CHECK_EQ(hw.reason, BC250_CU_REASON_NONE); CHECK(hw.wrote);
	CHECK_EQ(hw.entry_cc[0], FULL_CC);
	check_registers(STOCK_CC, STOCK_SPI);
	for (k = 0; k < SA; k++) CHECK_EQ(hw.active_wgps[k], 0x7);

	/* The same without the record: 40 is taken for stock, and the caps still say 40 because that
	 * is what the SPI dispatches to. Wrong count of the harvest, right size of scratch. */
	model_reset(FULL_CC, FULL_SPI);
	run(&hw, 24, 0, NULL, NULL);
	CHECK_EQ(hw.applied, 24); CHECK_EQ(m.banked_writes, 0);
	for (k = 0; k < SA; k++) CHECK_EQ(hw.active_wgps[k], 0x1F);

	/* Warm restart, 40 again, already in place: nothing to write. */
	model_reset(FULL_CC, FULL_SPI);
	run(&hw, 40, 0, stock_cc, stock_spi);
	CHECK_EQ(hw.applied, 40); CHECK_EQ(m.banked_writes, 0); CHECK(!hw.wrote);

	/* A retained-power resume after a power loss: registers back at stock, mode 40 re-applied. */
	model_reset(STOCK_CC, STOCK_SPI);
	run(&hw, 40, 0, stock_cc, stock_spi);
	CHECK_EQ(hw.applied, 40); check_registers(FULL_CC, FULL_SPI);

	/* Outside the topology: nothing touched at all. */
	model_reset(STOCK_CC, STOCK_SPI);
	memset(&hw, 0, sizeof(hw));
	hw.mode = 40;
	hw.max_cu_per_sh = 12;
	bc250_cu_mode_apply(g_adev, &hw, select_se_sh);
	CHECK(!hw.ran); CHECK_EQ(hw.reason, BC250_CU_REASON_TOPOLOGY); CHECK_EQ(m.banked_writes, 0);
}

int main(void)
{
	g_adev = (struct amdgpu_device *)calloc(1, sizeof(*g_adev));
	if (!g_adev)
		return 2;
	cyan_skillfish_reg_base_init(g_adev);
	g_adev->gfx.config.max_shader_engines = 2;
	g_adev->gfx.config.max_sh_per_se = 2;
	{
		struct amdgpu_device *adev = g_adev;

		g_idx_grbm = SOC15_REG_OFFSET(GC, 0, mmGRBM_GFX_INDEX);
		g_idx_cc = SOC15_REG_OFFSET(GC, 0, mmCC_GC_SHADER_ARRAY_CONFIG);
		g_idx_user = SOC15_REG_OFFSET(GC, 0, mmGC_USER_SHADER_ARRAY_CONFIG);
		g_idx_spi = SOC15_REG_OFFSET(GC, 0, mmSPI_PG_ENABLE_STATIC_WGP_MASK);
		g_idx_pg = SOC15_REG_OFFSET(GC, 0, mmRLC_PG_CNTL);
		g_idx_aon = SOC15_REG_OFFSET(GC, 0, mmRLC_PG_ALWAYS_ON_WGP_MASK);
	}
	/* The byte offsets regcalc gives (tools/regcalc, and the allow table's own comments). */
	CHECK_EQ(g_idx_cc * 4u, 0x089BC);
	CHECK_EQ(g_idx_spi * 4u, 0x0935C);
	CHECK_EQ(g_idx_aon * 4u, 0x3B14C);
	CHECK_EQ(g_idx_grbm * 4u, 0x30800);
	CHECK(allowed(g_idx_grbm) && allowed(g_idx_cc) && allowed(g_idx_user) && allowed(g_idx_spi) &&
	      allowed(g_idx_pg) && allowed(g_idx_aon));

	test_decide();
	test_targets();
	test_info();
	test_apply();
	free(g_adev);
	if (g_failures == 0) {
		printf("cu_mode_test: all %d checks passed\n", g_checks);
		return 0;
	}
	fprintf(stderr, "cu_mode_test: %d of %d checks failed\n", g_failures, g_checks);
	return 1;
}
