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
 * The READ path writes only the hardware monitor's page and index ports, never a configuration,
 * control, limit or duty register. The host test (test/hwmon_test.c) asserts that the mock EC saw no
 * write outside the page and index ports for every read.
 *
 * The WRITE path (Part B, docs/design/fan.md, driver/shim/bc250_fan.c) is one function,
 * bc250_hwmon_write8(), and it writes exactly three EC registers: the fan configuration request at
 * 0x0A01, the manual-mode mask at 0x0A00 and the duty target of the one fan that turns, 0x0A28 + 1.
 * bc250_hwmon_write_allowed() is that list as code. The write needs a data-port writer in the io
 * vtable (out8_data). The sampler's transport has none, so a read can never become a write. The
 * register meanings and the handshake are measured on unit A (fact M803).
 *
 * Register facts come from Linux mainline drivers/hwmon/nct6683.c (GPL-2.0, facts only, no code
 * taken), which binds on this chip and read live values at E01, and - where a row says so - from
 * the out-of-tree nct6687d, which is MSI-centred and therefore an inference for this board. Every
 * inferred register is marked UNPROVEN below and needs one lab readback before anything depends on
 * it. M803 (unit A, 2026-10-06) measured the mode mask, the engine status, the configuration request
 * and the duty target of fan index 1, so those four are no longer UNPROVEN for that one channel.
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

/* The three windows this chip can sit on, and the only three HwmonBasePort admits. The DSDT reports exactly
 * these and nothing else: IO1B 0x0A00, IO2B 0x0A10 and IO3B 0x0A20, 16 bytes each (facts-firmware, DSDT
 * _CRS of SIO1). The hardware monitor itself is on the third one, which E01 also measured.
 *
 * An allowlist and not a rule, because the two upstream rules (at least 0x100, eight-byte aligned, bits 12
 * to 15 clear) were written for a base READ OUT OF THE SUPER I/O, where the chip itself names the window.
 * Here the value comes from the registry, and those rules admit 0x0CF8, the PCI configuration address port,
 * and 0x0CD0, the FCH power-management index pair. The reader would then write its latch bytes into one of
 * them once a second. A registry DWORD is not a chip's own answer, so it gets a list. */
#define BC250_HWMON_BASE_DEFAULT 0x0A20u	/* measured on unit A (E01 ioports.txt, DSDT IO3B) */
#define BC250_HWMON_BASE_ALT_1	0x0A00u		/* DSDT IO1B */
#define BC250_HWMON_BASE_ALT_2	0x0A10u		/* DSDT IO2B */
#define BC250_HWMON_BASE_MAX	0xFFF8u		/* a port is 16 bits wide: the HAL would truncate anything above */

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
#define BC250_HWMON_REG_MODE		0x0A00u	/* one bit per channel, set = manual. Measured for bit 1 (M803) */
#define BC250_HWMON_REG_ENGINE		0x0CF8u	/* fan engine status. The handshake bits below are measured (M803) */

/* The write-side registers. bc250_hwmon_write_allowed() admits FAN_CTRL, MODE and DUTY_WRITE(BC250_FAN_CHANNEL)
 * and nothing else. The other names are here so that a mistake is visible in review and in the host test. */
#define BC250_HWMON_REG_FAN_CTRL	0x0A01u		/* fan configuration request */
#define BC250_HWMON_REG_DUTY_WRITE(i)	(0x0A28u + (i))	/* duty target of output i, 0..255 */
#define BC250_HWMON_REG_SIO_VOLTAGE	0x01BBu		/* nct6687d re-assigns 0x1BB..0x1BF at init: never */
#define BC250_HWMON_REG_BEEP		0x00E0u

/* The one fan the write path drives: duty output and tachometer index 1 (Linux fan2). M803 measured that it is
 * the only fan that turns on unit A, and that its duty read-back follows the written target. The other four
 * outputs exist and are never written. */
#define BC250_FAN_CHANNEL		1u

/* The configuration handshake, measured on unit A (M803, the nct6687d sequence). FAN_CTRL takes REQUEST to open a
 * configuration phase and DONE to close it. nct6687d closes with 0x00 on an NCT6683 only, where that value is
 * reported to clear the mode mask and every duty target. It is never ours: this chip is an NCT6686D. */
#define BC250_HWMON_FAN_CFG_REQUEST	0x80u
#define BC250_HWMON_FAN_CFG_DONE	0x40u
/* ENGINE bits. At rest unit A reads 0x60 (CHECK_DONE and LOCK). An open moves it to 0x08 (PHASE) within one 1 ms
 * poll. A close sets CHECK_DONE and LOCK again within three or four polls. INVALID never set on unit A. */
#define BC250_HWMON_ENGINE_PHASE	0x08u
#define BC250_HWMON_ENGINE_INVALID	0x10u
#define BC250_HWMON_ENGINE_CHECK_DONE	0x20u
#define BC250_HWMON_ENGINE_LOCK		0x40u
/* What unit A reads at rest with the EC curve in charge (M803): the mode mask 0xE0 (bits 0 to 4 clear) and every
 * duty target 128. The restore writes these when the saved record cannot be trusted. */
#define BC250_HWMON_MODE_REST		0xE0u
#define BC250_HWMON_TARGET_REST		128u

/* Monitor source codes measured on unit A through the Linux labels (E01 sensors-all.txt).
 * A source below 0x60 is a temperature, 0x60 and above a voltage. */
#define BC250_HWMON_SOURCE_VOLTAGE_FIRST 0x60u
#define BC250_HWMON_SOURCE_APU		0x46u	/* AMD TSI at SMBus 0x98: the APU die, 83.0 C at E01 */
#define BC250_HWMON_SOURCE_THERMISTOR14	0x08u	/* a board sensor, 59.5 C at E01 */
#define BC250_HWMON_SOURCE_THERMISTOR15	0x09u	/* a board sensor, 59.5 C at E01 */

/* What the identity insists on, beyond "the bytes are not all 0x00 or all 0xFF". A window that only echoes
 * its own index latch answers every read with the low byte of the register it was asked for, which passes
 * that pair: version 0x0809, a build date of 04/05/06, five present masks of 0xFF... and the reader would
 * then publish four temperature channels of 0.0 C from a window with no chip behind it.
 *
 * So three independent rules, each one a value an echo cannot produce:
 *   - the firmware's major version is 1 (unit A reads 1.0; the echo reads 8.9),
 *   - the build year is between 2020 and 2039 (the echo reads 2004),
 *   - at least one monitor channel carries a VOLTAGE source. E01 found six on this board (VIN0, VIN1, VIN2,
 *     VIN6, VIN7, VIN16) through the same MON_CFG walk, so this costs a real chip nothing, and an echo
 *     cannot give one: its sources are the channel indexes 0x20 to 0x3F, every one of them a temperature. */
#define BC250_HWMON_EC_MAJOR		1u	/* 0x608: the EC firmware major version this reader knows */
#define BC250_HWMON_BUILD_YEAR_MIN	20u	/* 0x604 holds the year without its century */
#define BC250_HWMON_BUILD_YEAR_MAX	39u

/* Plausibility bounds. A collision with another agent on this unarbitrated window returns a
 * plausible byte and no error, so every sample is checked. */
#define BC250_HWMON_RPM_NONE		0xFFFFu	/* the chip's own "no reading" */
/* A monitor channel with nothing on it reads 0xFFFF or 0x0000, as a tachometer reads RPM_NONE. Both
 * are refused before the conversion, because the conversion turns either of them into 0.0 C and a
 * dead window must never look like a cold board. A sensor genuinely at 0.0 C is refused with them.
 * That costs one reading on a board whose coldest sensor sits inside a closed case next to an APU,
 * and it buys the one thing the window cannot otherwise give us: a wrong reading that says so.
 * The conversion step is 0.5 C, so the raw words 1 to 127 also come out as 0 mC: refusing the two
 * words alone would let a quarter of a degree of noise print the same 0.0 C this rule exists to
 * prevent. A CONVERTED 0 mC is therefore refused as well, whatever raw word produced it. */
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
	/* The write path only. Null in the sampler's transport, so a read can never write the data port:
	 * bc250_hwmon_write8() refuses without it. delay_us waits between two handshake polls. It may be null (no
	 * wait), which the host test uses. */
	void		(*out8_data)(void *context, unsigned char value);
	void		(*delay_us)(void *context, unsigned int us);
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
	unsigned int	voltages;	/* voltage channels seen in the walk; at least one, or the window is not a chip */
	unsigned int	channel[BC250_HWMON_TEMP_MAX];	/* monitor index of each one */
	unsigned int	source[BC250_HWMON_TEMP_MAX];	/* its source code */
	/* The two UNPROVEN registers, read once here and never again. They answer a Part B question ("does the
	 * EC curve own every channel at rest?") and no decision of ours reads them, so a sample has no business
	 * putting eight more port accesses a second on an uncharacterised register of the chip that cools the
	 * board. Published and logged as the start found them. */
	unsigned int	mode_mask;	/* 0xA00 as read: one bit per channel, set = manual. UNPROVEN */
	unsigned int	engine;		/* 0xCF8 as read: fan engine status. UNPROVEN, published and logged only */
	unsigned int	reason;		/* enum bc250_hwmon_reason */
};

/* One sample. A refused value is published as 0 with its valid bit clear: never a guess. */
struct bc250_hwmon_sample {
	unsigned int	rpm[BC250_HWMON_FAN_MAX];
	unsigned int	rpm_valid;	/* bit i */
	unsigned int	duty[BC250_HWMON_FAN_MAX];	/* raw 0..255 */
	unsigned int	duty_valid;
	int		temperature_mc[BC250_HWMON_TEMP_MAX];
	unsigned int	temperature_valid;
	unsigned int	reads;		/* register transactions of this sample */
	unsigned int	retries;	/* refused values re-read */
	unsigned int	refusals;	/* values that stayed refused */
};

/* ---- policy -------------------------------------------------------------------------------- */

/* The base a HwmonBasePort may name: one of the three windows the DSDT reports, and nothing else. See the
 * BC250_HWMON_BASE_* block above for why this is a list and not the two upstream rules. */
int bc250_hwmon_base_allowed(unsigned int base);

/* The read allowlist, as code. Everything outside it is refused, a page above 0xFF included. */
int bc250_hwmon_read_allowed(unsigned int reg);

/* The write allowlist, as code: BC250_HWMON_REG_FAN_CTRL, BC250_HWMON_REG_MODE and
 * BC250_HWMON_REG_DUTY_WRITE(BC250_FAN_CHANNEL). Every other register is refused. */
int bc250_hwmon_write_allowed(unsigned int reg);

/* One register, through the four- (or eight-) access sequence, inside one lock hold. Zero on
 * success; BC250_HWMON_REFUSED when the register is outside the allowlist. */
#define BC250_HWMON_REFUSED (-22)
int bc250_hwmon_read8(const struct bc250_hwmon_io *io, unsigned int reg, unsigned int *value);
int bc250_hwmon_read16(const struct bc250_hwmon_io *io, unsigned int reg, unsigned int *value);
/* One register written: unlock, page, index, then the data port, inside one lock hold. BC250_HWMON_REFUSED when
 * the register is outside the write allowlist, the value is not a byte or the transport has no out8_data. No port
 * is touched then. */
int bc250_hwmon_write8(const struct bc250_hwmon_io *io, unsigned int reg, unsigned int value);

/* The chip identity, the channel map and the two UNPROVEN registers, all read once. Zero when the
 * chip answered plausibly; otherwise the reason is in id->reason and the caller must not sample.
 *
 * The walk reads all 32 MON_CFG entries, keeps at most BC250_HWMON_TEMP_MAX temperature channels
 * and counts the voltage channels it passed. The APU channel (source BC250_HWMON_SOURCE_APU) is
 * kept whatever its index: it is the one channel a thermal decision would ever read, and a board
 * that puts four thermistors before it must not push it out of the map. */
int bc250_hwmon_identify(const struct bc250_hwmon_io *io, unsigned int base,
			 struct bc250_hwmon_identity *id);

/* One sample of every present tachometer and duty output and of the mapped temperature channels.
 * `previous` may be null (the first sample).
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
