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
 *     assert that counter stays zero for every read.
 *   - the write path of the fan control (Part B). A data-port write lands only through ec_out8_data, after the same
 *     latch sequence as a read, and every one is logged with its register, so a test can hold the whole log to
 *     the allowlist and to the order of the handshake. The fan engine follows M803: 0x80 to 0x0A01 opens a phase
 *     after `open_delay` status polls (0x0CF8 goes from 0x60 to 0x08), 0x40 closes it after `close_delay` polls
 *     (0x60 again). A mode or target write outside an open phase counts a protocol error. The duty read-back of
 *     fan 1 follows the target while the mode bit is set, and the tachometer follows the duty.
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
	/* 1: every data read answers the index byte it was last given. That is an I/O window with nothing behind
	 * it but its own address latch, and it is the shape the identity rules have to refuse: every byte it
	 * returns is plausible on its own. */
	int		answer_index;

	/* ---- the write path and the fan engine (Part B) ---- */
	unsigned int	writes_data, protocol_errors, engine_reads;
	struct { unsigned int reg, value; } log[512];
	unsigned int	logged;
	int		data_admitted;		/* 1: a data-port write through ec_out8 lands as through ec_out8_data */
	int		engine_model;		/* 1: 0x0CF8, 0x0A01, the read-back and the tachometer of fan 1 are live */
	unsigned int	open_delay, close_delay;	/* status polls before the engine answers */
	unsigned int	pending_open, pending_close;
	int		stuck_open;		/* the request is never granted */
	int		stuck_close;		/* CHECK_DONE never comes back */
	int		invalid_on_close;	/* the close sets INVALID */
	int		unlocked_on_close;	/* the close leaves LOCK clear */
	int		ignore_target;		/* a target write does not land */
	int		drop_mode_at_close;	/* the close clears the mode bit of fan 1 */
	int		readback_stuck;		/* the duty read-back of fan 1 keeps its old value */
	int		fan_stopped;		/* the tachometer of fan 1 reads 0 whatever the duty */
	unsigned int	curve_duty;		/* what the EC curve drives while the mode bit is clear */
	unsigned int	curve_rpm;
};

static void ec_reset(struct ec_mock *ec)
{
	memset(ec, 0, sizeof(*ec));
}

/* The log of data-port writes, for the tests: how many went to `reg`, and the index of the n-th one. */
static unsigned int ec_writes_to(const struct ec_mock *ec, unsigned int reg)
{
	unsigned int i, n = 0;

	for (i = 0; i < ec->logged; i++)
		if (ec->log[i].reg == reg)
			n++;
	return n;
}

static void ec_engine_poll(struct ec_mock *ec)
{
	ec->engine_reads++;
	if (ec->pending_open && --ec->pending_open == 0u) {
		ec->mem[BC250_HWMON_REG_ENGINE] = BC250_HWMON_ENGINE_PHASE;
		ec->mem[BC250_HWMON_REG_FAN_CTRL] = 0;
	}
	if (ec->pending_close && --ec->pending_close == 0u) {
		unsigned char status = BC250_HWMON_ENGINE_CHECK_DONE;

		if (!ec->unlocked_on_close)
			status |= BC250_HWMON_ENGINE_LOCK;
		if (ec->invalid_on_close)
			status |= BC250_HWMON_ENGINE_INVALID;
		ec->mem[BC250_HWMON_REG_ENGINE] = status;
		ec->mem[BC250_HWMON_REG_FAN_CTRL] = 0;
		if (ec->drop_mode_at_close)
			ec->mem[BC250_HWMON_REG_MODE] &= (unsigned char)(0xFFu & ~(1u << BC250_FAN_CHANNEL));
	}
}

static int ec_phase_open(const struct ec_mock *ec)
{
	unsigned char status = ec->mem[BC250_HWMON_REG_ENGINE];

	return (status & BC250_HWMON_ENGINE_PHASE) != 0u && (status & BC250_HWMON_ENGINE_LOCK) == 0u;
}

static void ec_data_write(struct ec_mock *ec, unsigned char value)
{
	unsigned int address;

	if (ec->step != 3) {
		ec->sequence_errors++;
		ec->step = 0;
		return;
	}
	ec->step = 0;
	ec->writes_data++;
	address = ((ec->page << 8) | ec->index) & (EC_MOCK_BYTES - 1u);
	if (ec->logged < sizeof(ec->log) / sizeof(ec->log[0])) {
		ec->log[ec->logged].reg = address;
		ec->log[ec->logged].value = value;
		ec->logged++;
	}
	if (!ec->engine_model) {
		ec->mem[address] = value;
		return;
	}
	if (address == BC250_HWMON_REG_FAN_CTRL) {
		ec->mem[address] = value;
		if (value == BC250_HWMON_FAN_CFG_REQUEST && !ec->stuck_open)
			ec->pending_open = ec->open_delay + 1u;
		else if (value == BC250_HWMON_FAN_CFG_DONE && ec_phase_open(ec) && !ec->stuck_close)
			ec->pending_close = ec->close_delay + 1u;
		return;
	}
	if (address == BC250_HWMON_REG_MODE || (address >= BC250_HWMON_REG_DUTY_WRITE(0) &&
						address <= BC250_HWMON_REG_DUTY_WRITE(7))) {
		if (!ec_phase_open(ec)) {
			ec->protocol_errors++;
			return;
		}
		if (address == BC250_HWMON_REG_DUTY_WRITE(BC250_FAN_CHANNEL) && ec->ignore_target)
			return;
	}
	ec->mem[address] = value;
}

static void ec_out8_data(void *context, unsigned char value)
{
	struct ec_mock *ec = (struct ec_mock *)context;

	if (!ec->held)
		ec->outside_hold++;
	ec->accesses_in_hold++;
	ec_data_write(ec, value);
}

/* What a live register of the model answers, or -1 for "the memory as it is". */
static int ec_live(struct ec_mock *ec, unsigned int address)
{
	unsigned int fan = BC250_FAN_CHANNEL, manual, duty, rpm;

	if (!ec->engine_model)
		return -1;
	if (address == BC250_HWMON_REG_ENGINE) {
		ec_engine_poll(ec);
		return ec->mem[address];
	}
	manual = (ec->mem[BC250_HWMON_REG_MODE] & (1u << fan)) != 0u;
	duty = manual ? ec->mem[BC250_HWMON_REG_DUTY_WRITE(fan)] : ec->curve_duty;
	if (address == BC250_HWMON_REG_DUTY(fan))
		return ec->readback_stuck ? ec->mem[address] : (int)duty;
	rpm = ec->fan_stopped ? 0u : manual ? duty * 1720u / 255u : ec->curve_rpm;
	if (address == BC250_HWMON_REG_FAN(fan))
		return (int)((rpm >> 8) & 0xFFu);
	if (address == BC250_HWMON_REG_FAN(fan) + 1u)
		return (int)(rpm & 0xFFu);
	return -1;
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
	if (port == BC250_HWMON_PORT_DATA && ec->data_admitted) {
		/* The miniport's write transport reaches the data port through WRITE_PORT_UCHAR like every other
		 * access, so the native test admits it here; the shim test uses ec_out8_data directly. */
		ec->accesses_in_hold++;
		ec_data_write(ec, value);
		return;
	}
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
	if (ec->answer_index)
		return (unsigned char)ec->index;
	address = ((ec->page << 8) | ec->index) & (EC_MOCK_BYTES - 1u);
	{
		int live = ec_live(ec, address);

		if (live >= 0)
			return (unsigned char)live;
	}
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
 * SB-TSI, two board thermistors at 59.5 C and SIX voltage channels. The EC firmware is 1.0, built 2021-07-28.
 * The customer ID is the one value E01 did not print, so the fixture carries an ASRock value and the tests
 * never assert which one the board has.
 *
 * The monitor channels are deliberately NOT 0, 1, 2. E01 printed the values and their labels, not which
 * monitor index carried each one, so the index map is an assumption either way - and a fixture that puts the
 * three temperatures first lets a reader that ignores MON_CFG and uses a fixed table pass every test. The
 * voltages therefore interleave with the temperatures, which is also what makes the "at least one voltage"
 * identity rule and the channel map itself testable. The real map is a stage-1 readback (A13). */
#define EC_MOCK_UNIT_A_CUSTOMER 0x0E2Cu
#define EC_MOCK_APU_CHANNEL 1u
#define EC_MOCK_TH14_CHANNEL 5u
#define EC_MOCK_TH15_CHANNEL 9u

/* The six voltage sources E01 found, at the even channels between the temperatures. */
static const unsigned char ec_mock_voltages[6][2] = {
	{ 0u, 0x66u }, { 2u, 0x67u }, { 4u, 0x68u }, { 6u, 0x6Cu }, { 8u, 0x6Du }, { 10u, 0x76u }
};

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
	ec_put8(ec, BC250_HWMON_REG_MON_CFG(EC_MOCK_APU_CHANNEL), BC250_HWMON_SOURCE_APU);
	ec_put8(ec, BC250_HWMON_REG_MON_CFG(EC_MOCK_TH14_CHANNEL), BC250_HWMON_SOURCE_THERMISTOR14);
	ec_put8(ec, BC250_HWMON_REG_MON_CFG(EC_MOCK_TH15_CHANNEL), BC250_HWMON_SOURCE_THERMISTOR15);
	for (i = 0; i < 6u; i++) {
		ec_put8(ec, BC250_HWMON_REG_MON_CFG(ec_mock_voltages[i][0]), ec_mock_voltages[i][1]);
		ec_put16(ec, BC250_HWMON_REG_MON(ec_mock_voltages[i][0]), 0x0330u);	/* a voltage raw word */
	}
	ec_put16(ec, BC250_HWMON_REG_MON(EC_MOCK_APU_CHANNEL), 0x5300u);	/* 83.0 C: 166 steps of 0.5 C */
	ec_put16(ec, BC250_HWMON_REG_MON(EC_MOCK_TH14_CHANNEL), 0x3B80u);	/* 59.5 C */
	ec_put16(ec, BC250_HWMON_REG_MON(EC_MOCK_TH15_CHANNEL), 0x3B80u);
	ec_put16(ec, BC250_HWMON_REG_FAN(1), 1589u);	/* the one fan that turns */
	ec_put8(ec, BC250_HWMON_REG_MODE, 0x00u);	/* Standard Mode: the EC curve owns every channel */
	ec_put8(ec, BC250_HWMON_REG_ENGINE, 0x00u);
}

/* Unit A as the write test of M803 found it at rest (2026-10-06): customer ID 0x162B, the mode mask 0xE0, the
 * engine status 0x60 (CHECK_DONE and LOCK), every duty read-back 204, every duty target 128, fan 1 at 1360 RPM,
 * the configuration request 0. The engine answers an open after one poll and a close after three, as measured. */
#define EC_MOCK_M803_CUSTOMER 0x162Bu

static void ec_unit_a_m803(struct ec_mock *ec)
{
	unsigned int i;

	ec_unit_a(ec);
	ec_put16(ec, BC250_HWMON_REG_CUSTOMER, EC_MOCK_M803_CUSTOMER);
	ec_put8(ec, BC250_HWMON_REG_MODE, BC250_HWMON_MODE_REST);
	ec_put8(ec, BC250_HWMON_REG_ENGINE, BC250_HWMON_ENGINE_CHECK_DONE | BC250_HWMON_ENGINE_LOCK);
	ec_put8(ec, BC250_HWMON_REG_FAN_CTRL, 0);
	for (i = 0; i < 8u; i++)
		ec_put8(ec, BC250_HWMON_REG_DUTY_WRITE(i), BC250_HWMON_TARGET_REST);
	for (i = 0; i < 5u; i++)
		ec_put8(ec, BC250_HWMON_REG_DUTY(i), 204);
	ec->engine_model = 1;
	ec->open_delay = 1;
	ec->close_delay = 3;
	ec->curve_duty = 204;
	ec->curve_rpm = 1360;
}

/* The fan control's transport: the read transport plus the data port. */
static void ec_io_write(struct bc250_hwmon_io *io, struct ec_mock *ec)
{
	ec_io(io, ec);
	io->out8_data = ec_out8_data;
}

#endif
