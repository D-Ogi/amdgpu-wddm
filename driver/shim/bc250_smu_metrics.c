/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The SMU metrics table: the allowlist of its three messages, the decode of one table in either layout, the layout
 * detection and latch, and the reader's rules.
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

/* The offsets one decode needs. Index: enum bc250_smu_metrics_layout. */
struct layout {
	unsigned int bytes, half, gfxclk, gfx_temp, voltage, power, socket, soc_temp, throttler, spare;
};

static const struct layout layouts[BC250_SMU_METRICS_LAYOUT_COUNT] = {
	{ 0 },
	{ BC250_SMU_METRICS_BYTES, BC250_SMU_METRICS_HALF, BC250_SMU_METRICS_OFF_GFXCLK,
	  BC250_SMU_METRICS_OFF_GFX_TEMP, BC250_SMU_METRICS_OFF_VOLTAGE, BC250_SMU_METRICS_OFF_POWER,
	  BC250_SMU_METRICS_OFF_SOCKET, BC250_SMU_METRICS_OFF_SOC_TEMP, BC250_SMU_METRICS_OFF_THROTTLER,
	  BC250_SMU_METRICS_OFF_SPARE },
	{ BC250_SMU_METRICS_8C_BYTES, BC250_SMU_METRICS_8C_HALF, BC250_SMU_METRICS_8C_OFF_GFXCLK,
	  BC250_SMU_METRICS_8C_OFF_GFX_TEMP, BC250_SMU_METRICS_8C_OFF_VOLTAGE, BC250_SMU_METRICS_8C_OFF_POWER,
	  BC250_SMU_METRICS_8C_OFF_SOCKET, BC250_SMU_METRICS_8C_OFF_SOC_TEMP, BC250_SMU_METRICS_8C_OFF_THROTTLER,
	  BC250_SMU_METRICS_8C_OFF_SPARE },
};

static const struct layout *layout_of(unsigned int layout)
{
	if (layout != BC250_SMU_METRICS_LAYOUT_STOCK && layout != BC250_SMU_METRICS_LAYOUT_8CORE)
		return NULL;
	return &layouts[layout];
}

unsigned int bc250_smu_metrics_detect(const unsigned char *raw, unsigned int length)
{
	unsigned int i;
	if (raw == NULL || length < BC250_SMU_METRICS_COPY_BYTES)
		return BC250_SMU_METRICS_LAYOUT_AUTO;
	for (i = BC250_SMU_METRICS_BYTES; i < BC250_SMU_METRICS_COPY_BYTES; i++)
		if (raw[i] != BC250_SMU_METRICS_POISON)
			return BC250_SMU_METRICS_LAYOUT_8CORE;
	return BC250_SMU_METRICS_LAYOUT_STOCK;
}

unsigned int bc250_smu_metrics_marker(const unsigned char *raw, unsigned int length, unsigned int layout)
{
	const struct layout *l = layout_of(layout);
	unsigned int i, mask = 0;
	if (raw == NULL || l == NULL || length < l->bytes)
		return 0;
	for (i = 0; i < 4u; i++) {
		unsigned int offset = l->spare + (i / 2u) * l->half + i % 2u;
		if (raw[offset] == 0x67u)
			mask |= 1u << i;
	}
	return mask;
}

enum bc250_smu_metrics_parse bc250_smu_metrics_parse(const unsigned char *raw, unsigned int length,
						     unsigned int layout, struct bc250_smu_metrics *out)
{
	const struct layout *l = layout_of(layout);
	struct bc250_smu_metrics m;
	memset(out, 0, sizeof(*out));
	if (l == NULL)
		return BC250_SMU_METRICS_PARSE_LAYOUT;
	if (raw == NULL || length < l->bytes)
		return BC250_SMU_METRICS_PARSE_SHORT;
	m.socket_mw = u32_at(raw, l->socket);
	m.socket_avg_mw = u32_at(raw, l->half + l->socket);
	m.soc_mw = u32_at(raw, l->power);
	m.gfx_mw = u32_at(raw, l->power + 4u);
	m.soc_mv = u32_at(raw, l->voltage);
	m.gfx_mv = u32_at(raw, l->voltage + 4u);
	m.gfx_mhz = u16_at(raw, l->gfxclk);
	m.gfx_cc = u16_at(raw, l->gfx_temp);
	m.soc_cc = u16_at(raw, l->soc_temp);
	m.throttler = u16_at(raw, l->throttler);
	/* A complete poison field is not a measurement. Include every published field, including the opaque
	 * throttler word. A single 0xFF byte can occur in a valid value, so do not reject individual bytes. */
	if (m.socket_mw == 0xFFFFFFFFu || m.socket_avg_mw == 0xFFFFFFFFu || m.gfx_mv == 0xFFFFFFFFu ||
	    m.soc_mv == 0xFFFFFFFFu || m.gfx_mw == 0xFFFFFFFFu || m.soc_mw == 0xFFFFFFFFu ||
	    m.gfx_mhz == 0xFFFFu || m.gfx_cc == 0xFFFFu || m.soc_cc == 0xFFFFu || m.throttler == 0xFFFFu)
		return BC250_SMU_METRICS_PARSE_UNWRITTEN;
	/* BD-148: C0Residency starts at byte 56. Two writes past its six uint16 entries land at 68/70, exactly
	 * GfxclkFrequency/GfxTemperature. Two percentages there imply <=100 MHz AND <=1 C. This is a refusal
	 * signature, not proof of which firmware wrote it. Neither a marker nor CPU core count bypasses it. The
	 * eight-core layout has the same order (C0Residency[8] ends at its GfxclkFrequency), so the same check
	 * applies to it: no valid table of either layout has a GPU at <=100 MHz and <=1 C. */
	if (m.gfx_mhz <= BC250_SMU_METRICS_MAX_RESIDENCY && m.gfx_cc <= BC250_SMU_METRICS_MAX_RESIDENCY)
		return BC250_SMU_METRICS_PARSE_CORE_OVERFLOW;
	if (m.gfx_mhz != 0u && (m.gfx_mhz < BC250_SMU_METRICS_MIN_GFX_MHZ ||
	    m.gfx_mhz > BC250_SMU_METRICS_MAX_GFX_MHZ))
		return BC250_SMU_METRICS_PARSE_CLOCK;
	if (m.socket_mw > BC250_SMU_METRICS_MAX_MW || m.socket_avg_mw > BC250_SMU_METRICS_MAX_MW ||
	    m.gfx_mw > BC250_SMU_METRICS_MAX_MW || m.soc_mw > BC250_SMU_METRICS_MAX_MW ||
	    m.gfx_mv > BC250_SMU_METRICS_MAX_GFX_MV || m.soc_mv > BC250_SMU_METRICS_MAX_SOC_MV ||
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
				   int refused_this_boot, unsigned int layout_setting)
{
	memset(r, 0, sizeof(*r));
	if (layout_of(layout_setting) != NULL) {
		r->layout = layout_setting;
		r->layout_forced = 1;
	}
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
	unsigned int layout;
	const struct layout *l;
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
	/* The whole window or nothing: without the tail the layout is unknown, and a forced layout reads it too. */
	memset(&m, 0, sizeof(m));
	r->last_tail = bc250_smu_metrics_detect(raw, length);
	layout = r->layout != BC250_SMU_METRICS_LAYOUT_AUTO ? r->layout : r->last_tail;
	if (r->last_tail == BC250_SMU_METRICS_LAYOUT_AUTO)
		parse = BC250_SMU_METRICS_PARSE_SHORT;
	else if (!r->layout_forced && r->layout != BC250_SMU_METRICS_LAYOUT_AUTO && r->last_tail != r->layout)
		parse = BC250_SMU_METRICS_PARSE_LAYOUT;
	else
		parse = bc250_smu_metrics_parse(raw, length, layout, &m);
	r->last_parse = (unsigned int)parse;
	r->marker_mask = bc250_smu_metrics_marker(raw, length, layout);
	l = layout_of(layout);
	r->spare_bytes = raw != NULL && l != NULL && length >= l->bytes ?
		u16_at(raw, l->spare) | (u16_at(raw, l->half + l->spare) << 16) : 0u;
	if (parse != BC250_SMU_METRICS_PARSE_OK) {
		if (r->rejected_tables != 0xFFFFFFFFu)
			r->rejected_tables++;
		r->last_reject = (unsigned int)parse;
		r->layout_run = 0;
	}
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
	if (r->layout == BC250_SMU_METRICS_LAYOUT_AUTO) {
		if (r->layout_run != 0 && r->layout_candidate == layout) {
			r->layout_run++;
		} else {
			r->layout_candidate = layout;
			r->layout_run = 1;
		}
		if (r->layout_run < BC250_SMU_METRICS_LATCH_TABLES)
			return 0;       /* a good table, not published: the layout is not latched yet */
		r->layout = layout;
	}
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
