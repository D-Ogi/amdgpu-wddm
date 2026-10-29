/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The SMU metrics table of Cyan Skillfish (KMD 0.7.215). Pure policy: no OS call, no lock, no time source of its
 * own. The SMU owner (driver/kmd/smu.c) sends the messages and copies the page; the miniport
 * (driver/kmd/smu_metrics.c) owns the page, the gate, the cadence and the published snapshot. The design is in
 * bc250-win docs/design/dpm.md, section "Power reading".
 *
 * What the firmware does. The driver gives the SMU the GPU (MC) address of one page with
 * SetDriverTableDramAddrHigh and SetDriverTableDramAddrLow. After that, TransferTableSmu2Dram with the table id
 * as its argument makes the SMU write that table into the page and answer OK. Nothing else is written there.
 *
 * Sources, facts only, nothing copied:
 *   - message numbers: driver/amdgpu-import/smu_v11_8_ppsmc.h (AMD, MIT, Linux v6.18): 0x4, 0x5 and 0x6.
 *   - the call order and the arguments: Linux v6.18 drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 *     smu_v11_0_set_driver_table_location (High gets upper_32_bits(mc), Low gets lower_32_bits(mc)), and
 *     swsmu/smu_cmn.c smu_cmn_update_table (TransferTableSmu2Dram with table_id | argument << 16, argument 0).
 *   - the table id and the layout: Linux v6.18 swsmu/inc/pmfw_if/smu11_driver_if_cyan_skillfish.h (AMD, MIT):
 *     TABLE_SMU_METRICS 6, SmuMetrics_t = { SmuMetricsTable_t Current, Average; uint32 SampleStartTime,
 *     SampleStopTime, Accnt }, MP1_DRIVER_IF_VERSION 0x8. The offsets below are that structure with natural
 *     alignment, which is how amdgpu's own copy of it is compiled.
 *   - the units: the same header (mW, mV, mA, MHz, centi-Celsius), and swsmu/smu11/cyan_skillfish_ppt.c, which
 *     reads Current.CurrentSocketPower for hwmon power1_input and Average.CurrentSocketPower for power1_average.
 *   - on unit A: amdgpu sent exactly these three messages during its init, High 0xF4 and Low 0x8CF000, then
 *     TransferTableSmu2Dram (docs/init-sequence.md, experiment E03), and under load TransferTableSmu2Dram with
 *     argument 6 alone, 61 times in 20 s, with hwmon power1_input at 58-62 W (facts M90, experiment E21).
 *   - a third-party manual (cachenetics/project-ariel, no log) says queue 0 message 0x4 hangs the SMU until AC is
 *     removed. amdgpu sends it at every Linux start of unit A with the address of its own table (E03 above), so
 *     the list below admits it with the driver's own page address and nothing else
 *     (docs/design/rejected-options.md). EnableSmuMetrics 0 sends none of the three.
 */
#ifndef BC250_SMU_METRICS_H
#define BC250_SMU_METRICS_H

#define BC250_SMU_METRICS_MSG_ADDR_HIGH 0x4u    /* PPSMC_MSG_SetDriverTableDramAddrHigh */
#define BC250_SMU_METRICS_MSG_ADDR_LOW 0x5u     /* PPSMC_MSG_SetDriverTableDramAddrLow */
#define BC250_SMU_METRICS_MSG_TRANSFER 0x6u     /* PPSMC_MSG_TransferTableSmu2Dram */
#define BC250_SMU_METRICS_TABLE_ID 6u           /* TABLE_SMU_METRICS; the argument of the transfer, upper half 0 */
#define BC250_SMU_METRICS_PAGE 4096u            /* the driver's page; the table must fit in it */

/* SmuMetrics_t, byte offsets. SmuMetricsTable_t is 116 bytes; Average follows Current. */
#define BC250_SMU_METRICS_HALF 116u             /* sizeof(SmuMetricsTable_t) */
#define BC250_SMU_METRICS_BYTES 244u            /* sizeof(SmuMetrics_t) */
#define BC250_SMU_METRICS_OFF_GFXCLK 68u        /* uint16 GfxclkFrequency, MHz */
#define BC250_SMU_METRICS_OFF_GFX_TEMP 70u      /* uint16 GfxTemperature, centi-Celsius (amdgpu: edge) */
#define BC250_SMU_METRICS_OFF_VOLTAGE 80u       /* uint32 Voltage[2], mV: [0] VDDCR_VDD, [1] VDDCR_GFX */
#define BC250_SMU_METRICS_OFF_CURRENT 88u       /* uint32 Current[2], mA, same indices */
#define BC250_SMU_METRICS_OFF_POWER 96u         /* uint32 Power[2], mW, same indices */
#define BC250_SMU_METRICS_OFF_SOCKET 104u       /* uint32 CurrentSocketPower, mW: the whole package */
#define BC250_SMU_METRICS_OFF_SOC_TEMP 108u     /* uint16 SocTemperature, centi-Celsius (amdgpu: hotspot) */
#define BC250_SMU_METRICS_OFF_THROTTLER 112u    /* uint16 ThrottlerStatus */
#define BC250_SMU_METRICS_OFF_SPARE 114u        /* uint16 Spare, diagnostic only */
#define BC250_SMU_METRICS_OFF_ACCNT 240u        /* uint32 Accnt, after SampleStartTime and SampleStopTime */

/* The eight-core layout. The patched SMU firmware of the community 8-core BIOS (bc250-smu-unlock "metrics-8core")
 * widens every per-core array of SmuMetricsTable_t from 6 to 8 entries IN PLACE, so every field after them moves.
 * Source, facts only: the community CachyOS BC-250 kernel (GPL; study in the workspace, no code copied). Its
 * export DMA is 0x11C bytes. Byte offsets in one half, natural alignment:
 *   CoreFrequency[8] 0x00, CorePower[8] 0x10, CoreTemperature[8] 0x30, L3Frequency[2] 0x40, L3Temperature[2] 0x44,
 *   C0Residency[8] 0x48, GfxclkFrequency 0x58, GfxTemperature 0x5A, Socclk 0x5C, Vclk 0x5E, Dclk 0x60,
 *   Memclk 0x62, Voltage[2] 0x64, Current[2] 0x6C, Power[2] 0x74, CurrentSocketPower 0x7C, SocTemperature 0x80,
 *   EdgeTemperature 0x82, ThrottlerStatus 0x84, Spare 0x86. Half 0x88 bytes; Average follows Current, then the
 *   same three uint32 counters. With the stock decode, offset 68 of such a table is L3Temperature[0]. */
#define BC250_SMU_METRICS_8C_HALF 136u
#define BC250_SMU_METRICS_8C_BYTES 284u         /* 0x11C, the firmware's DMA length */
#define BC250_SMU_METRICS_8C_OFF_C0 72u         /* uint16 C0Residency[8] */
#define BC250_SMU_METRICS_8C_OFF_GFXCLK 88u
#define BC250_SMU_METRICS_8C_OFF_GFX_TEMP 90u
#define BC250_SMU_METRICS_8C_OFF_VOLTAGE 100u
#define BC250_SMU_METRICS_8C_OFF_CURRENT 108u
#define BC250_SMU_METRICS_8C_OFF_POWER 116u
#define BC250_SMU_METRICS_8C_OFF_SOCKET 124u
#define BC250_SMU_METRICS_8C_OFF_SOC_TEMP 128u
#define BC250_SMU_METRICS_8C_OFF_THROTTLER 132u
#define BC250_SMU_METRICS_8C_OFF_SPARE 134u
#define BC250_SMU_METRICS_8C_OFF_ACCNT 280u

/* The bytes the miniport poisons, transfers and copies: the larger of the two tables. The firmware writes only its
 * own table. Bytes BC250_SMU_METRICS_BYTES .. BC250_SMU_METRICS_COPY_BYTES - 1 (the tail) therefore keep the poison
 * under the stock firmware, and a changed byte there means the firmware's DMA is longer than the stock table. The
 * layout is detected from the DMA length, never from the CPU core count: an unlocked board with the stock SMU
 * firmware writes the stock 244 bytes (BD-148) and stays on the stock decode and its overflow refusal. */
#define BC250_SMU_METRICS_COPY_BYTES BC250_SMU_METRICS_8C_BYTES
/* Consistent tables in a row before the reader latches a detected layout. No table is accepted before the latch. */
#define BC250_SMU_METRICS_LATCH_TABLES 3u

/* The layouts. The values are also those of the diagnostic REG_DWORD SmuMetricsLayout (driver/kmd/smu_metrics.c):
 * 0 detects, 1 and 2 force a decode. Any other value of the setting detects. */
enum bc250_smu_metrics_layout {
	BC250_SMU_METRICS_LAYOUT_AUTO = 0,      /* not latched yet; as a setting: detect */
	BC250_SMU_METRICS_LAYOUT_STOCK = 1,     /* 244 bytes, six cores (interface 0x8) */
	BC250_SMU_METRICS_LAYOUT_8CORE = 2,     /* 284 bytes, eight cores, in place */
	BC250_SMU_METRICS_LAYOUT_COUNT
};

/* The byte the owner fills the page with before every transfer. A field that still holds it afterwards was not
 * written by the firmware, so the table is refused instead of read as 4294967 W. */
#define BC250_SMU_METRICS_POISON 0xFFu

/* Acceptance bounds, not control limits. The board has a 300 W PSU. The GPU uses 500..2000 MHz and at most
 * 1000 mV (docs/hardware.md), and an opt-in overclock tier admits up to 2300 MHz and 1075 mV
 * (BC250_CLOCK_OC_CEILING_MHZ/_MV in bc250_board_envelope.h; smu_metrics_test.c checks the relation). Allow 5%
 * clock and 100 mV readback margin over that ceiling, not higher setpoints. A lower bound here stops the
 * reader after BC250_SMU_METRICS_BAD_LIMIT tables at a tier clock, and the fan curve then loses its input. The CPU rail's
 * readback envelope is at most 1600 mV there. Zero voltage/clock is allowed for a gated rail. Temperature must
 * remain visible ABOVE the thermal trip: 150 C is a decode ceiling, never permission to operate that hot. */
#define BC250_SMU_METRICS_MAX_MW 300000u
#define BC250_SMU_METRICS_MAX_GFX_MV 1175u
#define BC250_SMU_METRICS_MAX_SOC_MV 1600u
#define BC250_SMU_METRICS_MIN_GFX_MHZ 500u
#define BC250_SMU_METRICS_MAX_GFX_MHZ 2415u
#define BC250_SMU_METRICS_MAX_CC 15000u
#define BC250_SMU_METRICS_MAX_RESIDENCY 100u /* C0Residency is percentage, upstream interface 0x8 */

/* Cadence and freshness. One table per second at most, read by the DPM governor thread, the same cadence as its
 * clock readback (BC250_DPM_VERIFY_MS). A reading older than three periods is "no reading". */
#define BC250_SMU_METRICS_PERIOD_MS 1000u
#define BC250_SMU_METRICS_FRESH_MS (3u * BC250_SMU_METRICS_PERIOD_MS)
/* Tables in a row the firmware answered OK for and that did not pass the checks above. After this many the
 * reader stops for the rest of the start: the page or the layout is wrong, and reading it again proves nothing. */
#define BC250_SMU_METRICS_BAD_LIMIT 3u

/* Transport results the reader tells apart. 0 is success; this value means the owner was not online and no
 * message went out (smu.c OwnerBegin), so the next period tries again. Every other non-zero value is a refusal:
 * a firmware answer other than OK, a timeout, or the allowlist below. */
#define BC250_SMU_METRICS_OFFLINE (-19)

/* The state the escape publishes (BC250_DPM_METRICS_* in driver/kmd/bc250kmd_escape.h has the same numbers). */
enum bc250_smu_metrics_state {
	BC250_SMU_METRICS_STATE_OFF = 0,        /* EnableSmuMetrics 0 */
	BC250_SMU_METRICS_STATE_WAITING = 1,    /* on, and no table accepted yet in this start */
	BC250_SMU_METRICS_STATE_OK = 2,         /* the last table was accepted */
	BC250_SMU_METRICS_STATE_REFUSED = 3,    /* the firmware refused or did not answer: no more reads this boot */
	BC250_SMU_METRICS_STATE_NO_TABLE = 4,   /* no page (VRAM closed, mapping failed) or no SMU owner at start */
	BC250_SMU_METRICS_STATE_BAD_TABLE = 5,  /* BC250_SMU_METRICS_BAD_LIMIT bad tables in a row: stopped */
	BC250_SMU_METRICS_STATE_COUNT
};

/* Why one table was not accepted. */
enum bc250_smu_metrics_parse {
	BC250_SMU_METRICS_PARSE_OK = 0,
	BC250_SMU_METRICS_PARSE_SHORT = 1,      /* fewer bytes than the layout's table, or the reader's copy */
	BC250_SMU_METRICS_PARSE_UNWRITTEN = 2,  /* the poison is still in a field the firmware always writes */
	BC250_SMU_METRICS_PARSE_RANGE = 3,      /* a value outside the bounds above */
	BC250_SMU_METRICS_PARSE_ZERO_POWER = 4, /* the current or average socket power reads 0 mW: a skipped sample */
	BC250_SMU_METRICS_PARSE_CORE_OVERFLOW = 5, /* clock AND temperature look like C0Residency entries */
	BC250_SMU_METRICS_PARSE_CLOCK = 6,     /* nonzero GPU clock outside the readback envelope */
	BC250_SMU_METRICS_PARSE_LAYOUT = 7     /* no known layout, or the tail disagrees with the latched layout */
};

/* One accepted table, in the units of the header. */
struct bc250_smu_metrics {
	unsigned int socket_mw;         /* Current.CurrentSocketPower: the package, CPU and GPU together */
	unsigned int socket_avg_mw;     /* Average.CurrentSocketPower */
	unsigned int gfx_mw, soc_mw;    /* Current.Power[1] (VDDCR_GFX) and Current.Power[0] (VDDCR_VDD) */
	unsigned int gfx_mv, soc_mv;    /* Current.Voltage[1] and [0] */
	unsigned int gfx_mhz;           /* Current.GfxclkFrequency: equal to GetGfxFrequency when the layout is right */
	unsigned int gfx_cc, soc_cc;    /* Current.GfxTemperature and SocTemperature, centi-Celsius */
	unsigned int throttler;         /* Current.ThrottlerStatus */
};

/* The reader. The governor thread alone touches it; the miniport copies what it publishes. */
struct bc250_smu_metrics_reader {
	unsigned int state;             /* enum bc250_smu_metrics_state */
	unsigned int reads;             /* tables accepted in this start */
	unsigned int failures;          /* reads that ended without a table: refusals, bad tables */
	unsigned int bad_in_row;
	int last_status;                /* the transport's last result, 0 for success */
	unsigned int last_parse;        /* enum bc250_smu_metrics_parse of the last table the firmware wrote */
	unsigned int rejected_tables;   /* saturating count of parse refusals, no transport failures */
	unsigned int last_reject;       /* last non-OK parse, retained across successful reads */
	unsigned int marker_mask;       /* 0x67 seen in Spare bytes 114,115,230,231: bits 0..3; diagnostic only */
	unsigned int spare_bytes;       /* raw Spare bytes of both halves, packed from least to most significant */
	unsigned int layout;            /* enum bc250_smu_metrics_layout: the latched decode, AUTO until the latch */
	unsigned int layout_forced;     /* 1: SmuMetricsLayout chose the layout; the tail check is off */
	unsigned int layout_candidate;  /* the detected layout of the current run of good tables */
	unsigned int layout_run;        /* good tables in a row with that layout, toward BC250_SMU_METRICS_LATCH_TABLES */
	unsigned int last_tail;         /* enum bc250_smu_metrics_layout detected in the last copy, AUTO for a short one */
	unsigned long long next_ms;     /* no read before this time */
	unsigned long long last_ok_ms;  /* the time of the last accepted table */
	int has_last;
	struct bc250_smu_metrics last;  /* the last accepted table */
};

/* The three messages of this path and the one argument each may carry, for the driver's own page at table_mc.
 * table_mc must be non-zero and page-aligned. High takes table_mc >> 32, Low takes its low 32 bits, the transfer
 * takes BC250_SMU_METRICS_TABLE_ID. Every other message or argument: 0. The clock list (bc250_clock.h) admits
 * none of the three, and this list admits none of the clock messages. */
int bc250_smu_metrics_message_allowed(unsigned int message, unsigned int parameter, unsigned long long table_mc);
/* The page the driver may name: page-aligned, a whole page inside the carve-out [vram_mc, vram_mc + vram_len),
 * and in its top 2 MB, the part of the carve-out that Windows' memory segment never covers (driver/kmd/wddm.c
 * keeps BC250_VRAM_TOP_RESERVED, which is larger, out of the segment). */
int bc250_smu_metrics_table_allowed(unsigned long long table_mc, unsigned long long vram_mc,
				    unsigned long long vram_len);
/* Decodes one table in the given layout (STOCK or 8CORE; any other value is PARSE_LAYOUT). On
 * BC250_SMU_METRICS_PARSE_OK *out holds it; on anything else *out is zeroed. */
enum bc250_smu_metrics_parse bc250_smu_metrics_parse(const unsigned char *raw, unsigned int length,
						     unsigned int layout, struct bc250_smu_metrics *out);
/* The layout one copy shows: STOCK when all of the tail still holds the poison, 8CORE when any byte of it changed,
 * AUTO when the copy is shorter than BC250_SMU_METRICS_COPY_BYTES and there is no tail to read. */
unsigned int bc250_smu_metrics_detect(const unsigned char *raw, unsigned int length);
/* Only inspect the four named Spare bytes of the layout. The community marker's exact byte is not specified. No
 * marker admits a layout or overrides validation; this function never follows an address or reads another table. */
unsigned int bc250_smu_metrics_marker(const unsigned char *raw, unsigned int length, unsigned int layout);
/* A reader for one start. enabled: EnableSmuMetrics. table_ok: the page exists and the SMU owner is online.
 * refused_this_boot: an earlier start of this driver image saw a refusal, so this one sends nothing.
 * layout_setting: SmuMetricsLayout. STOCK or 8CORE latch that layout at once and turn the tail check off; any other
 * value detects the layout from the first BC250_SMU_METRICS_LATCH_TABLES good tables. */
void bc250_smu_metrics_reader_init(struct bc250_smu_metrics_reader *r, int enabled, int table_ok,
				   int refused_this_boot, unsigned int layout_setting);
/* 1 when a read is due at now_ms: the state is WAITING or OK and the period since the last attempt is over. */
int bc250_smu_metrics_due(const struct bc250_smu_metrics_reader *r, unsigned long long now_ms);
/* Records one attempt: the transport's result and, when it is 0, the page as copied (BC250_SMU_METRICS_COPY_BYTES;
 * a shorter copy is PARSE_SHORT). Returns 1 when this attempt ended the reader for the rest of the boot (a refusal),
 * which the miniport latches. An OFFLINE result changes nothing but the time of the next attempt.
 * The layout rules, for a detected layout:
 *   - Before the latch, a table is decoded in the layout its tail shows. A good table extends the run when its
 *     layout equals the run's, and starts a new run of 1 when it does not. Any refused table, a zero-power one
 *     included, ends the run. The table that makes the run BC250_SMU_METRICS_LATCH_TABLES long latches the layout
 *     and is the first one accepted; the tables before it are not published.
 *   - After the latch, a table whose tail shows the other layout is refused (PARSE_LAYOUT) and counts toward the
 *     bad-table limit. The reader never latches again in this start: the SMU firmware does not change while the
 *     board runs, so a changed tail is a fault, not a new layout. */
int bc250_smu_metrics_record(struct bc250_smu_metrics_reader *r, unsigned long long now_ms, int status,
			     const unsigned char *raw, unsigned int length);
/* 1 when the last accepted table is at most BC250_SMU_METRICS_FRESH_MS old and the state is OK. */
int bc250_smu_metrics_fresh(const struct bc250_smu_metrics_reader *r, unsigned long long now_ms);
/* The age of the last accepted table in ms, saturated at 0xFFFFFFFF; 0 when there is none. */
unsigned int bc250_smu_metrics_age_ms(const struct bc250_smu_metrics_reader *r, unsigned long long now_ms);

#endif
