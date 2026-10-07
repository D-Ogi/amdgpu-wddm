/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * BC-250 hardware monitor read policy, and the one write primitive of the fan control (bc250_fan.c owns the
 * handshake and every decision). See include/bc250_hwmon.h for the contract and the access
 * sequence, and bc250-win docs/design/fan.md for the design. Pure: the only outside world this file
 * knows is the eight-bit port vtable it is given, so driver/shim/test/hwmon_test.c drives exactly
 * what the miniport drives.
 *
 * Register numbers and behaviour are facts taken from Linux mainline drivers/hwmon/nct6683.c
 * (GPL-2.0) and, where the header marks a row UNPROVEN, from the out-of-tree nct6687d. No code is
 * copied from either. Two upstream habits are deliberately NOT followed:
 *   - nct6683_init_device sets HWM_CFG bit 7 when the firmware did not. We never write that bit; on
 *     unit A the firmware already runs the monitor, and a reader has no business starting one.
 *   - nct6687d writes 0x61..0x65 into 0x1BB..0x1BF at init and calls it "enable SIO voltage". That
 *     re-assigns four monitor channels. We never write it either.
 */
#include <string.h>
#include "bc250_hwmon.h"

/* ---- the allowlist -------------------------------------------------------------------------- */

/* The three windows the DSDT reports, and nothing else. The header says why a registry DWORD gets a list
 * instead of the two upstream rules: those rules admit 0x0CF8 and 0x0CD0, and this reader writes three latch
 * bytes per register read. The upstream rules are kept as well, in front of the list, so that a base which
 * fails them is refused for the reason upstream gives and the list stays the narrow end of the funnel. */
static const unsigned int g_hwmon_bases[] = {
	BC250_HWMON_BASE_ALT_1, BC250_HWMON_BASE_ALT_2, BC250_HWMON_BASE_DEFAULT
};

int bc250_hwmon_base_allowed(unsigned int base)
{
	unsigned int i;

	if (base < 0x100u || base > BC250_HWMON_BASE_MAX)
		return 0;
	if ((base & 0xF007u) != 0u)
		return 0;
	for (i = 0; i < sizeof(g_hwmon_bases) / sizeof(g_hwmon_bases[0]); i++)
		if (base == g_hwmon_bases[i])
			return 1;
	return 0;
}

static int in_range(unsigned int reg, unsigned int first, unsigned int count, unsigned int stride)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		if (reg == first + i * stride)
			return 1;
	return 0;
}

int bc250_hwmon_read_allowed(unsigned int reg)
{
	if (reg > 0xFFFFu)
		return 0;			/* the page register is eight bits wide */
	/* Every range starts at its own macro, so that a register block which moves in the header cannot leave
	 * an address behind in this list. */
	if (in_range(reg, BC250_HWMON_REG_MON(0), BC250_HWMON_MON_MAX * 2u, 1u))
		return 1;			/* monitor values, both halves of each one */
	if (in_range(reg, BC250_HWMON_REG_FAN(0), BC250_HWMON_FAN_MAX * 2u, 1u))
		return 1;			/* tachometers, both halves */
	if (in_range(reg, BC250_HWMON_REG_DUTY(0), BC250_HWMON_FAN_MAX, 1u))
		return 1;			/* duty read-back */
	if (reg == BC250_HWMON_REG_CFG)
		return 1;			/* HWM_CFG, read side only */
	if (in_range(reg, BC250_HWMON_REG_MON_CFG(0), BC250_HWMON_MON_MAX, 1u))
		return 1;			/* monitor source assignment */
	if (in_range(reg, BC250_HWMON_REG_FAN_PRESENT(0), BC250_HWMON_FAN_MAX, 1u))
		return 1;			/* tachometer present */
	if (in_range(reg, BC250_HWMON_REG_DUTY_PRESENT(0), BC250_HWMON_FAN_MAX, 1u))
		return 1;			/* duty output present */
	if (reg == BC250_HWMON_REG_CUSTOMER || reg == BC250_HWMON_REG_CUSTOMER + 1u)
		return 1;			/* customer ID, 16-bit */
	if (reg >= BC250_HWMON_REG_BUILD_YEAR && reg <= BC250_HWMON_REG_VERSION_LO)
		return 1;			/* build date and firmware version */
	if (reg == BC250_HWMON_REG_MODE)
		return 1;			/* fan mode mask; read only, UNPROVEN */
	if (reg == BC250_HWMON_REG_ENGINE)
		return 1;			/* fan engine status: the handshake polls it */
	if (reg == BC250_HWMON_REG_FAN_CTRL)
		return 1;			/* the configuration request: the open waits for its bit 7 to clear */
	if (reg == BC250_HWMON_REG_DUTY_WRITE(BC250_FAN_CHANNEL))
		return 1;			/* the duty target of the one fan we drive: saved and verified */
	return 0;
}

int bc250_hwmon_write_allowed(unsigned int reg)
{
	/* Three EC registers and nothing else, all measured on unit A (M803). The page and index ports are not EC
	 * registers: they are the chip's own address latch. HWM_CFG, the limits, the beep, the curve points, the
	 * monitor sources (nct6687d's "enable SIO voltage") and the four duty targets of the outputs with no fan
	 * stay refused. */
	return reg == BC250_HWMON_REG_FAN_CTRL || reg == BC250_HWMON_REG_MODE ||
	       reg == BC250_HWMON_REG_DUTY_WRITE(BC250_FAN_CHANNEL);
}

/* ---- conversions ---------------------------------------------------------------------------- */

int bc250_hwmon_temperature_mc(unsigned int raw16, int *mc)
{
	int signed16 = (int)(short)(unsigned short)raw16;
	int value = (signed16 / 128) * 500;

	*mc = 0;
	if (value < BC250_HWMON_TEMP_MIN_MC || value > BC250_HWMON_TEMP_MAX_MC)
		return BC250_HWMON_REFUSED;
	*mc = value;
	return 0;
}

unsigned int bc250_hwmon_duty_permille(unsigned int raw)
{
	if (raw > 255u)
		raw = 255u;
	return (raw * 1000u + 127u) / 255u;
}

int bc250_hwmon_rpm_plausible(unsigned int rpm, unsigned int previous)
{
	if (rpm == BC250_HWMON_RPM_NONE)
		return 0;
	if (rpm > BC250_HWMON_RPM_MAX)
		return 0;
	/* The jump rule needs two turning samples. A fan that starts, or one that stops, has a zero
	 * on one side and is a real event, not a collision. */
	if (rpm != 0u && previous != 0u) {
		if (rpm > previous * BC250_HWMON_RPM_JUMP)
			return 0;
		if (previous > rpm * BC250_HWMON_RPM_JUMP)
			return 0;
	}
	return 1;
}

/* ---- the access sequence -------------------------------------------------------------------- */

static void access_pause(const struct bc250_hwmon_io *io)
{
	if (io->pause != 0)
		io->pause(io->context);
}

/* One byte, with the lock already held. The page is written twice: the unlock value first, then the
 * register's own high byte, exactly as the chip's address latch wants it. */
static unsigned int read8_locked(const struct bc250_hwmon_io *io, unsigned int reg)
{
	unsigned char value;

	io->out8(io->context, BC250_HWMON_PORT_PAGE, BC250_HWMON_PAGE_UNLOCK);
	access_pause(io);
	io->out8(io->context, BC250_HWMON_PORT_PAGE, (unsigned char)((reg >> 8) & 0xFFu));
	access_pause(io);
	io->out8(io->context, BC250_HWMON_PORT_INDEX, (unsigned char)(reg & 0xFFu));
	access_pause(io);
	value = io->in8(io->context, BC250_HWMON_PORT_DATA);
	access_pause(io);
	return value;
}

static void transaction_begin(const struct bc250_hwmon_io *io)
{
	if (io->lock != 0)
		io->lock(io->context);
}

static void transaction_end(const struct bc250_hwmon_io *io)
{
	if (io->unlock != 0)
		io->unlock(io->context);
}

int bc250_hwmon_read8(const struct bc250_hwmon_io *io, unsigned int reg, unsigned int *value)
{
	*value = 0;
	if (!bc250_hwmon_read_allowed(reg))
		return BC250_HWMON_REFUSED;
	transaction_begin(io);
	*value = read8_locked(io, reg);
	transaction_end(io);
	return 0;
}

int bc250_hwmon_read16(const struct bc250_hwmon_io *io, unsigned int reg, unsigned int *value)
{
	unsigned int high, low;

	*value = 0;
	if (!bc250_hwmon_read_allowed(reg) || !bc250_hwmon_read_allowed(reg + 1u))
		return BC250_HWMON_REFUSED;
	/* One hold for both halves: the index latch must not move between them. */
	transaction_begin(io);
	high = read8_locked(io, reg);
	low = read8_locked(io, reg + 1u);
	transaction_end(io);
	*value = (high << 8) | low;
	return 0;
}

int bc250_hwmon_write8(const struct bc250_hwmon_io *io, unsigned int reg, unsigned int value)
{
	if (!bc250_hwmon_write_allowed(reg) || value > 0xFFu || io->out8_data == 0)
		return BC250_HWMON_REFUSED;
	/* The latch is set exactly as for a read, and the data port then takes the byte, all in one hold. */
	transaction_begin(io);
	io->out8(io->context, BC250_HWMON_PORT_PAGE, BC250_HWMON_PAGE_UNLOCK);
	access_pause(io);
	io->out8(io->context, BC250_HWMON_PORT_PAGE, (unsigned char)((reg >> 8) & 0xFFu));
	access_pause(io);
	io->out8(io->context, BC250_HWMON_PORT_INDEX, (unsigned char)(reg & 0xFFu));
	access_pause(io);
	io->out8_data(io->context, (unsigned char)value);
	access_pause(io);
	transaction_end(io);
	return 0;
}

/* ---- identity ------------------------------------------------------------------------------- */

static int plausible_byte_pair(unsigned int a, unsigned int b)
{
	/* A window nothing answers on reads 0xFF everywhere; one that is powered down reads 0x00. */
	if (a == 0u && b == 0u)
		return 0;
	if (a == 0xFFu && b == 0xFFu)
		return 0;
	return 1;
}

static unsigned int present_mask(const struct bc250_hwmon_io *io, unsigned int first, int *bad)
{
	unsigned int mask = 0, i, value;

	for (i = 0; i < BC250_HWMON_FAN_MAX; i++) {
		if (bc250_hwmon_read8(io, first + i, &value) != 0) {
			*bad = 1;
			return 0;
		}
		if (value == 0xFFu)
			*bad = 1;	/* every bit set on every channel is a dead window, not a chip */
		if ((value & BC250_HWMON_PRESENT_BIT) != 0u)
			mask |= 1u << i;
	}
	if (mask == 0u)
		*bad = 1;
	return mask;
}

int bc250_hwmon_identify(const struct bc250_hwmon_io *io, unsigned int base,
			 struct bc250_hwmon_identity *id)
{
	unsigned int hi = 0, lo = 0, year = 0, month = 0, day = 0, customer = 0, cfg = 0, i, source;
	int bad = 0;

	memset(id, 0, sizeof(*id));
	id->base_port = base;
	if (!bc250_hwmon_base_allowed(base)) {
		id->reason = BC250_HWMON_REASON_BASE;
		return BC250_HWMON_REFUSED;
	}
	id->reason = BC250_HWMON_REASON_IDENTITY;
	if (bc250_hwmon_read8(io, BC250_HWMON_REG_VERSION_HI, &hi) != 0 ||
	    bc250_hwmon_read8(io, BC250_HWMON_REG_VERSION_LO, &lo) != 0)
		return BC250_HWMON_REFUSED;
	id->version = (hi << 8) | lo;
	if (!plausible_byte_pair(hi, lo))
		return BC250_HWMON_REFUSED;
	/* The major version, not the whole value: unit A reads 1.0, a later EC build of the same firmware may
	 * read 1.1, and a window that echoes its own index latch reads 8.9. */
	if (hi != BC250_HWMON_EC_MAJOR)
		return BC250_HWMON_REFUSED;
	if (bc250_hwmon_read8(io, BC250_HWMON_REG_BUILD_YEAR, &year) != 0 ||
	    bc250_hwmon_read8(io, BC250_HWMON_REG_BUILD_MONTH, &month) != 0 ||
	    bc250_hwmon_read8(io, BC250_HWMON_REG_BUILD_DAY, &day) != 0)
		return BC250_HWMON_REFUSED;
	/* The chip stores the date in BCD-free bytes: 07/28/21 on unit A. */
	id->build = (year << 16) | (month << 8) | day;
	if (month == 0u || month > 12u || day == 0u || day > 31u)
		return BC250_HWMON_REFUSED;
	if (year < BC250_HWMON_BUILD_YEAR_MIN || year > BC250_HWMON_BUILD_YEAR_MAX)
		return BC250_HWMON_REFUSED;
	if (bc250_hwmon_read16(io, BC250_HWMON_REG_CUSTOMER, &customer) != 0)
		return BC250_HWMON_REFUSED;
	id->customer_id = customer;
	id->fan_present = present_mask(io, 0x1C0u, &bad);
	id->duty_present = present_mask(io, 0x1D0u, &bad);
	if (bad)
		return BC250_HWMON_REFUSED;
	if (bc250_hwmon_read8(io, BC250_HWMON_REG_CFG, &cfg) != 0)
		return BC250_HWMON_REFUSED;
	id->cfg = cfg;
	id->monitoring = (cfg & BC250_HWMON_CFG_MONITOR) != 0u ? 1u : 0u;
	if (!id->monitoring) {
		/* The firmware is not monitoring. Upstream would write bit 7 here. We do not: that is a
		 * configuration change, and this is a reader. */
		id->reason = BC250_HWMON_REASON_MONITORING;
		return BC250_HWMON_REFUSED;
	}
	/* The channel map: the monitor channels are generic, so their meaning comes from 0x1A0 + i. All 32 are
	 * walked once, here; a sample then reads only the channels it kept.
	 *
	 * Two rules inside the walk:
	 *   - the APU channel is kept whatever its index. A board whose first four channels are thermistors
	 *     would otherwise drop the one channel a thermal reading cares about, and the tools would print a
	 *     board sensor where the operator expects the die.
	 *   - a voltage channel is counted, not read. One of them must exist, or this window is not a chip:
	 *     see BC250_HWMON_EC_MAJOR in the header. */
	for (i = 0; i < BC250_HWMON_MON_MAX; i++) {
		if (bc250_hwmon_read8(io, BC250_HWMON_REG_MON_CFG(i), &source) != 0)
			return BC250_HWMON_REFUSED;
		source &= 0x7Fu;
		if (source == 0u)
			continue;		/* unused */
		if (source >= BC250_HWMON_SOURCE_VOLTAGE_FIRST) {
			id->voltages++;		/* we read no voltage: it only proves the chip is one */
			continue;
		}
		if (id->temperatures < BC250_HWMON_TEMP_MAX) {
			id->channel[id->temperatures] = i;
			id->source[id->temperatures] = source;
			id->temperatures++;
		} else if (source == BC250_HWMON_SOURCE_APU) {
			/* The map is full and the APU is not in it. It takes the last slot: three board sensors
			 * and the die beat four board sensors. */
			id->channel[BC250_HWMON_TEMP_MAX - 1u] = i;
			id->source[BC250_HWMON_TEMP_MAX - 1u] = source;
		}
	}
	if (id->voltages == 0u)
		return BC250_HWMON_REFUSED;
	/* The two UNPROVEN registers, once. They are allowlisted, so neither read can fail here. */
	if (bc250_hwmon_read8(io, BC250_HWMON_REG_MODE, &source) == 0)
		id->mode_mask = source & 0xFFu;
	if (bc250_hwmon_read8(io, BC250_HWMON_REG_ENGINE, &source) == 0)
		id->engine = source & 0xFFu;
	id->reason = BC250_HWMON_REASON_OK;
	return 0;
}

/* ---- one sample ----------------------------------------------------------------------------- */

/* A refused value is re-read once, inside the sample's retry budget, because a collision on this
 * unarbitrated window is a one-off. A value that stays refused is published as zero. */
static int read_checked(const struct bc250_hwmon_io *io, unsigned int reg, unsigned int *value,
			struct bc250_hwmon_sample *out, int wide)
{
	unsigned int raw = 0;
	int status;

	out->reads++;
	status = wide ? bc250_hwmon_read16(io, reg, &raw) : bc250_hwmon_read8(io, reg, &raw);
	*value = raw;
	return status;
}

static int accept_rpm(const struct bc250_hwmon_io *io, unsigned int reg, unsigned int previous,
		      struct bc250_hwmon_sample *out, unsigned int *rpm)
{
	unsigned int value = 0;

	if (read_checked(io, reg, &value, out, 1) != 0) {
		out->refusals++;
		*rpm = 0;
		return 0;
	}
	if (!bc250_hwmon_rpm_plausible(value, previous) && out->retries < BC250_HWMON_RETRY_MAX) {
		out->retries++;
		if (read_checked(io, reg, &value, out, 1) != 0) {
			out->refusals++;
			*rpm = 0;
			return 0;
		}
	}
	if (!bc250_hwmon_rpm_plausible(value, previous)) {
		out->refusals++;
		*rpm = 0;
		return 0;
	}
	*rpm = value;
	return 1;
}

/* The raw word first, then the conversion. 0xFFFF and 0x0000 are the chip's "nothing on this
 * channel", and the conversion would hand both of them back as 0.0 C. So would every raw word from 1 to 127,
 * because the step is 0.5 C, so a converted 0 mC is refused as well whatever produced it: the whole purpose
 * of the rule is that a dead or a noisy window never looks like a cold board. */
static int temperature_accepted(unsigned int raw, int *mc)
{
	*mc = 0;
	if (raw == BC250_HWMON_MON_NONE || raw == BC250_HWMON_MON_EMPTY)
		return 0;
	if (bc250_hwmon_temperature_mc(raw, mc) != 0)
		return 0;
	if (*mc == 0) {
		*mc = 0;
		return 0;
	}
	return 1;
}

static int accept_temperature(const struct bc250_hwmon_io *io, unsigned int reg,
			      struct bc250_hwmon_sample *out, int *mc)
{
	unsigned int value = 0;

	*mc = 0;
	if (read_checked(io, reg, &value, out, 1) != 0) {
		out->refusals++;
		return 0;
	}
	if (!temperature_accepted(value, mc) && out->retries < BC250_HWMON_RETRY_MAX) {
		out->retries++;
		if (read_checked(io, reg, &value, out, 1) != 0) {
			out->refusals++;
			return 0;
		}
	}
	if (!temperature_accepted(value, mc)) {
		out->refusals++;
		*mc = 0;
		return 0;
	}
	return 1;
}

int bc250_hwmon_sample(const struct bc250_hwmon_io *io, const struct bc250_hwmon_identity *id,
		       const struct bc250_hwmon_sample *previous,
		       struct bc250_hwmon_sample *out)
{
	unsigned int i, value = 0, accepted = 0, temperatures = 0;

	memset(out, 0, sizeof(*out));
	if (id->reason != BC250_HWMON_REASON_OK)
		return BC250_HWMON_REFUSED;
	for (i = 0; i < BC250_HWMON_FAN_MAX; i++) {
		unsigned int rpm = 0;

		if ((id->fan_present & (1u << i)) == 0u)
			continue;
		if (accept_rpm(io, BC250_HWMON_REG_FAN(i), previous != 0 ? previous->rpm[i] : 0u,
			       out, &rpm)) {
			out->rpm[i] = rpm;
			out->rpm_valid |= 1u << i;
			accepted++;
		}
	}
	for (i = 0; i < BC250_HWMON_FAN_MAX; i++) {
		if ((id->duty_present & (1u << i)) == 0u)
			continue;
		if (read_checked(io, BC250_HWMON_REG_DUTY(i), &value, out, 0) != 0) {
			out->refusals++;
			continue;
		}
		out->duty[i] = value & 0xFFu;
		out->duty_valid |= 1u << i;
		/* Published, but it does not make the sample: see the contract in the header. */
	}
	/* The mode mask and the fan engine status are NOT read here. Both are documented by the out-of-tree
	 * driver alone, neither is proven on this board, and no decision of ours reads either one, so
	 * bc250_hwmon_identify reads them once at start instead of putting eight port accesses a second on an
	 * uncharacterised register of the chip that cools the board. */
	for (i = 0; i < id->temperatures && i < BC250_HWMON_TEMP_MAX; i++) {
		int mc = 0;

		if (accept_temperature(io, BC250_HWMON_REG_MON(id->channel[i]), out, &mc)) {
			out->temperature_mc[i] = mc;
			out->temperature_valid |= 1u << i;
			temperatures++;
			accepted++;
		}
	}
	/* Every mapped temperature channel refused means the window stopped answering, whatever the
	 * tachometers and the duty read-backs said. A chip that answers has at least one of these. */
	if (id->temperatures != 0u && temperatures == 0u)
		return BC250_HWMON_REFUSED;
	return accepted != 0u ? 0 : BC250_HWMON_REFUSED;
}
