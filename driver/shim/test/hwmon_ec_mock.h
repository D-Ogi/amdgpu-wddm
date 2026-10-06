/* A model of the NCT6686D's EC window, for the two hardware-monitor host tests. Plain C: no Windows, so the
 * shim test and the native test share exactly one model of the chip.
 *
 * What it models, and therefore what it can catch:
 *   - the address latch. A data read answers only when the page was unlocked with 0xFF, then set to the
 *     register's high byte, then the index was set to its low byte, in that order. A wrong order, a missing
 *     unlock or a stale index counts a sequence error instead of returning a plausible byte.
 *   - the lock discipline. Every access must happen inside a hold, and the model records how many accesses one
 *     hold covered, so a 16-bit read that split into two holds is visible.
 *   - the write rule. A write to anything but the page and the index port is counted separately, and the tests
 *     assert that counter stays zero.
 */
#ifndef BC250_HWMON_EC_MOCK_H
#define BC250_HWMON_EC_MOCK_H

#include <string.h>
#include "bc250_hwmon.h"

#define EC_MOCK_BYTES 0x10000

struct ec_mock {
	unsigned char	mem[EC_MOCK_BYTES];
	unsigned int	page, index, step;	/* step: 0 none, 1 unlocked, 2 page set, 3 index set */
	unsigned int	reads, writes_latch, writes_other, sequence_errors, outside_hold;
	int		held, holds, accesses_in_hold, max_accesses_in_hold;
	int		answer_fixed;		/* 1: every data read answers `fixed`, whatever the address */
	unsigned char	fixed;
};

static void ec_reset(struct ec_mock *ec)
{
	memset(ec, 0, sizeof(*ec));
}

static void ec_put8(struct ec_mock *ec, unsigned int reg, unsigned char value)
{
	ec->mem[reg & (EC_MOCK_BYTES - 1u)] = value;
}

/* Straight out of the model's memory, without the latch: how a test asks "and did it stay that way?". */
static unsigned char ec_peek8(const struct ec_mock *ec, unsigned int reg)
{
	return ec->mem[reg & (EC_MOCK_BYTES - 1u)];
}

static void ec_put16(struct ec_mock *ec, unsigned int reg, unsigned int value)
{
	ec_put8(ec, reg, (unsigned char)((value >> 8) & 0xFFu));
	ec_put8(ec, reg + 1u, (unsigned char)(value & 0xFFu));
}

static void ec_out8(void *context, unsigned int port, unsigned char value)
{
	struct ec_mock *ec = (struct ec_mock *)context;

	if (!ec->held)
		ec->outside_hold++;
	ec->accesses_in_hold++;
	if (port != BC250_HWMON_PORT_PAGE && port != BC250_HWMON_PORT_INDEX) {
		ec->writes_other++;
		return;
	}
	ec->writes_latch++;
	if (port == BC250_HWMON_PORT_PAGE) {
		if (value == BC250_HWMON_PAGE_UNLOCK) {
			ec->step = 1;
		} else if (ec->step == 1) {
			ec->page = value;
			ec->step = 2;
		} else {
			ec->sequence_errors++;
			ec->step = 0;
		}
		return;
	}
	if (ec->step != 2) {
		ec->sequence_errors++;
		ec->step = 0;
		return;
	}
	ec->index = value;
	ec->step = 3;
}

static unsigned char ec_in8(void *context, unsigned int port)
{
	struct ec_mock *ec = (struct ec_mock *)context;
	unsigned int address;

	if (!ec->held)
		ec->outside_hold++;
	ec->accesses_in_hold++;
	if (port != BC250_HWMON_PORT_DATA) {
		ec->sequence_errors++;
		return 0;
	}
	ec->reads++;
	if (ec->step != 3) {
		ec->sequence_errors++;
		return 0;
	}
	ec->step = 0;
	if (ec->answer_fixed)
		return ec->fixed;
	address = ((ec->page << 8) | ec->index) & (EC_MOCK_BYTES - 1u);
	return ec->mem[address];
}

static void ec_lock(void *context)
{
	struct ec_mock *ec = (struct ec_mock *)context;

	ec->held++;
	ec->holds++;
	ec->accesses_in_hold = 0;
}

static void ec_unlock(void *context)
{
	struct ec_mock *ec = (struct ec_mock *)context;

	if (ec->accesses_in_hold > ec->max_accesses_in_hold)
		ec->max_accesses_in_hold = ec->accesses_in_hold;
	ec->held--;
}

static void ec_io(struct bc250_hwmon_io *io, struct ec_mock *ec)
{
	memset(io, 0, sizeof(*io));
	io->context = ec;
	io->out8 = ec_out8;
	io->in8 = ec_in8;
	io->pause = 0;
	io->lock = ec_lock;
	io->unlock = ec_unlock;
}

/* Unit A as our own Linux recon E01 measured it: five tachometers and five duty outputs, one fan turning at
 * 1589 RPM on channel 1 (fan2 in the Linux labels), every duty read-back 245 of 255, the APU at 83.0 C over
 * SB-TSI and two board thermistors at 59.5 C. The EC firmware is 1.0, built 2021-07-28.
 * The customer ID is the one value E01 did not print, so the fixture carries an ASRock value and the tests
 * never assert which one the board has. */
#define EC_MOCK_UNIT_A_CUSTOMER 0x0E2Cu

static void ec_unit_a(struct ec_mock *ec)
{
	unsigned int i;

	ec_reset(ec);
	ec_put8(ec, BC250_HWMON_REG_VERSION_HI, 1);
	ec_put8(ec, BC250_HWMON_REG_VERSION_LO, 0);
	ec_put8(ec, BC250_HWMON_REG_BUILD_YEAR, 21);
	ec_put8(ec, BC250_HWMON_REG_BUILD_MONTH, 7);
	ec_put8(ec, BC250_HWMON_REG_BUILD_DAY, 28);
	ec_put16(ec, BC250_HWMON_REG_CUSTOMER, EC_MOCK_UNIT_A_CUSTOMER);
	ec_put8(ec, BC250_HWMON_REG_CFG, BC250_HWMON_CFG_MONITOR);
	for (i = 0; i < 5; i++) {
		ec_put8(ec, BC250_HWMON_REG_FAN_PRESENT(i), BC250_HWMON_PRESENT_BIT);
		ec_put8(ec, BC250_HWMON_REG_DUTY_PRESENT(i), BC250_HWMON_PRESENT_BIT);
		ec_put8(ec, BC250_HWMON_REG_DUTY(i), 245);
	}
	ec_put8(ec, BC250_HWMON_REG_MON_CFG(0), BC250_HWMON_SOURCE_APU);
	ec_put8(ec, BC250_HWMON_REG_MON_CFG(1), BC250_HWMON_SOURCE_THERMISTOR14);
	ec_put8(ec, BC250_HWMON_REG_MON_CFG(2), BC250_HWMON_SOURCE_THERMISTOR15);
	ec_put16(ec, BC250_HWMON_REG_MON(0), 0x5300u);	/* 83.0 C: 166 steps of 0.5 C, 166 * 128 */
	ec_put16(ec, BC250_HWMON_REG_MON(1), 0x3B80u);	/* 59.5 C */
	ec_put16(ec, BC250_HWMON_REG_MON(2), 0x3B80u);
	ec_put16(ec, BC250_HWMON_REG_FAN(1), 1589u);	/* the one fan that turns */
	ec_put8(ec, BC250_HWMON_REG_MODE, 0x00u);	/* Standard Mode: the EC curve owns every channel */
	ec_put8(ec, BC250_HWMON_REG_ENGINE, 0x00u);
}

#endif
