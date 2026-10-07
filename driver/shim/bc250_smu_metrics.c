/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The SMU metrics table: the allowlist of its three messages, the decode of one table and the reader's rules.
 * Contract and sources: include/bc250_smu_metrics.h. Pure: no OS call, so driver/shim/test/smu_metrics_test.c
 * drives exactly what the miniport drives (driver/shim/test/run_smu_metrics.ps1).
 */
#include <string.h>
#include "bc250_smu_metrics.h"

#define TOP_WINDOW 0x200000ull  /* the carve-out's top 2 MB: bc250kmd.h BC250_VRAM_GART_BELOW */

int bc250_smu_metrics_message_allowed(unsigned int message, unsigned int parameter, unsigned long long table_mc)
{
	if (table_mc == 0 || (table_mc & (BC250_SMU_METRICS_PAGE - 1u)) != 0)
		return 0;
	switch (message) {
	case BC250_SMU_METRICS_MSG_ADDR_HIGH:
		return parameter == (unsigned int)(table_mc >> 32);
	case BC250_SMU_METRICS_MSG_ADDR_LOW:
		return parameter == (unsigned int)(table_mc & 0xFFFFFFFFull);
	case BC250_SMU_METRICS_MSG_TRANSFER:
		return parameter == BC250_SMU_METRICS_TABLE_ID;
	default:
		return 0;
	}
}

int bc250_smu_metrics_table_allowed(unsigned long long table_mc, unsigned long long vram_mc,
				    unsigned long long vram_len)
{
	unsigned long long end;
	if (table_mc == 0 || (table_mc & (BC250_SMU_METRICS_PAGE - 1u)) != 0)
		return 0;
	if (vram_len < TOP_WINDOW || vram_mc + vram_len < vram_mc)
		return 0;
	end = vram_mc + vram_len;
	if (table_mc < end - TOP_WINDOW || table_mc > end - BC250_SMU_METRICS_PAGE)
		return 0;
	return 1;
}

static unsigned int u16_at(const unsigned char *raw, unsigned int offset)
{
	return (unsigned int)raw[offset] | (unsigned int)raw[offset + 1] << 8;
}

static unsigned int u32_at(const unsigned char *raw, unsigned int offset)
{
	return u16_at(raw, offset) | u16_at(raw, offset + 2) << 16;
}

enum bc250_smu_metrics_parse bc250_smu_metrics_parse(const unsigned char *raw, unsigned int length,
						     struct bc250_smu_metrics *out)
{
	struct bc250_smu_metrics m;
	memset(out, 0, sizeof(*out));
	if (raw == NULL || length < BC250_SMU_METRICS_BYTES)
		return BC250_SMU_METRICS_PARSE_SHORT;
	m.socket_mw = u32_at(raw, BC250_SMU_METRICS_OFF_SOCKET);
	m.socket_avg_mw = u32_at(raw, BC250_SMU_METRICS_HALF + BC250_SMU_METRICS_OFF_SOCKET);
	m.soc_mw = u32_at(raw, BC250_SMU_METRICS_OFF_POWER);
	m.gfx_mw = u32_at(raw, BC250_SMU_METRICS_OFF_POWER + 4u);
	m.soc_mv = u32_at(raw, BC250_SMU_METRICS_OFF_VOLTAGE);
	m.gfx_mv = u32_at(raw, BC250_SMU_METRICS_OFF_VOLTAGE + 4u);
	m.gfx_mhz = u16_at(raw, BC250_SMU_METRICS_OFF_GFXCLK);
	m.gfx_cc = u16_at(raw, BC250_SMU_METRICS_OFF_GFX_TEMP);
	m.soc_cc = u16_at(raw, BC250_SMU_METRICS_OFF_SOC_TEMP);
	m.throttler = u16_at(raw, BC250_SMU_METRICS_OFF_THROTTLER);
	/* The fields every table carries. A whole field of poison is a page the firmware did not write; one poison
	 * byte inside a real value is impossible for these widths and bounds, so whole fields are enough. */
	if (m.socket_mw == 0xFFFFFFFFu || m.socket_avg_mw == 0xFFFFFFFFu || m.gfx_mv == 0xFFFFFFFFu ||
	    m.soc_mv == 0xFFFFFFFFu || m.gfx_mhz == 0xFFFFu || m.gfx_cc == 0xFFFFu || m.soc_cc == 0xFFFFu)
		return BC250_SMU_METRICS_PARSE_UNWRITTEN;
	if (m.socket_mw > BC250_SMU_METRICS_MAX_MW || m.socket_avg_mw > BC250_SMU_METRICS_MAX_MW ||
	    m.gfx_mw > BC250_SMU_METRICS_MAX_MW || m.soc_mw > BC250_SMU_METRICS_MAX_MW ||
	    m.gfx_mv > BC250_SMU_METRICS_MAX_MV || m.soc_mv > BC250_SMU_METRICS_MAX_MV ||
	    m.gfx_cc > BC250_SMU_METRICS_MAX_CC || m.soc_cc > BC250_SMU_METRICS_MAX_CC)
		return BC250_SMU_METRICS_PARSE_RANGE;
	/* The package never draws 0 W while the SMU answers (58 W idle on unit A's Linux). A Linux stress run of
	 * 2026-10-07 (4 parallel readers, 316 000 tables in 90 s) read 0 for the average in about one sample of
	 * three, and never with one reader. A zero is therefore a table caught while the firmware wrote it: the
	 * reader skips it and keeps the previous reading, which then ages out by the freshness rule. */
	if (m.socket_mw == 0u || m.socket_avg_mw == 0u)
		return BC250_SMU_METRICS_PARSE_ZERO_POWER;
	*out = m;
	return BC250_SMU_METRICS_PARSE_OK;
}

void bc250_smu_metrics_reader_init(struct bc250_smu_metrics_reader *r, int enabled, int table_ok,
				   int refused_this_boot)
{
	memset(r, 0, sizeof(*r));
	if (!enabled)
		r->state = BC250_SMU_METRICS_STATE_OFF;
	else if (refused_this_boot)
		r->state = BC250_SMU_METRICS_STATE_REFUSED;
	else if (!table_ok)
		r->state = BC250_SMU_METRICS_STATE_NO_TABLE;
	else
		r->state = BC250_SMU_METRICS_STATE_WAITING;
}

int bc250_smu_metrics_due(const struct bc250_smu_metrics_reader *r, unsigned long long now_ms)
{
	if (r->state != BC250_SMU_METRICS_STATE_WAITING && r->state != BC250_SMU_METRICS_STATE_OK)
		return 0;
	return now_ms >= r->next_ms;
}

int bc250_smu_metrics_record(struct bc250_smu_metrics_reader *r, unsigned long long now_ms, int status,
			     const unsigned char *raw, unsigned int length)
{
	struct bc250_smu_metrics m;
	enum bc250_smu_metrics_parse parse;
	if (r->state != BC250_SMU_METRICS_STATE_WAITING && r->state != BC250_SMU_METRICS_STATE_OK)
		return 0;
	r->next_ms = now_ms + BC250_SMU_METRICS_PERIOD_MS;
	r->last_status = status;
	if (status == BC250_SMU_METRICS_OFFLINE)
		return 0;       /* nothing was sent: a power transition or a stop is under way */
	if (status != 0) {
		/* The firmware refused, did not answer within the transport's bound, or the allowlist said no. None of
		 * them gets better by asking again, and a timeout costs the governor its bound every time. */
		r->failures++;
		r->state = BC250_SMU_METRICS_STATE_REFUSED;
		return 1;
	}
	parse = bc250_smu_metrics_parse(raw, length, &m);
	r->last_parse = (unsigned int)parse;
	if (parse == BC250_SMU_METRICS_PARSE_ZERO_POWER) {
		r->failures++;  /* counted, but not toward the bad-table limit: the next table is usually whole */
		return 0;
	}
	if (parse != BC250_SMU_METRICS_PARSE_OK) {
		r->failures++;
		if (++r->bad_in_row >= BC250_SMU_METRICS_BAD_LIMIT)
			r->state = BC250_SMU_METRICS_STATE_BAD_TABLE;
		return 0;
	}
	r->bad_in_row = 0;
	r->reads++;
	r->last = m;
	r->has_last = 1;
	r->last_ok_ms = now_ms;
	r->state = BC250_SMU_METRICS_STATE_OK;
	return 0;
}

int bc250_smu_metrics_fresh(const struct bc250_smu_metrics_reader *r, unsigned long long now_ms)
{
	return r->state == BC250_SMU_METRICS_STATE_OK && r->has_last && now_ms >= r->last_ok_ms &&
	       now_ms - r->last_ok_ms <= BC250_SMU_METRICS_FRESH_MS;
}

unsigned int bc250_smu_metrics_age_ms(const struct bc250_smu_metrics_reader *r, unsigned long long now_ms)
{
	unsigned long long age;
	if (!r->has_last || now_ms < r->last_ok_ms)
		return 0;
	age = now_ms - r->last_ok_ms;
	return age > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned int)age;
}
