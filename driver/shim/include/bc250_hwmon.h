/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * BC-250 hardware monitor (Nuvoton NCT6686D) read path. Pure policy: no OS call, no lock of its
 * own, no time source. The miniport (driver/kmd/hwmon.c) owns the ports, the lock, the gate and
 * the published snapshot; bc250-win docs/design/fan.md is the design.
 *
 * The chip has two access paths. The Super I/O configuration pair 0x2E/0x2F finds the EC base on
 * every other board. WE NEVER USE IT: the DSDT drives the same pair under ACPI mutex
 * \_SB.PCI0.SBRG.SIO1.MUT0, which a kernel driver cannot take, and the enter/select/exit sequence
 * has no abort. The base is a constant on this board (0x0A20, from the DSDT _CRS and from our own
 * Linux recon E01) and the chip identity is readable from the EC window alone. So this module only
 * ever drives the EC window, four ports at base + 4.
 *
 * One 8-bit read of EC register `reg` is four port accesses:
 *   write 0xFF        -> page    (the unlock value)
 *   write reg >> 8    -> page
 *   write reg & 0xFF  -> index
 *   read              <- data
 * A 16-bit read is two such reads, high byte at reg and low byte at reg + 1, inside one lock hold
 * (the index latch must not move between them).
 *
 * THESE ARE THE ONLY WRITES THIS READER ISSUES. They go to the hardware monitor's page and index
 * ports, never to a configuration, control, limit or duty register. There is no write function in
 * this module at all, and bc250_hwmon_write_allowed() answers "no" for every register, so a future
 * duty write cannot arrive by accident. The host test (test/hwmon_test.c) asserts that the mock EC
 * saw no write outside the page and index ports.
 *
 * Register facts come from Linux mainline drivers/hwmon/nct6683.c (GPL-2.0, facts only, no code
 * taken), which binds on this chip and read live values at E01, and - where a row says so - from
 * the out-of-tree nct6687d, which is MSI-centred and therefore an inference for this board. Every
 * inferred register is marked UNPROVEN below and needs one lab readback before anything depends on
 * it. Nothing in Part A depends on an UNPROVEN register for a decision; the two are read, published
 * and logged only.
 */
#ifndef BC250_HWMON_H
#define BC250_HWMON_H

/* ---- the EC window ------------------------------------------------------------------------- */

/* Port indexes handed to the io vtable. Never an address: the miniport adds the base, so a bug in
 * this module cannot name a port outside the window it was started with. */
#define BC250_HWMON_PORT_PAGE	0u	/* base + 4 */
#define BC250_HWMON_PORT_INDEX	1u	/* base + 5 */
#define BC250_HWMON_PORT_DATA	2u	/* base + 6 */
#define BC250_HWMON_PORT_EVENT	3u	/* base + 7, never touched */
#define BC250_HWMON_PORT_COUNT	4u

#define BC250_HWMON_PAGE_UNLOCK	0xFFu	/* written to the page port before the page itself */
#define BC250_HWMON_BASE_DEFAULT 0x0A20u	/* measured on unit A (E01 ioports.txt, DSDT IO3B) */

/* ---- EC registers, read side -------------------------------------------------------------- */

#define BC250_HWMON_MON_MAX	32u	/* monitor channels */
#define BC250_HWMON_FAN_MAX	8u	/* tachometers and duty outputs */
#define BC250_HWMON_TEMP_MAX	4u	/* temperature channels this reader publishes */

#define BC250_HWMON_REG_MON(i)		(0x100u + 2u * (i))	/* monitor value, 16-bit */
#define BC250_HWMON_REG_FAN(i)		(0x140u + 2u * (i))	/* tachometer, raw RPM, 16-bit */
#define BC250_HWMON_REG_DUTY(i)		(0x160u + (i))		/* duty read-back, 0..255 */
#define BC250_HWMON_REG_CFG		0x180u			/* HWM_CFG; read only, never written */
#define BC250_HWMON_CFG_MONITOR		0x80u			/* bit 7: the monitor runs */
#define BC250_HWMON_REG_MON_CFG(i)	(0x1A0u + (i))		/* monitor source, low 7 bits */
#define BC250_HWMON_REG_FAN_PRESENT(i)	(0x1C0u + (i))		/* bit 7: this tachometer exists */
#define BC250_HWMON_REG_DUTY_PRESENT(i)	(0x1D0u + (i))		/* bit 7: this duty output exists */
#define BC250_HWMON_PRESENT_BIT		0x80u
#define BC250_HWMON_REG_CUSTOMER	0x602u			/* customer ID, 16-bit */
#define BC250_HWMON_REG_BUILD_YEAR	0x604u
#define BC250_HWMON_REG_BUILD_MONTH	0x605u
#define BC250_HWMON_REG_BUILD_DAY	0x606u
#define BC250_HWMON_REG_VERSION_HI	0x608u
#define BC250_HWMON_REG_VERSION_LO	0x609u
#define BC250_HWMON_REG_MODE		0x0A00u	/* UNPROVEN: one bit per channel, set = manual. nct6687d only */
#define BC250_HWMON_REG_ENGINE		0x0CF8u	/* UNPROVEN: fan engine status. nct6687d only; logged, never used */

/* The write-side registers, named so that a mistake is visible in review and in the host test.
 * bc250_hwmon_write_allowed() refuses every register, these included. */
#define BC250_HWMON_REG_FAN_CTRL	0x0A01u		/* fan configuration control */
#define BC250_HWMON_REG_DUTY_WRITE(i)	(0x0A28u + (i))	/* duty write */
#define BC250_HWMON_REG_SIO_VOLTAGE	0x01BBu		/* nct6687d re-assigns 0x1BB..0x1BF at init: never */
#define BC250_HWMON_REG_BEEP		0x00E0u

/* Monitor source codes measured on unit A through the Linux labels (E01 sensors-all.txt).
 * A source below 0x60 is a temperature, 0x60 and above a voltage. */
#define BC250_HWMON_SOURCE_VOLTAGE_FIRST 0x60u
#define BC250_HWMON_SOURCE_APU		0x46u	/* AMD TSI at SMBus 0x98: the APU die, 83.0 C at E01 */
#define BC250_HWMON_SOURCE_THERMISTOR14	0x08u	/* a board sensor, 59.5 C at E01 */
#define BC250_HWMON_SOURCE_THERMISTOR15	0x09u	/* a board sensor, 59.5 C at E01 */

/* Plausibility bounds. A collision with another agent on this unarbitrated window returns a
 * plausible byte and no error, so every sample is checked. */
#define BC250_HWMON_RPM_NONE		0xFFFFu	/* the chip's own "no reading" */
/* A monitor channel with nothing on it reads 0xFFFF or 0x0000, as a tachometer reads RPM_NONE. Both
 * are refused before the conversion, because the conversion turns either of them into 0.0 C and a
 * dead window must never look like a cold board. A sensor genuinely at 0.0 C is refused with them.
 * That costs one reading on a board whose coldest sensor sits inside a closed case next to an APU,
 * and it buys the one thing the window cannot otherwise give us: a wrong reading that says so. */
#define BC250_HWMON_MON_NONE		0xFFFFu
#define BC250_HWMON_MON_EMPTY		0x0000u
#define BC250_HWMON_RPM_MAX		6000u	/* the measured full-duty speed of this board is 3100 */
#define BC250_HWMON_RPM_JUMP		4u	/* factor between two samples, both nonzero */
#define BC250_HWMON_TEMP_MIN_MC		(-40000)
#define BC250_HWMON_TEMP_MAX_MC		150000
#define BC250_HWMON_RETRY_MAX		4u	/* retries per sample, over every register */

/* Why the reader is not online, or not reading. Shared with the escape (BC250_ESCAPE_HWMON) and
 * printed by the tools. */
enum bc250_hwmon_reason {
	BC250_HWMON_REASON_OK = 0,
	BC250_HWMON_REASON_GATED = 1,		/* EnableHwmon is 0: no port access happened at all */
	BC250_HWMON_REASON_BASE = 2,		/* HwmonBasePort is not a window this chip can sit on */
	BC250_HWMON_REASON_IDENTITY = 3,	/* version, build date or a present mask is not a chip */
	BC250_HWMON_REASON_MONITORING = 4,	/* HWM_CFG bit 7 clear: the firmware is not monitoring */
	BC250_HWMON_REASON_CUSTOMER = 5,	/* HwmonExpectId is pinned and does not match */
	BC250_HWMON_REASON_NO_THREAD = 6,	/* no governor thread, so nothing samples */
	BC250_HWMON_REASON_PORT = 7,		/* the reads kept failing: the reader gave up */
	BC250_HWMON_REASON_COUNT
};

/* The miniport's 8-bit port transport. `port` is one of BC250_HWMON_PORT_*, never an address.
 * pause, lock and unlock may be null. lock/unlock bound ONE register transaction: four accesses for
 * an 8-bit read, eight for a 16-bit one. The holder takes no other lock. */
struct bc250_hwmon_io {
	void		*context;
	void		(*out8)(void *context, unsigned int port, unsigned char value);
	unsigned char	(*in8)(void *context, unsigned int port);
	void		(*pause)(void *context);	/* the ISA pause after each access */
	void		(*lock)(void *context);
	void		(*unlock)(void *context);
};

/* What the chip is, read once at start from the EC window alone. */
struct bc250_hwmon_identity {
	unsigned int	base_port;
	unsigned int	customer_id;	/* 0x602; unknown on unit A until the first lab readback */
	unsigned int	version;	/* 0x608 << 8 | 0x609; 0x0100 on unit A */
	unsigned int	build;		/* year << 16 | month << 8 | day; 2021-07-28 on unit A */
	unsigned int	cfg;		/* 0x180 as read */
	unsigned int	monitoring;	/* cfg & BC250_HWMON_CFG_MONITOR */
	unsigned int	fan_present;	/* bit i: tachometer i exists */
	unsigned int	duty_present;	/* bit i: duty output i exists */
	unsigned int	temperatures;	/* temperature channels found, at most BC250_HWMON_TEMP_MAX */
	unsigned int	channel[BC250_HWMON_TEMP_MAX];	/* monitor index of each one */
	unsigned int	source[BC250_HWMON_TEMP_MAX];	/* its source code */
	unsigned int	reason;		/* enum bc250_hwmon_reason */
};

/* One sample. A refused value is published as 0 with its valid bit clear: never a guess. */
struct bc250_hwmon_sample {
	unsigned int	rpm[BC250_HWMON_FAN_MAX];
	unsigned int	rpm_valid;	/* bit i */
	unsigned int	duty[BC250_HWMON_FAN_MAX];	/* raw 0..255 */
	unsigned int	duty_valid;
	unsigned int	mode_mask;	/* 0xA00 as read; UNPROVEN */
	unsigned int	engine;		/* 0xCF8 as read; UNPROVEN, logged only */
	int		temperature_mc[BC250_HWMON_TEMP_MAX];
	unsigned int	temperature_valid;
	unsigned int	reads;		/* register transactions of this sample */
	unsigned int	retries;	/* refused values re-read */
	unsigned int	refusals;	/* values that stayed refused */
};

/* ---- policy -------------------------------------------------------------------------------- */

/* The base a HwmonBasePort may name. The two rules are the ones the Linux drivers apply to the base
 * they read out of the Super I/O: at least 0x100, and (base & 0xF007) == 0. 0x0A20 passes. */
int bc250_hwmon_base_allowed(unsigned int base);

/* The read allowlist, as code. Everything outside it is refused, a page above 0xFF included. */
int bc250_hwmon_read_allowed(unsigned int reg);

/* Always 0, for every register. There is no write path; this exists so the host test can state the
 * rule and so a reviewer sees one answer for the whole chip. */
int bc250_hwmon_write_allowed(unsigned int reg);

/* One register, through the four- (or eight-) access sequence, inside one lock hold. Zero on
 * success; BC250_HWMON_REFUSED when the register is outside the allowlist. */
#define BC250_HWMON_REFUSED (-22)
int bc250_hwmon_read8(const struct bc250_hwmon_io *io, unsigned int reg, unsigned int *value);
int bc250_hwmon_read16(const struct bc250_hwmon_io *io, unsigned int reg, unsigned int *value);

/* The chip identity and the channel map. Zero when the chip answered plausibly; otherwise the
 * reason is in id->reason and the caller must not sample. */
int bc250_hwmon_identify(const struct bc250_hwmon_io *io, unsigned int base,
			 struct bc250_hwmon_identity *id);

/* One sample of every present tachometer and duty output, the mode mask, the engine status and the
 * mapped temperature channels. `previous` may be null (the first sample).
 *
 * Zero when the sample is a reading, BC250_HWMON_REFUSED when it is not. A reading needs at least
 * one accepted tachometer or temperature, AND every mapped temperature channel refused makes the
 * sample refused whatever else came back. The reason is that a duty read-back cannot tell a chip
 * from a dead window: it is a byte the EC wrote, and 0xFF (full duty) and 0x00 (no duty) are both
 * legal. A temperature can, because 0xFFFF and 0x0000 are refused above. So the sample stands or
 * falls on the one value the window cannot fake, and the miniport's give-up rule actually fires. */
int bc250_hwmon_sample(const struct bc250_hwmon_io *io, const struct bc250_hwmon_identity *id,
		       const struct bc250_hwmon_sample *previous,
		       struct bc250_hwmon_sample *out);

/* A speed this board can turn at. `previous` 0 means "no earlier sample": the jump rule is then
 * not applied. A fan that starts or stops passes it too, because one of the two values is 0. */
int bc250_hwmon_rpm_plausible(unsigned int rpm, unsigned int previous);

/* (signed raw / 128) * 500 millidegrees, a 0.5 C step. Zero when the result is inside the bounds. */
int bc250_hwmon_temperature_mc(unsigned int raw16, int *mc);

/* raw of 255 to permille, rounded to nearest: 245 -> 961. */
unsigned int bc250_hwmon_duty_permille(unsigned int raw);

#endif
