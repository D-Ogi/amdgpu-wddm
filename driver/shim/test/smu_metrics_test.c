/* The SMU metrics table policy (driver/shim/bc250_smu_metrics.c, KMD 0.7.215), driven on the host.
 *
 * What this test is for: the driver asks the SMU to write one table into one page, then believes a few numbers in
 * it. So the things that must not drift are the offsets of those numbers, the three messages and the one argument
 * each may carry, the page the driver may name, the decode's refusals, and the reader's rules: one read per
 * period, a refusal ends the reads for the boot, an owner that is offline costs nothing, three bad tables in a
 * row end the reads for the start, and a reading older than three periods is no reading.
 *
 *   pwsh driver\shim\test\run_smu_metrics.ps1
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "bc250_smu_metrics.h"

static int checks, failures;

#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

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
	SAME(average, offsetof(struct table, average) == BC250_SMU_METRICS_HALF);
	SAME(accnt, offsetof(struct table, accnt) == BC250_SMU_METRICS_OFF_ACCNT);
	SAME(fits, BC250_SMU_METRICS_BYTES % 4u == 0 && BC250_SMU_METRICS_BYTES <= BC250_SMU_METRICS_PAGE);
	/* The numbers of smu_v11_8_ppsmc.h and of TABLE_SMU_METRICS. */
	SAME(addr, BC250_SMU_METRICS_MSG_ADDR_HIGH == 0x4u && BC250_SMU_METRICS_MSG_ADDR_LOW == 0x5u);
	SAME(transfer, BC250_SMU_METRICS_MSG_TRANSFER == 0x6u && BC250_SMU_METRICS_TABLE_ID == 6u);
};

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
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), &m) == BC250_SMU_METRICS_PARSE_OK);
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
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), &m) == BC250_SMU_METRICS_PARSE_OK);
	CHECK(m.socket_mw == 78000 && m.socket_avg_mw == 77400);
	CHECK(m.gfx_mw == 48000 && m.soc_mw == 21000 && m.gfx_mv == 919 && m.soc_mv == 900);
	CHECK(m.gfx_mhz == 1500 && m.gfx_cc == 6200 && m.soc_cc == 6050 && m.throttler == 0x0004);

	/* Short: refused, and the output is zeroed rather than left with the caller's garbage. */
	memset(&m, 0x5A, sizeof(m));
	CHECK(bc250_smu_metrics_parse(raw, BC250_SMU_METRICS_BYTES - 4u, &m) == BC250_SMU_METRICS_PARSE_SHORT);
	CHECK(m.socket_mw == 0 && m.gfx_mhz == 0);
	CHECK(bc250_smu_metrics_parse(NULL, sizeof(raw), &m) == BC250_SMU_METRICS_PARSE_SHORT);

	/* A page the firmware did not write: every byte still the poison. */
	memset(raw, BC250_SMU_METRICS_POISON, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), &m) == BC250_SMU_METRICS_PARSE_UNWRITTEN && m.socket_mw == 0);
	/* One field left unwritten is enough to refuse the table. */
	good_table(&t);
	t.average.socket_mw = 0xFFFFFFFFu;
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), &m) == BC250_SMU_METRICS_PARSE_UNWRITTEN);
	good_table(&t);
	t.current.soc_cc = 0xFFFFu;
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), &m) == BC250_SMU_METRICS_PARSE_UNWRITTEN);

	/* Out of range: 401 W, 2.1 V, 151 C. The bounds themselves pass. */
	good_table(&t);
	t.current.socket_mw = BC250_SMU_METRICS_MAX_MW + 1u;
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), &m) == BC250_SMU_METRICS_PARSE_RANGE);
	good_table(&t);
	t.current.mv[1] = BC250_SMU_METRICS_MAX_MV + 100u;
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), &m) == BC250_SMU_METRICS_PARSE_RANGE);
	good_table(&t);
	t.current.gfx_cc = (unsigned short)(BC250_SMU_METRICS_MAX_CC + 100u);
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), &m) == BC250_SMU_METRICS_PARSE_RANGE);
	good_table(&t);
	t.current.socket_mw = BC250_SMU_METRICS_MAX_MW;
	t.current.mv[1] = BC250_SMU_METRICS_MAX_MV;
	t.current.gfx_cc = (unsigned short)BC250_SMU_METRICS_MAX_CC;
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), &m) == BC250_SMU_METRICS_PARSE_OK);
	/* 0 mW, current or average, is a table caught mid-write (Linux stress 2026-10-07): a skipped sample. */
	good_table(&t);
	t.current.socket_mw = 0;
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), &m) == BC250_SMU_METRICS_PARSE_ZERO_POWER);
	good_table(&t);
	t.average.socket_mw = 0;
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), &m) == BC250_SMU_METRICS_PARSE_ZERO_POWER);
	good_table(&t);
	t.current.socket_mw = 1u;
	memcpy(raw, &t, sizeof(raw));
	CHECK(bc250_smu_metrics_parse(raw, sizeof(raw), &m) == BC250_SMU_METRICS_PARSE_OK && m.socket_mw == 1u);
}

/* ---- the reader ---------------------------------------------------------------------------------------- */

static void reader(void)
{
	struct bc250_smu_metrics_reader r;
	struct table t;
	unsigned char raw[BC250_SMU_METRICS_BYTES], bad[BC250_SMU_METRICS_BYTES];
	unsigned long long now = 5000;

	good_table(&t);
	memcpy(raw, &t, sizeof(raw));
	memset(bad, BC250_SMU_METRICS_POISON, sizeof(bad));

	/* The gate and the start states: off, refused earlier in this boot, no page; none of them is ever due. */
	bc250_smu_metrics_reader_init(&r, 0, 1, 0);
	CHECK(r.state == BC250_SMU_METRICS_STATE_OFF && !bc250_smu_metrics_due(&r, now));
	CHECK(bc250_smu_metrics_record(&r, now, 0, raw, sizeof(raw)) == 0 && r.state == BC250_SMU_METRICS_STATE_OFF && !r.reads);
	bc250_smu_metrics_reader_init(&r, 0, 1, 1);
	CHECK(r.state == BC250_SMU_METRICS_STATE_OFF);                  /* the switch wins over the latch */
	bc250_smu_metrics_reader_init(&r, 1, 1, 1);
	CHECK(r.state == BC250_SMU_METRICS_STATE_REFUSED && !bc250_smu_metrics_due(&r, now));
	bc250_smu_metrics_reader_init(&r, 1, 0, 0);
	CHECK(r.state == BC250_SMU_METRICS_STATE_NO_TABLE && !bc250_smu_metrics_due(&r, now));

	/* A good start: waiting, then one table, then due again only a period later. */
	bc250_smu_metrics_reader_init(&r, 1, 1, 0);
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
		unsigned char zraw[BC250_SMU_METRICS_BYTES];
		unsigned int i, before = r.failures;
		good_table(&z);
		z.average.socket_mw = 0;
		memcpy(zraw, &z, sizeof(zraw));
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
			bc250_smu_metrics_reader_init(&r, 1, 1, 0);
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
	bc250_smu_metrics_reader_init(&r, 1, 1, 0);
	CHECK(bc250_smu_metrics_record(&r, now, 0xFE, NULL, 0) == 1 && !r.has_last && !bc250_smu_metrics_fresh(&r, now));
}

int main(void)
{
	layout();
	messages();
	page();
	decode();
	reader();
	printf("SMU metrics policy: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
