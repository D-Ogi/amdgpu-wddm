/* The SMU metrics table policy (driver/shim/bc250_smu_metrics.c, KMD 0.7.215), driven on the host.
 *
 * What this test is for: the driver asks the SMU to write one table into one page, then believes a few numbers in
 * it. So the things that must not drift are the offsets of those numbers, the three messages and the one argument
 * each may carry, the page the driver may name, the decode's refusals, and the reader's rules: one read per
 * period, a refusal ends the reads for the boot, an owner that is offline costs nothing, three bad tables in a
 * row end the reads for the start, and a reading older than three periods is no reading. Two layouts: the stock
 * six-core table and the eight-core one of the patched community SMU firmware, told apart by the poisoned tail of
 * the copy and latched after three good tables in a row.
 *
 *   pwsh driver\shim\test\run_smu_metrics.ps1
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "bc250_smu_metrics.h"
#include "bc250_board_envelope.h"

static int checks, failures;

#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

#define STOCK BC250_SMU_METRICS_LAYOUT_STOCK
#define EIGHT BC250_SMU_METRICS_LAYOUT_8CORE
#define COPY BC250_SMU_METRICS_COPY_BYTES

/* ---- the layout ---------------------------------------------------------------------------------------- */

/* SmuMetricsTable_t and SmuMetrics_t of Linux v6.18 swsmu/inc/pmfw_if/smu11_driver_if_cyan_skillfish.h
 * (MP1_DRIVER_IF_VERSION 0x8), written out here field by field in our own names, with the C compiler's natural
 * alignment, which is how amdgpu compiles its copy. The driver's offsets are plain numbers; this declaration is the
 * independent computation they are checked against. */
struct half {
	unsigned short core_mhz[6];
	unsigned int core_mw[6];
	unsigned short core_cc[6];
	unsigned short l3_mhz[2];
	unsigned short l3_cc[2];
	unsigned short c0_residency[6];
	unsigned short gfx_mhz;
	unsigned short gfx_cc;
	unsigned short soc_mhz, vclk_mhz, dclk_mhz, mem_mhz;
	unsigned int mv[2];         /* [0] VDDCR_VDD, [1] VDDCR_GFX */
	unsigned int ma[2];
	unsigned int mw[2];
	unsigned int socket_mw;
	unsigned short soc_cc;
	unsigned short edge_cc;
	unsigned short throttler;
	unsigned short spare;
};
struct table {
	struct half current, average;
	unsigned int start, stop, accnt;
};

/* Compile-time: a member of negative size stops the build, so a drifted offset never reaches a test run. */
#define SAME(name, cond) char name[(cond) ? 1 : -1]
struct layout_checks {
	SAME(half_size, sizeof(struct half) == BC250_SMU_METRICS_HALF);
	SAME(table_size, sizeof(struct table) == BC250_SMU_METRICS_BYTES);
	SAME(gfxclk, offsetof(struct half, gfx_mhz) == BC250_SMU_METRICS_OFF_GFXCLK);
	SAME(gfx_temp, offsetof(struct half, gfx_cc) == BC250_SMU_METRICS_OFF_GFX_TEMP);
	SAME(voltage, offsetof(struct half, mv) == BC250_SMU_METRICS_OFF_VOLTAGE);
	SAME(current, offsetof(struct half, ma) == BC250_SMU_METRICS_OFF_CURRENT);
	SAME(power, offsetof(struct half, mw) == BC250_SMU_METRICS_OFF_POWER);
	SAME(socket, offsetof(struct half, socket_mw) == BC250_SMU_METRICS_OFF_SOCKET);
	SAME(soc_temp, offsetof(struct half, soc_cc) == BC250_SMU_METRICS_OFF_SOC_TEMP);
	SAME(throttler, offsetof(struct half, throttler) == BC250_SMU_METRICS_OFF_THROTTLER);
	SAME(spare, offsetof(struct half, spare) == BC250_SMU_METRICS_OFF_SPARE);
	SAME(core_overflow, offsetof(struct half, c0_residency) + 6 * sizeof(unsigned short) == BC250_SMU_METRICS_OFF_GFXCLK);
	SAME(average, offsetof(struct table, average) == BC250_SMU_METRICS_HALF);
	SAME(accnt, offsetof(struct table, accnt) == BC250_SMU_METRICS_OFF_ACCNT);
	SAME(fits, BC250_SMU_METRICS_BYTES % 4u == 0 && BC250_SMU_METRICS_BYTES <= BC250_SMU_METRICS_PAGE);
	/* The numbers of smu_v11_8_ppsmc.h and of TABLE_SMU_METRICS. */
	SAME(addr, BC250_SMU_METRICS_MSG_ADDR_HIGH == 0x4u && BC250_SMU_METRICS_MSG_ADDR_LOW == 0x5u);
	SAME(transfer, BC250_SMU_METRICS_MSG_TRANSFER == 0x6u && BC250_SMU_METRICS_TABLE_ID == 6u);
};

/* The eight-core table of the patched community SMU firmware (bc250_smu_metrics.h), written out field by field in
 * our own names from the offsets of the facts study: every per-core array widened to 8 entries in place. */
struct half8 {
	unsigned short core_mhz[8];
	unsigned int core_mw[8];
	unsigned short core_cc[8];
	unsigned short l3_mhz[2];
	unsigned short l3_cc[2];
	unsigned short c0_residency[8];
	unsigned short gfx_mhz;
	unsigned short gfx_cc;
	unsigned short soc_mhz, vclk_mhz, dclk_mhz, mem_mhz;
	unsigned int mv[2];
	unsigned int ma[2];
	unsigned int mw[2];
	unsigned int socket_mw;
	unsigned short soc_cc;
	unsigned short edge_cc;
	unsigned short throttler;
	unsigned short spare;
};
struct table8 {
	struct half8 current, average;
	unsigned int start, stop, accnt;
};

struct layout8_checks {
	/* The study's numbers, as numbers. */
	SAME(study, offsetof(struct half8, core_mw) == 0x10 && offsetof(struct half8, core_cc) == 0x30 &&
	     offsetof(struct half8, l3_mhz) == 0x40 && offsetof(struct half8, l3_cc) == 0x44 &&
	     offsetof(struct half8, c0_residency) == 0x48 && offsetof(struct half8, gfx_mhz) == 0x58 &&
	     offsetof(struct half8, gfx_cc) == 0x5A && offsetof(struct half8, soc_mhz) == 0x5C &&
	     offsetof(struct half8, vclk_mhz) == 0x5E && offsetof(struct half8, dclk_mhz) == 0x60 &&
	     offsetof(struct half8, mem_mhz) == 0x62 && offsetof(struct half8, mv) == 0x64 &&
	     offsetof(struct half8, ma) == 0x6C && offsetof(struct half8, mw) == 0x74 &&
	     offsetof(struct half8, socket_mw) == 0x7C && offsetof(struct half8, soc_cc) == 0x80 &&
	     offsetof(struct half8, edge_cc) == 0x82 && offsetof(struct half8, throttler) == 0x84 &&
	     offsetof(struct half8, spare) == 0x86 && sizeof(struct half8) == 136 && sizeof(struct table8) == 0x11C);
	/* The driver's constants against the declaration. */
	SAME(half8_size, sizeof(struct half8) == BC250_SMU_METRICS_8C_HALF);
	SAME(table8_size, sizeof(struct table8) == BC250_SMU_METRICS_8C_BYTES);
	SAME(c0_8, offsetof(struct half8, c0_residency) == BC250_SMU_METRICS_8C_OFF_C0);
	SAME(gfxclk8, offsetof(struct half8, gfx_mhz) == BC250_SMU_METRICS_8C_OFF_GFXCLK);
	SAME(gfx_temp8, offsetof(struct half8, gfx_cc) == BC250_SMU_METRICS_8C_OFF_GFX_TEMP);
	SAME(voltage8, offsetof(struct half8, mv) == BC250_SMU_METRICS_8C_OFF_VOLTAGE);
	SAME(current8, offsetof(struct half8, ma) == BC250_SMU_METRICS_8C_OFF_CURRENT);
	SAME(power8, offsetof(struct half8, mw) == BC250_SMU_METRICS_8C_OFF_POWER);
	SAME(socket8, offsetof(struct half8, socket_mw) == BC250_SMU_METRICS_8C_OFF_SOCKET);
	SAME(soc_temp8, offsetof(struct half8, soc_cc) == BC250_SMU_METRICS_8C_OFF_SOC_TEMP);
	SAME(throttler8, offsetof(struct half8, throttler) == BC250_SMU_METRICS_8C_OFF_THROTTLER);
	SAME(spare8, offsetof(struct half8, spare) == BC250_SMU_METRICS_8C_OFF_SPARE);
	SAME(core_overflow8, offsetof(struct half8, c0_residency) + 8 * sizeof(unsigned short) ==
	     BC250_SMU_METRICS_8C_OFF_GFXCLK);
	SAME(accnt8, offsetof(struct table8, accnt) == BC250_SMU_METRICS_8C_OFF_ACCNT);
	/* The window the miniport poisons and copies: the larger table, whole dwords, inside the page. */
	SAME(copy, BC250_SMU_METRICS_COPY_BYTES == sizeof(struct table8) && BC250_SMU_METRICS_COPY_BYTES % 4u == 0 &&
	     BC250_SMU_METRICS_COPY_BYTES > BC250_SMU_METRICS_BYTES && BC250_SMU_METRICS_COPY_BYTES <= BC250_SMU_METRICS_PAGE);
	SAME(latch, BC250_SMU_METRICS_LATCH_TABLES == 3u);
	SAME(layouts, STOCK == 1 && EIGHT == 2 && BC250_SMU_METRICS_LAYOUT_AUTO == 0);
};

/* The copy the miniport hands the reader: the firmware's table and, after it, the poison it did not overwrite. */
static void stock_copy(unsigned char *raw, const struct table *t)
{
	memset(raw, BC250_SMU_METRICS_POISON, COPY);
	memcpy(raw, t, sizeof(*t));
}

static void eight_copy(unsigned char *raw, const struct table8 *t)
{
	memcpy(raw, t, sizeof(*t));
}

static void layout(void)
{
	struct table t;
	unsigned char raw[sizeof(t)];
	struct bc250_smu_metrics m;

	/* The same at run time, through the decode: each field written in the firmware's structure comes out as the
	 * driver's field of the same meaning. */
	memset(&t, 0, sizeof(t));
	t.current.gfx_mhz = 1111; t.current.gfx_cc = 2222; t.current.mv[0] = 333; t.current.mv[1] = 444;
	t.current.mw[0] = 5555; t.current.mw[1] = 6666; t.current.socket_mw = 77777; t.current.soc_cc = 3333;
	t.current.throttler = 0x0102; t.average.socket_mw = 88888;
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), STOCK, &m) == BC250_SMU_METRICS_PARSE_OK);
	CHECK(m.gfx_mhz == 1111 && m.gfx_cc == 2222 && m.soc_mv == 333 && m.gfx_mv == 444);
	CHECK(m.soc_mw == 5555 && m.gfx_mw == 6666 && m.socket_mw == 77777 && m.soc_cc == 3333);
	CHECK(m.throttler == 0x0102 && m.socket_avg_mw == 88888);
}

/* A table as the firmware writes it, in its own struct: 78 W on the package, 48 W on the GPU rail. */
static void good_table(struct table *t)
{
	memset(t, 0, sizeof(*t));
	t->current.gfx_mhz = 1500;
	t->current.gfx_cc = 6200;
	t->current.mv[0] = 900;
	t->current.mv[1] = 919;
	t->current.mw[0] = 21000;
	t->current.mw[1] = 48000;
	t->current.socket_mw = 78000;
	t->current.soc_cc = 6050;
	t->current.edge_cc = 6100;
	t->current.throttler = 0x0004;
	t->average = t->current;
	t->average.socket_mw = 77400;
	t->accnt = 12345;
}

/* An eight-core table: other GPU numbers than good_table, and live CPU fields in front of them, so a decode at the
 * stock offsets reads CPU data (offset 68 is L3Temperature[0], 55 C) and cannot pass for the right answer. */
static void good_table8(struct table8 *t)
{
	unsigned int i;
	memset(t, 0, sizeof(*t));
	for (i = 0; i < 8; i++) {
		t->current.core_mhz[i] = 3500;
		t->current.core_mw[i] = 4000 + i;
		t->current.core_cc[i] = 5800;
		t->current.c0_residency[i] = 40;
	}
	t->current.l3_mhz[0] = t->current.l3_mhz[1] = 3400;
	t->current.l3_cc[0] = t->current.l3_cc[1] = 5500;
	t->current.gfx_mhz = 1750;
	t->current.gfx_cc = 6400;
	t->current.soc_mhz = 1000;
	t->current.mem_mhz = 1750;
	t->current.mv[0] = 1100;
	t->current.mv[1] = 950;
	t->current.ma[0] = 20000;
	t->current.ma[1] = 60000;
	t->current.mw[0] = 33000;
	t->current.mw[1] = 61000;
	t->current.socket_mw = 112000;
	t->current.soc_cc = 6600;
	t->current.edge_cc = 6300;
	t->current.throttler = 0x0010;
	t->average = t->current;
	t->average.socket_mw = 109500;
	t->start = 1000;
	t->stop = 2000;
	t->accnt = 777;
}

/* ---- the message list and the page --------------------------------------------------------------------- */

static void messages(void)
{
	const unsigned long long mc = 0xF5FFFEC000ull;      /* unit A: VRAM end - 0x14000 (MC base 0xF400000000, 8 GB) */
	unsigned int m;

	CHECK(bc250_smu_metrics_message_allowed(BC250_SMU_METRICS_MSG_ADDR_HIGH, 0xF5u, mc));
	CHECK(bc250_smu_metrics_message_allowed(BC250_SMU_METRICS_MSG_ADDR_LOW, 0xFFFEC000u, mc));
	CHECK(bc250_smu_metrics_message_allowed(BC250_SMU_METRICS_MSG_TRANSFER, BC250_SMU_METRICS_TABLE_ID, mc));
	/* Any other argument: the firmware is never told an address that is not the page's, nor another table. */
	CHECK(!bc250_smu_metrics_message_allowed(BC250_SMU_METRICS_MSG_ADDR_HIGH, 0u, mc));
	CHECK(!bc250_smu_metrics_message_allowed(BC250_SMU_METRICS_MSG_ADDR_HIGH, 0xF4u, mc));
	CHECK(!bc250_smu_metrics_message_allowed(BC250_SMU_METRICS_MSG_ADDR_LOW, 0u, mc));
	CHECK(!bc250_smu_metrics_message_allowed(BC250_SMU_METRICS_MSG_ADDR_LOW, 0xFFFED000u, mc));
	CHECK(!bc250_smu_metrics_message_allowed(BC250_SMU_METRICS_MSG_TRANSFER, 0u, mc));
	CHECK(!bc250_smu_metrics_message_allowed(BC250_SMU_METRICS_MSG_TRANSFER, 6u | (1u << 16), mc));
	CHECK(!bc250_smu_metrics_message_allowed(BC250_SMU_METRICS_MSG_TRANSFER, 4u, mc));
	/* No address, or one that is not page-aligned: nothing at all. */
	CHECK(!bc250_smu_metrics_message_allowed(BC250_SMU_METRICS_MSG_TRANSFER, BC250_SMU_METRICS_TABLE_ID, 0));
	CHECK(!bc250_smu_metrics_message_allowed(BC250_SMU_METRICS_MSG_ADDR_HIGH, 0xF5u, mc + 8u));
	/* Every other message of queue 0, TransferTableDram2Smu (0x7) first: the only writer of SMU tables. */
	for (m = 0; m < 0x100u; m++)
		if (m < 0x4u || m > 0x6u)
			CHECK(!bc250_smu_metrics_message_allowed(m, 0u, mc) && !bc250_smu_metrics_message_allowed(m, 6u, mc));
}

static void page(void)
{
	const unsigned long long base = 0xF400000000ull, len = 0x200000000ull, end = base + len;

	CHECK(bc250_smu_metrics_table_allowed(end - 0x14000u, base, len));
	CHECK(bc250_smu_metrics_table_allowed(end - 0x200000u, base, len));            /* the lowest page of the window */
	CHECK(bc250_smu_metrics_table_allowed(end - BC250_SMU_METRICS_PAGE, base, len)); /* the highest */
	CHECK(!bc250_smu_metrics_table_allowed(end - 0x201000u, base, len));           /* under the window: Windows' */
	CHECK(!bc250_smu_metrics_table_allowed(end, base, len));                       /* past the carve-out */
	CHECK(!bc250_smu_metrics_table_allowed(end - 0x14000u + 0x800u, base, len));   /* not page-aligned */
	CHECK(!bc250_smu_metrics_table_allowed(0, base, len));
	CHECK(!bc250_smu_metrics_table_allowed(0x14000u, 0, 0x100000u));               /* a carve-out under 2 MB */
	CHECK(!bc250_smu_metrics_table_allowed(0xFFFFFFFFFFFFF000ull, 0xFFFFFFFFFFF00000ull, 0x200000u)); /* wraps */
}

/* ---- the decode ---------------------------------------------------------------------------------------- */

static void decode(void)
{
	struct table t;
	struct bc250_smu_metrics m;
	unsigned char raw[BC250_SMU_METRICS_BYTES];

	good_table(&t);
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), STOCK, &m) == BC250_SMU_METRICS_PARSE_OK);
	CHECK(m.socket_mw == 78000 && m.socket_avg_mw == 77400);
	CHECK(m.gfx_mw == 48000 && m.soc_mw == 21000 && m.gfx_mv == 919 && m.soc_mv == 900);
	CHECK(m.gfx_mhz == 1500 && m.gfx_cc == 6200 && m.soc_cc == 6050 && m.throttler == 0x0004);

	/* Short: refused, and the output is zeroed rather than left with the caller's garbage. */
	memset(&m, 0x5A, sizeof(m));
	CHECK(bc250_smu_metrics_parse(raw, BC250_SMU_METRICS_BYTES - 4u, STOCK, &m) == BC250_SMU_METRICS_PARSE_SHORT);
	CHECK(m.socket_mw == 0 && m.gfx_mhz == 0);
	CHECK(bc250_smu_metrics_parse(NULL, sizeof(raw), STOCK, &m) == BC250_SMU_METRICS_PARSE_SHORT);

	/* A page the firmware did not write: every byte still the poison. */
	memset(raw, BC250_SMU_METRICS_POISON, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), STOCK, &m) == BC250_SMU_METRICS_PARSE_UNWRITTEN && m.socket_mw == 0);
	/* One field left unwritten is enough to refuse the table. */
	good_table(&t);
	t.average.socket_mw = 0xFFFFFFFFu;
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), STOCK, &m) == BC250_SMU_METRICS_PARSE_UNWRITTEN);
	good_table(&t);
	t.current.soc_cc = 0xFFFFu;
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), STOCK, &m) == BC250_SMU_METRICS_PARSE_UNWRITTEN);

	/* Out of range: 300 W + 1 mW, GPU 100 mV over the bound, 151 C. The bounds themselves pass. */
	good_table(&t);
	t.current.socket_mw = BC250_SMU_METRICS_MAX_MW + 1u;
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), STOCK, &m) == BC250_SMU_METRICS_PARSE_RANGE);
	good_table(&t);
	t.current.mv[1] = BC250_SMU_METRICS_MAX_GFX_MV + 100u;
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), STOCK, &m) == BC250_SMU_METRICS_PARSE_RANGE);
	good_table(&t);
	t.current.gfx_cc = (unsigned short)(BC250_SMU_METRICS_MAX_CC + 100u);
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), STOCK, &m) == BC250_SMU_METRICS_PARSE_RANGE);
	good_table(&t);
	t.current.socket_mw = BC250_SMU_METRICS_MAX_MW;
	t.current.mv[1] = BC250_SMU_METRICS_MAX_GFX_MV;
	t.current.gfx_cc = (unsigned short)BC250_SMU_METRICS_MAX_CC;
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), STOCK, &m) == BC250_SMU_METRICS_PARSE_OK);
	/* 0 mW, current or average, is a table caught mid-write (Linux stress 2026-10-07): a skipped sample. */
	good_table(&t);
	t.current.socket_mw = 0;
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), STOCK, &m) == BC250_SMU_METRICS_PARSE_ZERO_POWER);
	good_table(&t);
	t.average.socket_mw = 0;
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), STOCK, &m) == BC250_SMU_METRICS_PARSE_ZERO_POWER);
	good_table(&t);
	t.current.socket_mw = 1u;
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), STOCK, &m) == BC250_SMU_METRICS_PARSE_OK && m.socket_mw == 1u);
}

/* Every published numeric field, the overflow signature, and diagnostic-only spare bytes. */
static void plausibility(void)
{
	struct table t;
	struct bc250_smu_metrics m;
	unsigned char raw[sizeof(t)];
	unsigned int i, half, core;
	const unsigned int offsets[] = {104, 220, 96, 100, 80, 84, 68, 70, 108, 112};
	const unsigned int widths[] = {4, 4, 4, 4, 4, 4, 2, 2, 2, 2};
	const unsigned int maxima[] = {300000, 300000, 300000, 300000, 1600, BC250_SMU_METRICS_MAX_GFX_MV,
				       BC250_SMU_METRICS_MAX_GFX_MHZ, 15000, 15000, 65534};
	for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
		unsigned int j, v;
		good_table(&t);
		memcpy(raw, &t, sizeof(raw));
		v = maxima[i];
		for (j = 0; j < widths[i]; j++) raw[offsets[i] + j] = (unsigned char)(v >> (8 * j));
		CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), STOCK, &m) == BC250_SMU_METRICS_PARSE_OK);
		v++;
		for (j = 0; j < widths[i]; j++) raw[offsets[i] + j] = (unsigned char)(v >> (8 * j));
		CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), STOCK, &m) != BC250_SMU_METRICS_PARSE_OK);
		CHECK(m.socket_mw == 0 && m.gfx_mhz == 0 && m.throttler == 0);
		memset(raw + offsets[i], 0xFF, widths[i]);
		CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), STOCK, &m) == BC250_SMU_METRICS_PARSE_UNWRITTEN);
	}
	good_table(&t);
	t.current.gfx_mhz = 500;
	CHECK(bc250_smu_metrics_parse((const unsigned char *)&t, sizeof(t), STOCK, &m) == BC250_SMU_METRICS_PARSE_OK);
	t.current.gfx_mhz = 499;
	CHECK(bc250_smu_metrics_parse((const unsigned char *)&t, sizeof(t), STOCK, &m) == BC250_SMU_METRICS_PARSE_CLOCK);
	/* The opt-in overclock tiers: the table at the tier ceiling, and 5% above it, still parse. Under a tier the
	 * reader must not stop, or the fan curve loses its input. One MHz over the margin is refused. */
	{
		struct oc_bounds {
			SAME(clock_margin, BC250_SMU_METRICS_MAX_GFX_MHZ ==
			     BC250_CLOCK_OC_CEILING_MHZ + BC250_CLOCK_OC_CEILING_MHZ / 20u);
			SAME(volt_margin, BC250_SMU_METRICS_MAX_GFX_MV == BC250_CLOCK_OC_CEILING_MV + 100u);
			SAME(tiers_in_ceiling, BC250_OC_AIR_GPU_MAX_MHZ <= BC250_CLOCK_OC_CEILING_MHZ &&
			     BC250_OC_AIO_GPU_MAX_MHZ <= BC250_CLOCK_OC_CEILING_MHZ);
		};
	}
	t.current.gfx_mhz = (unsigned short)BC250_OC_AIO_GPU_MAX_MHZ;
	t.current.mv[1] = (unsigned short)BC250_OC_AIO_GPU_MAX_MV;
	CHECK(bc250_smu_metrics_parse((const unsigned char *)&t, sizeof(t), STOCK, &m) == BC250_SMU_METRICS_PARSE_OK &&
	      m.gfx_mhz == BC250_OC_AIO_GPU_MAX_MHZ);
	t.current.gfx_mhz = (unsigned short)BC250_SMU_METRICS_MAX_GFX_MHZ;
	CHECK(bc250_smu_metrics_parse((const unsigned char *)&t, sizeof(t), STOCK, &m) == BC250_SMU_METRICS_PARSE_OK);
	t.current.gfx_mhz = (unsigned short)(BC250_SMU_METRICS_MAX_GFX_MHZ + 1u);
	CHECK(bc250_smu_metrics_parse((const unsigned char *)&t, sizeof(t), STOCK, &m) == BC250_SMU_METRICS_PARSE_CLOCK);
	good_table(&t);
	t.current.gfx_mhz = 0; /* gated clock alone is not a corrupted table */
	CHECK(bc250_smu_metrics_parse((const unsigned char *)&t, sizeof(t), STOCK, &m) == BC250_SMU_METRICS_PARSE_OK);
	t.current.gfx_cc = 8900; /* hot telemetry must not disappear at the thermal stop threshold */
	CHECK(bc250_smu_metrics_parse((const unsigned char *)&t, sizeof(t), STOCK, &m) == BC250_SMU_METRICS_PARSE_OK);

	good_table(&t);
	memcpy(raw, &t, sizeof(raw));
	/* Synthetic stock-6 layout with stores for cores 6 and 7 at EACH per-core array base. Byte writes avoid
	 * undefined C array overruns in the test. This models the report, not a captured field-board table. */
	for (half = 0; half < 2; half++) {
		unsigned int base = half * (unsigned int)sizeof(struct half);
		for (core = 6; core < 8; core++) {
			const unsigned int bases[] = {0, 12, 36, 56};
			const unsigned int strides[] = {2, 4, 2, 2};
			const unsigned int values[] = {3500, 5000, 6500, 100};
			for (i = 0; i < 4; i++) {
				unsigned int j, off = base + bases[i] + core * strides[i];
				for (j = 0; j < strides[i]; j++) raw[off + j] = (unsigned char)(values[i] >> (8 * j));
			}
		}
	}
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), STOCK, &m) == BC250_SMU_METRICS_PARSE_CORE_OVERFLOW);
	CHECK(m.socket_mw == 0 && m.gfx_mhz == 0);
	for (i = 0; i < 4; i++) {
		unsigned int off = 114 + (i / 2) * 116 + i % 2;
		raw[off] = 0x67;
		CHECK(bc250_smu_metrics_marker(raw, sizeof(raw), STOCK) == (1u << (i + 1)) - 1u);
		CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), STOCK, &m) == BC250_SMU_METRICS_PARSE_CORE_OVERFLOW);
	}
	/* A marker does not require refusing an otherwise valid stock table or trusting another table. */
	good_table(&t);
	t.current.spare = 0x67;
	CHECK(bc250_smu_metrics_marker((const unsigned char *)&t, sizeof(t), STOCK) == 1);
	CHECK(bc250_smu_metrics_parse((const unsigned char *)&t, sizeof(t), STOCK, &m) == BC250_SMU_METRICS_PARSE_OK);
	CHECK(bc250_smu_metrics_marker(NULL, sizeof(t), STOCK) == 0);
	CHECK(bc250_smu_metrics_marker((const unsigned char *)&t, 231, STOCK) == 0);
	t.current.spare = 0x42; /* unknown patch: no magic-based allow/deny decision */
	CHECK(bc250_smu_metrics_parse((const unsigned char *)&t, sizeof(t), STOCK, &m) == BC250_SMU_METRICS_PARSE_OK);
}

/* ---- the eight-core layout ----------------------------------------------------------------------------- */

static void eight_core(void)
{
	struct table8 t;
	struct table s;
	struct bc250_smu_metrics m;
	unsigned char raw[COPY];
	unsigned int i, half, core;
	const unsigned int offsets[] = {
		BC250_SMU_METRICS_8C_OFF_SOCKET, BC250_SMU_METRICS_8C_HALF + BC250_SMU_METRICS_8C_OFF_SOCKET,
		BC250_SMU_METRICS_8C_OFF_POWER, BC250_SMU_METRICS_8C_OFF_POWER + 4u,
		BC250_SMU_METRICS_8C_OFF_VOLTAGE, BC250_SMU_METRICS_8C_OFF_VOLTAGE + 4u,
		BC250_SMU_METRICS_8C_OFF_GFXCLK, BC250_SMU_METRICS_8C_OFF_GFX_TEMP,
		BC250_SMU_METRICS_8C_OFF_SOC_TEMP, BC250_SMU_METRICS_8C_OFF_THROTTLER };
	const unsigned int widths[] = {4, 4, 4, 4, 4, 4, 2, 2, 2, 2};
	const unsigned int maxima[] = {300000, 300000, 300000, 300000, 1600, BC250_SMU_METRICS_MAX_GFX_MV,
				       BC250_SMU_METRICS_MAX_GFX_MHZ, 15000, 15000, 65534};

	/* Each published field, one at a time, written through the declaration and read back by the decode. */
	good_table8(&t);
	eight_copy(raw, &t);
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), EIGHT, &m) == BC250_SMU_METRICS_PARSE_OK);
	CHECK(m.gfx_mhz == 1750 && m.gfx_cc == 6400 && m.soc_mv == 1100 && m.gfx_mv == 950);
	CHECK(m.soc_mw == 33000 && m.gfx_mw == 61000 && m.socket_mw == 112000 && m.socket_avg_mw == 109500);
	CHECK(m.soc_cc == 6600 && m.throttler == 0x0010);
	{
		struct field { unsigned int at, width, value; unsigned int (*get)(const struct bc250_smu_metrics *); };
		unsigned int k;
		good_table8(&t);
		t.current.gfx_mhz = 1234; t.current.gfx_cc = 4321; t.current.mv[0] = 1001; t.current.mv[1] = 902;
		t.current.mw[0] = 11111; t.current.mw[1] = 22222; t.current.socket_mw = 99999; t.average.socket_mw = 88888;
		t.current.soc_cc = 5151; t.current.throttler = 0xBEEF;
		eight_copy(raw, &t);
		CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), EIGHT, &m) == BC250_SMU_METRICS_PARSE_OK);
		CHECK(m.gfx_mhz == 1234 && m.gfx_cc == 4321 && m.soc_mv == 1001 && m.gfx_mv == 902);
		CHECK(m.soc_mw == 11111 && m.gfx_mw == 22222 && m.socket_mw == 99999 && m.socket_avg_mw == 88888);
		CHECK(m.soc_cc == 5151 && m.throttler == 0xBEEF);
		/* A change in a field the driver does not publish (the CPU arrays, Current[], the other clocks, edge, the
		 * counters, the average half except its socket power) changes nothing it publishes. */
		for (k = 0; k < sizeof(raw); k++) {
			struct bc250_smu_metrics n;
			int published = 0;
			for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
				if (k >= offsets[i] && k < offsets[i] + widths[i])
					published = 1;
			if (published)
				continue;
			eight_copy(raw, &t);
			raw[k] ^= 0x5Au;
			if (bc250_smu_metrics_parse(raw, sizeof(raw), EIGHT, &n) != BC250_SMU_METRICS_PARSE_OK ||
			    memcmp(&n, &m, sizeof(n)) != 0) {
				CHECK(!"an unpublished byte changed a published value");
				printf("  byte %u\n", k);
			}
		}
		checks++;
	}

	/* The bounds of every published field at its eight-core offset: the maximum passes, one more is refused, and
	 * the complete poison word is unwritten. */
	for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
		unsigned int j, v;
		good_table8(&t);
		eight_copy(raw, &t);
		v = maxima[i];
		for (j = 0; j < widths[i]; j++) raw[offsets[i] + j] = (unsigned char)(v >> (8 * j));
		CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), EIGHT, &m) == BC250_SMU_METRICS_PARSE_OK);
		v++;
		for (j = 0; j < widths[i]; j++) raw[offsets[i] + j] = (unsigned char)(v >> (8 * j));
		CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), EIGHT, &m) != BC250_SMU_METRICS_PARSE_OK);
		CHECK(m.socket_mw == 0 && m.gfx_mhz == 0 && m.throttler == 0);
		memset(raw + offsets[i], 0xFF, widths[i]);
		CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), EIGHT, &m) == BC250_SMU_METRICS_PARSE_UNWRITTEN);
	}
	/* Zero socket power in either half is the same skip as in the stock layout. */
	good_table8(&t);
	t.average.socket_mw = 0;
	eight_copy(raw, &t);
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), EIGHT, &m) == BC250_SMU_METRICS_PARSE_ZERO_POWER);

	/* Short for the eight-core table, though long enough for the stock one. */
	good_table8(&t);
	eight_copy(raw, &t);
	CHECK(bc250_smu_metrics_parse(raw, BC250_SMU_METRICS_BYTES, EIGHT, &m) == BC250_SMU_METRICS_PARSE_SHORT);
	CHECK(bc250_smu_metrics_parse(raw, COPY - 4u, EIGHT, &m) == BC250_SMU_METRICS_PARSE_SHORT);
	CHECK(bc250_smu_metrics_parse(NULL, COPY, EIGHT, &m) == BC250_SMU_METRICS_PARSE_SHORT);
	/* No layout, or an unknown one: refused before any byte is read. */
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), BC250_SMU_METRICS_LAYOUT_AUTO, &m) == BC250_SMU_METRICS_PARSE_LAYOUT);
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), 3u, &m) == BC250_SMU_METRICS_PARSE_LAYOUT && m.socket_mw == 0);

	/* The wrong decode of each table is refused, not read as plausible numbers. The stock decode of an
	 * eight-core table reads L3Temperature[0] (5500) as the GPU clock. The eight-core decode of a stock copy
	 * reads the average socket power from the poisoned tail. */
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), STOCK, &m) == BC250_SMU_METRICS_PARSE_CLOCK);
	good_table(&s);
	stock_copy(raw, &s);
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), EIGHT, &m) == BC250_SMU_METRICS_PARSE_UNWRITTEN);

	/* Spare and its marker at the eight-core offsets. */
	good_table8(&t);
	t.current.spare = 0x67;
	t.average.spare = 0x6700;
	eight_copy(raw, &t);
	CHECK(bc250_smu_metrics_marker(raw, sizeof(raw), EIGHT) == 0x9u);
	CHECK(bc250_smu_metrics_marker(raw, sizeof(raw), STOCK) == 0);
	CHECK(bc250_smu_metrics_marker(raw, BC250_SMU_METRICS_BYTES, EIGHT) == 0);
	CHECK(bc250_smu_metrics_marker(raw, sizeof(raw), BC250_SMU_METRICS_LAYOUT_AUTO) == 0);
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), EIGHT, &m) == BC250_SMU_METRICS_PARSE_OK);

	/* BD-148 under the eight-core layout. The same mechanism one array wider: stores for cores 8 and 9 past each
	 * eight-entry array put two percentages into GfxclkFrequency and GfxTemperature. Refused with reason 5. */
	good_table8(&t);
	eight_copy(raw, &t);
	for (half = 0; half < 2; half++) {
		unsigned int base = half * BC250_SMU_METRICS_8C_HALF;
		for (core = 8; core < 10; core++) {
			const unsigned int bases[] = {0x00, 0x10, 0x30, 0x48};
			const unsigned int strides[] = {2, 4, 2, 2};
			const unsigned int values[] = {3500, 5000, 6500, 100};
			for (i = 0; i < 4; i++) {
				unsigned int j, off = base + bases[i] + core * strides[i];
				for (j = 0; j < strides[i]; j++) raw[off + j] = (unsigned char)(values[i] >> (8 * j));
			}
		}
	}
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), EIGHT, &m) == BC250_SMU_METRICS_PARSE_CORE_OVERFLOW);
	CHECK(m.socket_mw == 0 && m.gfx_mhz == 0);
	/* A gated GPU clock alone, with a real temperature, is still a table. */
	good_table8(&t);
	t.current.gfx_mhz = 0;
	eight_copy(raw, &t);
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), EIGHT, &m) == BC250_SMU_METRICS_PARSE_OK);
}

/* The layout one copy shows: the stock tail untouched, or any one byte of it changed. */
static void detect(void)
{
	struct table s;
	struct table8 t;
	unsigned char raw[COPY];
	unsigned int i;

	good_table(&s);
	stock_copy(raw, &s);
	CHECK(bc250_smu_metrics_detect(raw, sizeof(raw)) == STOCK);
	/* The poison in the stock table itself changes nothing: only the tail is read. */
	memset(raw, BC250_SMU_METRICS_POISON, BC250_SMU_METRICS_BYTES);
	CHECK(bc250_smu_metrics_detect(raw, sizeof(raw)) == STOCK);
	for (i = BC250_SMU_METRICS_BYTES; i < COPY; i++) {
		stock_copy(raw, &s);
		raw[i] = 0xFE;
		if (bc250_smu_metrics_detect(raw, sizeof(raw)) != EIGHT) {
			CHECK(!"a changed tail byte was not detected");
			printf("  byte %u\n", i);
		}
	}
	checks++;
	good_table8(&t);
	eight_copy(raw, &t);
	CHECK(bc250_smu_metrics_detect(raw, sizeof(raw)) == EIGHT);
	/* No tail to read: unknown, whatever the bytes say. */
	CHECK(bc250_smu_metrics_detect(raw, COPY - 4u) == BC250_SMU_METRICS_LAYOUT_AUTO);
	CHECK(bc250_smu_metrics_detect(raw, BC250_SMU_METRICS_BYTES) == BC250_SMU_METRICS_LAYOUT_AUTO);
	CHECK(bc250_smu_metrics_detect(NULL, sizeof(raw)) == BC250_SMU_METRICS_LAYOUT_AUTO);
}

/* The latch: three good tables of one layout in a row, nothing published before it, never a second latch. */
static void latch(void)
{
	struct bc250_smu_metrics_reader r;
	struct table s;
	struct table8 t;
	unsigned char stock[COPY], eight[COPY], bad[COPY], zero[COPY], changed[COPY], overflow[COPY];
	struct bc250_smu_metrics m;
	unsigned long long now = 1000;
	unsigned int i;

	good_table(&s);
	stock_copy(stock, &s);
	good_table8(&t);
	eight_copy(eight, &t);
	memset(bad, BC250_SMU_METRICS_POISON, sizeof(bad));
	s.average.socket_mw = 0;
	stock_copy(zero, &s);
	good_table(&s);
	stock_copy(changed, &s);
	changed[COPY - 1u] = 0x00;              /* the stock table with one tail byte written */

	/* Unit A: the stock firmware. Two good tables are counted and not published; the third latches and is read. */
	bc250_smu_metrics_reader_init(&r, 1, 1, 0, BC250_SMU_METRICS_LAYOUT_AUTO);
	CHECK(r.layout == BC250_SMU_METRICS_LAYOUT_AUTO && !r.layout_forced);
	for (i = 1; i <= 2; i++) {
		CHECK(bc250_smu_metrics_record(&r, now, 0, stock, sizeof(stock)) == 0);
		CHECK(r.state == BC250_SMU_METRICS_STATE_WAITING && r.layout_run == i && r.layout_candidate == STOCK);
		CHECK(!r.has_last && r.reads == 0 && r.failures == 0 && r.last_parse == BC250_SMU_METRICS_PARSE_OK);
		CHECK(!bc250_smu_metrics_fresh(&r, now) && r.layout == BC250_SMU_METRICS_LAYOUT_AUTO);
		now += BC250_SMU_METRICS_PERIOD_MS;
	}
	CHECK(bc250_smu_metrics_record(&r, now, 0, stock, sizeof(stock)) == 0);
	CHECK(r.state == BC250_SMU_METRICS_STATE_OK && r.layout == STOCK && r.reads == 1 && r.last_ok_ms == now);
	CHECK(r.last.socket_mw == 78000 && r.last.gfx_mhz == 1500 && r.last_tail == STOCK);

	/* A tail written while the stock layout is latched: refused with reason 7, the last table kept, never a
	 * second latch. A good stock table in between ends the run of bad ones; three in a row stop the reader. */
	now += BC250_SMU_METRICS_PERIOD_MS;
	CHECK(bc250_smu_metrics_record(&r, now, 0, changed, sizeof(changed)) == 0);
	CHECK(r.last_parse == BC250_SMU_METRICS_PARSE_LAYOUT && r.last_reject == BC250_SMU_METRICS_PARSE_LAYOUT);
	CHECK(r.layout == STOCK && r.last_tail == EIGHT && r.bad_in_row == 1 && r.reads == 1 && r.last.socket_mw == 78000);
	now += BC250_SMU_METRICS_PERIOD_MS;
	CHECK(bc250_smu_metrics_record(&r, now, 0, stock, sizeof(stock)) == 0);
	CHECK(r.bad_in_row == 0 && r.reads == 2 && r.last_parse == BC250_SMU_METRICS_PARSE_OK);
	for (i = 0; i < 3; i++) {
		now += BC250_SMU_METRICS_PERIOD_MS;
		CHECK(bc250_smu_metrics_record(&r, now, 0, i == 1 ? eight : changed, COPY) == 0);
		CHECK(r.last_parse == BC250_SMU_METRICS_PARSE_LAYOUT && r.layout == STOCK);
	}
	CHECK(r.state == BC250_SMU_METRICS_STATE_BAD_TABLE && r.reads == 2 && r.last.socket_mw == 78000);

	/* The 8-core firmware: the tail is written, three tables latch the eight-core decode and the third is read
	 * with its offsets. A stock-looking copy after that is refused the same way. */
	bc250_smu_metrics_reader_init(&r, 1, 1, 0, BC250_SMU_METRICS_LAYOUT_AUTO);
	for (i = 0; i < 3; i++) {
		CHECK(bc250_smu_metrics_record(&r, now, 0, eight, sizeof(eight)) == 0);
		now += BC250_SMU_METRICS_PERIOD_MS;
	}
	CHECK(r.state == BC250_SMU_METRICS_STATE_OK && r.layout == EIGHT && r.reads == 1);
	CHECK(r.last.gfx_mhz == 1750 && r.last.gfx_cc == 6400 && r.last.socket_mw == 112000 && r.last.socket_avg_mw == 109500);
	CHECK(r.last.gfx_mv == 950 && r.last.soc_mv == 1100 && r.last.gfx_mw == 61000 && r.last.soc_mw == 33000);
	CHECK(r.last.soc_cc == 6600 && r.last.throttler == 0x0010);
	CHECK(bc250_smu_metrics_record(&r, now, 0, stock, sizeof(stock)) == 0);
	CHECK(r.last_parse == BC250_SMU_METRICS_PARSE_LAYOUT && r.layout == EIGHT && r.last_tail == STOCK && r.bad_in_row == 1);

	/* The run: a table of the other layout starts it again at 1, and any refused table ends it, a zero-power one
	 * included (which still does not count toward the bad-table limit). */
	bc250_smu_metrics_reader_init(&r, 1, 1, 0, BC250_SMU_METRICS_LAYOUT_AUTO);
	CHECK(bc250_smu_metrics_record(&r, now, 0, stock, COPY) == 0 && r.layout_run == 1);
	CHECK(bc250_smu_metrics_record(&r, now, 0, stock, COPY) == 0 && r.layout_run == 2);
	CHECK(bc250_smu_metrics_record(&r, now, 0, eight, COPY) == 0 && r.layout_run == 1 && r.layout_candidate == EIGHT);
	CHECK(bc250_smu_metrics_record(&r, now, 0, eight, COPY) == 0 && r.layout_run == 2);
	CHECK(bc250_smu_metrics_record(&r, now, 0, zero, COPY) == 0 && r.layout_run == 0 && r.bad_in_row == 0);
	CHECK(r.last_parse == BC250_SMU_METRICS_PARSE_ZERO_POWER && r.failures == 1);
	CHECK(bc250_smu_metrics_record(&r, now, 0, eight, COPY) == 0 && r.layout_run == 1);
	CHECK(bc250_smu_metrics_record(&r, now, 0, bad, COPY) == 0 && r.layout_run == 0 && r.bad_in_row == 1);
	CHECK(bc250_smu_metrics_record(&r, now, 0, eight, COPY) == 0 && r.layout_run == 1 && r.bad_in_row == 0);
	CHECK(bc250_smu_metrics_record(&r, now, 0, eight, COPY) == 0 && r.layout_run == 2);
	CHECK(r.state == BC250_SMU_METRICS_STATE_WAITING && !r.has_last && r.layout == BC250_SMU_METRICS_LAYOUT_AUTO);
	CHECK(bc250_smu_metrics_record(&r, now, 0, eight, COPY) == 0);
	CHECK(r.state == BC250_SMU_METRICS_STATE_OK && r.layout == EIGHT && r.reads == 1);
	/* A short copy has no tail: refused as short before or after the latch, and never counted toward a run. */
	CHECK(bc250_smu_metrics_record(&r, now, 0, eight, BC250_SMU_METRICS_BYTES) == 0);
	CHECK(r.last_parse == BC250_SMU_METRICS_PARSE_SHORT && r.last_tail == BC250_SMU_METRICS_LAYOUT_AUTO);
	bc250_smu_metrics_reader_init(&r, 1, 1, 0, BC250_SMU_METRICS_LAYOUT_AUTO);
	CHECK(bc250_smu_metrics_record(&r, now, 0, stock, COPY - 4u) == 0);
	CHECK(r.last_parse == BC250_SMU_METRICS_PARSE_SHORT && r.layout_run == 0 && r.bad_in_row == 1);

	/* BD-148 under the stock layout: an unlocked board with the stock SMU firmware writes 244 bytes, so the tail
	 * stays poisoned, the layout detected is stock, and the overwrite pattern is refused with reason 5 at every
	 * table. The reader never latches and stops after three. The eight-core layout never rescues such a copy. */
	good_table(&s);
	stock_copy(overflow, &s);
	for (i = 0; i < 2; i++) {
		unsigned int core;
		for (core = 6; core < 8; core++) {
			const unsigned int bases[] = {0, 12, 36, 56};
			const unsigned int strides[] = {2, 4, 2, 2};
			const unsigned int values[] = {3500, 5000, 6500, 100};
			unsigned int a;
			for (a = 0; a < 4; a++) {
				unsigned int j, off = i * BC250_SMU_METRICS_HALF + bases[a] + core * strides[a];
				for (j = 0; j < strides[a]; j++) overflow[off + j] = (unsigned char)(values[a] >> (8 * j));
			}
		}
	}
	CHECK(bc250_smu_metrics_detect(overflow, COPY) == STOCK);
	CHECK(bc250_smu_metrics_parse(overflow, COPY, STOCK, &m) == BC250_SMU_METRICS_PARSE_CORE_OVERFLOW);
	CHECK(bc250_smu_metrics_parse(overflow, COPY, EIGHT, &m) == BC250_SMU_METRICS_PARSE_UNWRITTEN);
	bc250_smu_metrics_reader_init(&r, 1, 1, 0, BC250_SMU_METRICS_LAYOUT_AUTO);
	for (i = 0; i < 3; i++) {
		CHECK(bc250_smu_metrics_record(&r, now, 0, overflow, COPY) == 0);
		CHECK(r.last_parse == BC250_SMU_METRICS_PARSE_CORE_OVERFLOW && r.layout_run == 0);
		now += BC250_SMU_METRICS_PERIOD_MS;
	}
	CHECK(r.state == BC250_SMU_METRICS_STATE_BAD_TABLE && r.layout == BC250_SMU_METRICS_LAYOUT_AUTO && !r.has_last);
	/* The same copy with the eight-core layout forced: refused too (the average lies in the poisoned tail). */
	bc250_smu_metrics_reader_init(&r, 1, 1, 0, EIGHT);
	for (i = 0; i < 3; i++)
		CHECK(bc250_smu_metrics_record(&r, now, 0, overflow, COPY) == 0 &&
		      r.last_parse == BC250_SMU_METRICS_PARSE_UNWRITTEN);
	CHECK(r.state == BC250_SMU_METRICS_STATE_BAD_TABLE && !r.has_last);

	/* SmuMetricsLayout. 1 or 2 latch at once, without the tail check: the first good table is read. */
	bc250_smu_metrics_reader_init(&r, 1, 1, 0, STOCK);
	CHECK(r.layout == STOCK && r.layout_forced);
	CHECK(bc250_smu_metrics_record(&r, now, 0, stock, COPY) == 0 && r.state == BC250_SMU_METRICS_STATE_OK && r.reads == 1);
	CHECK(bc250_smu_metrics_record(&r, now, 0, changed, COPY) == 0 && r.last_parse == BC250_SMU_METRICS_PARSE_OK);
	CHECK(r.last_tail == EIGHT && r.reads == 2);
	/* Stock forced on the eight-core firmware: the decode reads CPU data and is refused. */
	CHECK(bc250_smu_metrics_record(&r, now, 0, eight, COPY) == 0 && r.last_parse == BC250_SMU_METRICS_PARSE_CLOCK);
	bc250_smu_metrics_reader_init(&r, 1, 1, 0, EIGHT);
	CHECK(bc250_smu_metrics_record(&r, now, 0, eight, COPY) == 0 && r.reads == 1 && r.last.socket_mw == 112000);
	CHECK(bc250_smu_metrics_record(&r, now, 0, stock, COPY) == 0 && r.last_parse == BC250_SMU_METRICS_PARSE_UNWRITTEN);
	/* Any other value detects. */
	for (i = 3; i < 6; i++) {
		bc250_smu_metrics_reader_init(&r, 1, 1, 0, i == 5 ? 0xFFFFFFFFu : i);
		CHECK(r.layout == BC250_SMU_METRICS_LAYOUT_AUTO && !r.layout_forced);
	}
	/* The gate still wins: a forced layout on an off, refused or pageless reader is never due. */
	bc250_smu_metrics_reader_init(&r, 0, 1, 0, EIGHT);
	CHECK(r.state == BC250_SMU_METRICS_STATE_OFF && !bc250_smu_metrics_due(&r, now));
	bc250_smu_metrics_reader_init(&r, 1, 1, 1, EIGHT);
	CHECK(r.state == BC250_SMU_METRICS_STATE_REFUSED && !bc250_smu_metrics_due(&r, now));
}

/* ---- the reader ---------------------------------------------------------------------------------------- */

static void reader(void)
{
	struct bc250_smu_metrics_reader r;
	struct table t;
	unsigned char raw[COPY], bad[COPY];
	unsigned long long now = 5000;

	good_table(&t);
	stock_copy(raw, &t);
	memset(bad, BC250_SMU_METRICS_POISON, sizeof(bad));

	/* The cadence, freshness, refusal and bad-table rules do not depend on the layout: this reader has the stock
	 * layout forced (SmuMetricsLayout 1), so every good table is read at once. latch() covers detection. */
	/* The gate and the start states: off, refused earlier in this boot, no page; none of them is ever due. */
	bc250_smu_metrics_reader_init(&r, 0, 1, 0, STOCK);
	CHECK(r.state == BC250_SMU_METRICS_STATE_OFF && !bc250_smu_metrics_due(&r, now));
	CHECK(bc250_smu_metrics_record(&r, now, 0, raw, sizeof(raw)) == 0 && r.state == BC250_SMU_METRICS_STATE_OFF && !r.reads);
	bc250_smu_metrics_reader_init(&r, 0, 1, 1, STOCK);
	CHECK(r.state == BC250_SMU_METRICS_STATE_OFF);                  /* the switch wins over the latch */
	bc250_smu_metrics_reader_init(&r, 1, 1, 1, STOCK);
	CHECK(r.state == BC250_SMU_METRICS_STATE_REFUSED && !bc250_smu_metrics_due(&r, now));
	bc250_smu_metrics_reader_init(&r, 1, 0, 0, STOCK);
	CHECK(r.state == BC250_SMU_METRICS_STATE_NO_TABLE && !bc250_smu_metrics_due(&r, now));

	/* A good start: waiting, then one table, then due again only a period later. */
	bc250_smu_metrics_reader_init(&r, 1, 1, 0, STOCK);
	CHECK(r.state == BC250_SMU_METRICS_STATE_WAITING && bc250_smu_metrics_due(&r, now));
	CHECK(!bc250_smu_metrics_fresh(&r, now) && bc250_smu_metrics_age_ms(&r, now) == 0);
	CHECK(bc250_smu_metrics_record(&r, now, 0, raw, sizeof(raw)) == 0);
	CHECK(r.state == BC250_SMU_METRICS_STATE_OK && r.reads == 1 && r.failures == 0 && r.has_last);
	CHECK(r.last.socket_mw == 78000 && r.last_ok_ms == now);
	CHECK(!bc250_smu_metrics_due(&r, now + BC250_SMU_METRICS_PERIOD_MS - 1u));
	CHECK(bc250_smu_metrics_due(&r, now + BC250_SMU_METRICS_PERIOD_MS));
	/* Freshness: three periods, not one more millisecond. */
	CHECK(bc250_smu_metrics_fresh(&r, now + BC250_SMU_METRICS_FRESH_MS));
	CHECK(!bc250_smu_metrics_fresh(&r, now + BC250_SMU_METRICS_FRESH_MS + 1u));
	CHECK(bc250_smu_metrics_age_ms(&r, now + 640u) == 640u);
	CHECK(bc250_smu_metrics_age_ms(&r, now - 1u) == 0);              /* a clock that went back: no negative age */
	CHECK(bc250_smu_metrics_age_ms(&r, now + 0x200000000ull) == 0xFFFFFFFFu);

	/* Offline (a power transition): nothing sent, nothing counted, the next period tries again. */
	now += BC250_SMU_METRICS_PERIOD_MS;
	CHECK(bc250_smu_metrics_record(&r, now, BC250_SMU_METRICS_OFFLINE, NULL, 0) == 0);
	CHECK(r.state == BC250_SMU_METRICS_STATE_OK && r.failures == 0 && r.reads == 1);
	CHECK(!bc250_smu_metrics_due(&r, now) && bc250_smu_metrics_due(&r, now + BC250_SMU_METRICS_PERIOD_MS));

	/* Two bad tables: counted, the last good table stays, the reader goes on. A good table resets the run. */
	now += BC250_SMU_METRICS_PERIOD_MS;
	CHECK(bc250_smu_metrics_record(&r, now, 0, bad, sizeof(bad)) == 0);
	now += BC250_SMU_METRICS_PERIOD_MS;
	CHECK(bc250_smu_metrics_record(&r, now, 0, bad, sizeof(bad)) == 0);
	CHECK(r.state == BC250_SMU_METRICS_STATE_OK && r.failures == 2 && r.bad_in_row == 2);
	CHECK(r.last_parse == BC250_SMU_METRICS_PARSE_UNWRITTEN && r.last.socket_mw == 78000);
	CHECK(!bc250_smu_metrics_fresh(&r, now + BC250_SMU_METRICS_FRESH_MS));  /* the good table is getting old */
	now += BC250_SMU_METRICS_PERIOD_MS;
	CHECK(bc250_smu_metrics_record(&r, now, 0, raw, sizeof(raw)) == 0);
	CHECK(r.bad_in_row == 0 && r.reads == 2 && r.last_parse == BC250_SMU_METRICS_PARSE_OK && r.last_ok_ms == now);

	/* Zero-power tables (caught mid-write): counted, never toward the bad-table limit, the last reading stays. Five
	 * in a row do not stop the reader; the reading ages out by freshness instead. */
	{
		struct table z;
		unsigned char zraw[COPY];
		unsigned int i, before = r.failures;
		good_table(&z);
		z.average.socket_mw = 0;
		stock_copy(zraw, &z);
		for (i = 0; i < 5u; i++) {
			now += BC250_SMU_METRICS_PERIOD_MS;
			CHECK(bc250_smu_metrics_record(&r, now, 0, zraw, sizeof(zraw)) == 0);
		}
		CHECK(r.state == BC250_SMU_METRICS_STATE_OK && r.bad_in_row == 0 && r.failures == before + 5u && r.reads == 2);
		CHECK(r.last_parse == BC250_SMU_METRICS_PARSE_ZERO_POWER && r.last.socket_mw == 78000);
		CHECK(!bc250_smu_metrics_fresh(&r, now) && bc250_smu_metrics_due(&r, now + BC250_SMU_METRICS_PERIOD_MS));
		now += BC250_SMU_METRICS_PERIOD_MS;
		CHECK(bc250_smu_metrics_record(&r, now, 0, raw, sizeof(raw)) == 0 && r.reads == 3 && bc250_smu_metrics_fresh(&r, now));
	}

	/* Three bad tables in a row: stopped for this start, but not latched for the boot (return 0). */
	now += BC250_SMU_METRICS_PERIOD_MS;
	CHECK(bc250_smu_metrics_record(&r, now, 0, bad, sizeof(bad)) == 0);
	now += BC250_SMU_METRICS_PERIOD_MS;
	CHECK(bc250_smu_metrics_record(&r, now, 0, bad, 8u) == 0);                 /* short counts as bad too */
	now += BC250_SMU_METRICS_PERIOD_MS;
	CHECK(bc250_smu_metrics_record(&r, now, 0, bad, sizeof(bad)) == 0);
	CHECK(r.state == BC250_SMU_METRICS_STATE_BAD_TABLE && !bc250_smu_metrics_due(&r, now + 100000u));
	CHECK(!bc250_smu_metrics_fresh(&r, now));
	CHECK(bc250_smu_metrics_record(&r, now + 100000u, 0, raw, sizeof(raw)) == 0 && r.state == BC250_SMU_METRICS_STATE_BAD_TABLE);

	/* A refusal: the firmware's answer (0xFE, unknown or not allowed), a timeout (-62) or the allowlist (-22). Each one
	 * ends the reader and asks the miniport to latch it for the boot (return 1). A reading it had is no longer fresh. */
	{
		const int refusals[] = { 0xFE, -62, -22, -1 };
		unsigned int i;
		for (i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++) {
			bc250_smu_metrics_reader_init(&r, 1, 1, 0, STOCK);
			CHECK(bc250_smu_metrics_record(&r, now, 0, raw, sizeof(raw)) == 0 && r.state == BC250_SMU_METRICS_STATE_OK);
			CHECK(bc250_smu_metrics_record(&r, now + 1000u, refusals[i], NULL, 0) == 1);
			CHECK(r.state == BC250_SMU_METRICS_STATE_REFUSED && r.failures == 1 && r.last_status == refusals[i]);
			CHECK(!bc250_smu_metrics_due(&r, now + 100000u) && !bc250_smu_metrics_fresh(&r, now + 1000u));
			/* Nothing after it changes anything, and nothing asks for a second latch. */
			CHECK(bc250_smu_metrics_record(&r, now + 2000u, 0, raw, sizeof(raw)) == 0);
			CHECK(r.state == BC250_SMU_METRICS_STATE_REFUSED && r.reads == 1);
		}
	}
	/* The first read refused: no table ever, still latched. */
	bc250_smu_metrics_reader_init(&r, 1, 1, 0, STOCK);
	CHECK(bc250_smu_metrics_record(&r, now, 0xFE, NULL, 0) == 1 && !r.has_last && !bc250_smu_metrics_fresh(&r, now));
}

int main(void)
{
	layout();
	messages();
	page();
	decode();
	plausibility();
	eight_core();
	detect();
	latch();
	reader();
	printf("SMU metrics policy: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
