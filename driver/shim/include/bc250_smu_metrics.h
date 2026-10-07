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
#define BC250_SMU_METRICS_OFF_CORECLK 0u        /* uint16 CoreFrequency[6], MHz: the firmware's own CPU core clocks */
#define BC250_SMU_METRICS_OFF_L3CLK 48u         /* uint16 L3Frequency[2], MHz */
#define BC250_SMU_METRICS_OFF_GFXCLK 68u        /* uint16 GfxclkFrequency, MHz */
#define BC250_SMU_METRICS_OFF_SOCCLK 72u        /* uint16 SocclkFrequency, MHz */
#define BC250_SMU_METRICS_OFF_VCLK 74u          /* uint16 VclkFrequency, MHz */
#define BC250_SMU_METRICS_OFF_DCLK 76u          /* uint16 DclkFrequency, MHz */
#define BC250_SMU_METRICS_OFF_MEMCLK 78u        /* uint16 MemclkFrequency, MHz */
#define BC250_SMU_METRICS_OFF_GFX_TEMP 70u      /* uint16 GfxTemperature, centi-Celsius (amdgpu: edge) */
#define BC250_SMU_METRICS_OFF_VOLTAGE 80u       /* uint32 Voltage[2], mV: [0] VDDCR_VDD, [1] VDDCR_GFX */
#define BC250_SMU_METRICS_OFF_CURRENT 88u       /* uint32 Current[2], mA, same indices */
#define BC250_SMU_METRICS_OFF_POWER 96u         /* uint32 Power[2], mW, same indices */
#define BC250_SMU_METRICS_OFF_SOCKET 104u       /* uint32 CurrentSocketPower, mW: the whole package */
#define BC250_SMU_METRICS_OFF_SOC_TEMP 108u     /* uint16 SocTemperature, centi-Celsius (amdgpu: hotspot) */
#define BC250_SMU_METRICS_OFF_THROTTLER 112u    /* uint16 ThrottlerStatus */
#define BC250_SMU_METRICS_OFF_ACCNT 240u        /* uint32 Accnt, after SampleStartTime and SampleStopTime */
#define BC250_SMU_METRICS_CPU_CORES 6u          /* CoreFrequency[] entries: the six cores the part is sold with */
#define BC250_SMU_METRICS_L3 2u                 /* L3Frequency[] entries */
/* The SoC, memory and CPU clocks of the table (0.7.216.15, K137). The table has no FCLK and no STAPM or power-limit
 * field: MemclkFrequency, SocclkFrequency, the two L3 clocks and the six core clocks are what it says about the
 * fabric side and the processor, and ThrottlerStatus (above) is its only limit indicator. They are read-only
 * telemetry: no check below depends on them, and a clock field that still holds the poison reads 0 ("not
 * written") instead of refusing a table the power fields made whole. */

/* The byte the owner fills the page with before every transfer. A field that still holds it afterwards was not
 * written by the firmware, so the table is refused instead of read as 4294967 W. */
#define BC250_SMU_METRICS_POISON 0xFFu

/* Plausibility bounds of one accepted table. The lab PSU is 300 W (owner, 2026-10-01); a package figure above 400 W
 * or a rail voltage above 2 V is not a reading of this part, and neither is a temperature above 150 C. */
#define BC250_SMU_METRICS_MAX_MW 400000u
#define BC250_SMU_METRICS_MAX_MV 2000u
#define BC250_SMU_METRICS_MAX_CC 15000u

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
	BC250_SMU_METRICS_PARSE_SHORT = 1,      /* fewer than BC250_SMU_METRICS_BYTES bytes */
	BC250_SMU_METRICS_PARSE_UNWRITTEN = 2,  /* the poison is still in a field the firmware always writes */
	BC250_SMU_METRICS_PARSE_RANGE = 3,      /* a value outside the bounds above */
	BC250_SMU_METRICS_PARSE_ZERO_POWER = 4  /* the current or average socket power reads 0 mW: a skipped sample */
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
	/* Telemetry only (0.7.216.15): 0 for a field the firmware left unwritten. */
	unsigned int socclk_mhz;        /* Current.SocclkFrequency */
	unsigned int memclk_mhz;        /* Current.MemclkFrequency */
	unsigned int vclk_mhz;          /* Current.VclkFrequency */
	unsigned int dclk_mhz;          /* Current.DclkFrequency */
	unsigned int l3_mhz[BC250_SMU_METRICS_L3];              /* Current.L3Frequency[] */
	unsigned int core_mhz[BC250_SMU_METRICS_CPU_CORES];     /* Current.CoreFrequency[] */
};

/* The reader. The governor thread alone touches it; the miniport copies what it publishes. */
struct bc250_smu_metrics_reader {
	unsigned int state;             /* enum bc250_smu_metrics_state */
	unsigned int reads;             /* tables accepted in this start */
	unsigned int failures;          /* reads that ended without a table: refusals, bad tables */
	unsigned int bad_in_row;
	int last_status;                /* the transport's last result, 0 for success */
	unsigned int last_parse;        /* enum bc250_smu_metrics_parse of the last table the firmware wrote */
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
/* Decodes one table. On BC250_SMU_METRICS_PARSE_OK *out holds it; on anything else *out is zeroed. */
enum bc250_smu_metrics_parse bc250_smu_metrics_parse(const unsigned char *raw, unsigned int length,
						     struct bc250_smu_metrics *out);
/* A reader for one start. enabled: EnableSmuMetrics. table_ok: the page exists and the SMU owner is online.
 * refused_this_boot: an earlier start of this driver image saw a refusal, so this one sends nothing. */
void bc250_smu_metrics_reader_init(struct bc250_smu_metrics_reader *r, int enabled, int table_ok,
				   int refused_this_boot);
/* 1 when a read is due at now_ms: the state is WAITING or OK and the period since the last attempt is over. */
int bc250_smu_metrics_due(const struct bc250_smu_metrics_reader *r, unsigned long long now_ms);
/* Records one attempt: the transport's result and, when it is 0, the page as copied. Returns 1 when this attempt
 * ended the reader for the rest of the boot (a refusal), which the miniport latches. An OFFLINE result changes
 * nothing but the time of the next attempt. */
int bc250_smu_metrics_record(struct bc250_smu_metrics_reader *r, unsigned long long now_ms, int status,
			     const unsigned char *raw, unsigned int length);
/* 1 when the last accepted table is at most BC250_SMU_METRICS_FRESH_MS old and the state is OK. */
int bc250_smu_metrics_fresh(const struct bc250_smu_metrics_reader *r, unsigned long long now_ms);
/* The age of the last accepted table in ms, saturated at 0xFFFFFFFF; 0 when there is none. */
unsigned int bc250_smu_metrics_age_ms(const struct bc250_smu_metrics_reader *r, unsigned long long now_ms);

#endif
