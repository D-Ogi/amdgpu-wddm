/* The hardware-monitor read policy, driven on the host against a model of the chip (hwmon_ec_mock.h).
 *
 * What this test is for: the reader writes three bytes to read one, on a port range that has no arbiter, on a
 * chip that also holds a duty register. So the three things that must never drift are the access sequence, the
 * allowlist and the refusal of every write. All three are checked here, against the real
 * driver/shim/bc250_hwmon.c, not a copy of it.
 *
 *   pwsh driver\shim\test\run_hwmon.ps1
 */
#include <stdio.h>
#include "hwmon_ec_mock.h"

static int checks, failures;

#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

/* ---- the access sequence ------------------------------------------------------------------------------- */

static void sequence(void)
{
	struct ec_mock ec;
	struct bc250_hwmon_io io;
	unsigned int value = 0;

	ec_unit_a(&ec);
	ec_io(&io, &ec);
	/* One 8-bit read is four port accesses, in one hold: unlock, page, index, data. */
	CHECK(bc250_hwmon_read8(&io, BC250_HWMON_REG_CFG, &value) == 0);
	CHECK(value == BC250_HWMON_CFG_MONITOR);
	CHECK(ec.writes_latch == 3 && ec.reads == 1 && ec.writes_other == 0);
	CHECK(ec.holds == 1 && ec.max_accesses_in_hold == 4 && ec.outside_hold == 0);
	CHECK(ec.sequence_errors == 0);

	/* A 16-bit read is two of those, high byte at reg and low byte at reg + 1, inside ONE hold: the index
	 * latch must not move between the halves. */
	ec_unit_a(&ec);
	CHECK(bc250_hwmon_read16(&io, BC250_HWMON_REG_FAN(1), &value) == 0);
	CHECK(value == 1589u);
	CHECK(ec.writes_latch == 6 && ec.reads == 2 && ec.writes_other == 0);
	CHECK(ec.holds == 1 && ec.max_accesses_in_hold == 8 && ec.sequence_errors == 0);

	/* The byte order is not symmetric: proving it needs two different bytes. */
	ec_unit_a(&ec);
	ec_put8(&ec, BC250_HWMON_REG_FAN(1), 0x12);
	ec_put8(&ec, BC250_HWMON_REG_FAN(1) + 1u, 0x34);
	CHECK(bc250_hwmon_read16(&io, BC250_HWMON_REG_FAN(1), &value) == 0 && value == 0x1234u);
}

/* ---- the allowlist and the write refusal ---------------------------------------------------------------- */

static void allowlist(void)
{
	struct ec_mock ec;
	struct bc250_hwmon_io io;
	unsigned int value = 0, i;

	/* Both ends of every admitted range. */
	CHECK(bc250_hwmon_read_allowed(0x100u));
	CHECK(bc250_hwmon_read_allowed(0x100u + BC250_HWMON_MON_MAX * 2u - 1u));	/* 0x13F */
	CHECK(!bc250_hwmon_read_allowed(0x0FFu));	/* below the monitor block: the EC's own scratch */
	CHECK(bc250_hwmon_read_allowed(0x140u));	/* the tachometers follow the monitors with no gap */
	CHECK(bc250_hwmon_read_allowed(0x140u + BC250_HWMON_FAN_MAX * 2u - 1u));
	CHECK(bc250_hwmon_read_allowed(0x160u) && bc250_hwmon_read_allowed(0x167u));
	CHECK(!bc250_hwmon_read_allowed(0x168u));
	CHECK(bc250_hwmon_read_allowed(BC250_HWMON_REG_CFG));
	CHECK(bc250_hwmon_read_allowed(0x1A0u) && bc250_hwmon_read_allowed(0x1BFu));
	CHECK(bc250_hwmon_read_allowed(0x1C0u) && bc250_hwmon_read_allowed(0x1C7u));
	CHECK(bc250_hwmon_read_allowed(0x1D0u) && bc250_hwmon_read_allowed(0x1D7u));
	CHECK(bc250_hwmon_read_allowed(BC250_HWMON_REG_CUSTOMER));
	CHECK(bc250_hwmon_read_allowed(BC250_HWMON_REG_CUSTOMER + 1u));
	CHECK(bc250_hwmon_read_allowed(BC250_HWMON_REG_BUILD_YEAR));
	CHECK(bc250_hwmon_read_allowed(BC250_HWMON_REG_VERSION_LO));
	CHECK(bc250_hwmon_read_allowed(BC250_HWMON_REG_MODE));
	CHECK(bc250_hwmon_read_allowed(BC250_HWMON_REG_ENGINE));

	/* The write-side registers of this chip. Refused for read as well, because a reader has no reason to
	 * look at them and because the next mistake would be to write one. */
	CHECK(!bc250_hwmon_read_allowed(BC250_HWMON_REG_FAN_CTRL));
	for (i = 0; i < BC250_HWMON_FAN_MAX; i++)
		CHECK(!bc250_hwmon_read_allowed(BC250_HWMON_REG_DUTY_WRITE(i)));
	/* 0x1BB..0x1BF are the source assignments of monitor channels 27 to 31, so reading them is ordinary: the
	 * identity walk reads all 32. nct6687d WRITES 0x61..0x65 into them at init and calls it "enable SIO
	 * voltage", which re-assigns four channels of a chip the BIOS owns. That write is what is refused, and
	 * these five registers are the clearest case of why there is no write path at all. */
	for (i = 0; i < 5u; i++) {
		CHECK(bc250_hwmon_read_allowed(BC250_HWMON_REG_SIO_VOLTAGE + i));
		CHECK(!bc250_hwmon_write_allowed(BC250_HWMON_REG_SIO_VOLTAGE + i));
	}
	CHECK(!bc250_hwmon_read_allowed(BC250_HWMON_REG_BEEP));
	CHECK(!bc250_hwmon_read_allowed(0x330u) && !bc250_hwmon_read_allowed(0x350u));
	CHECK(!bc250_hwmon_read_allowed(0x370u) && !bc250_hwmon_read_allowed(0x3B8u));
	/* The page register is eight bits wide. */
	CHECK(!bc250_hwmon_read_allowed(0x10000u));
	CHECK(!bc250_hwmon_read_allowed(0xFFFFFFFFu));

	/* No register at all may be written, the admitted ones included. */
	CHECK(!bc250_hwmon_write_allowed(BC250_HWMON_REG_CFG));
	CHECK(!bc250_hwmon_write_allowed(BC250_HWMON_REG_FAN_CTRL));
	CHECK(!bc250_hwmon_write_allowed(BC250_HWMON_REG_DUTY_WRITE(0)));
	CHECK(!bc250_hwmon_write_allowed(BC250_HWMON_REG_MODE));
	CHECK(!bc250_hwmon_write_allowed(0u));

	/* A refused register touches no port at all. */
	ec_unit_a(&ec);
	ec_io(&io, &ec);
	CHECK(bc250_hwmon_read8(&io, BC250_HWMON_REG_FAN_CTRL, &value) == BC250_HWMON_REFUSED);
	CHECK(bc250_hwmon_read8(&io, BC250_HWMON_REG_DUTY_WRITE(3), &value) == BC250_HWMON_REFUSED);
	CHECK(bc250_hwmon_read16(&io, 0x10000u, &value) == BC250_HWMON_REFUSED);
	CHECK(value == 0);
	CHECK(ec.writes_latch == 0 && ec.reads == 0 && ec.holds == 0);
}

/* ---- the base port ------------------------------------------------------------------------------------- */

static void base(void)
{
	CHECK(bc250_hwmon_base_allowed(BC250_HWMON_BASE_DEFAULT));	/* 0x0A20, measured on unit A */
	CHECK(!bc250_hwmon_base_allowed(0u));
	CHECK(!bc250_hwmon_base_allowed(0xFFu));
	CHECK(!bc250_hwmon_base_allowed(0x0A21u));			/* the low three bits must be clear */
	CHECK(!bc250_hwmon_base_allowed(0x1A20u));			/* and so must bits 12 to 15 */
	CHECK(bc250_hwmon_base_allowed(0x0290u));
}

/* ---- conversions -------------------------------------------------------------------------------------- */

/* A compile-time statement of the line below: the upper bound lies above everything a signed word can carry,
 * so it is defence and never a live refusal. Written as a type and not as a CHECK, because a constant `if` is
 * a warning and this is not a run-time question. */
typedef char hwmon_upper_bound_is_defence[BC250_HWMON_TEMP_MAX_MC > 127500 ? 1 : -1];

static void conversions(void)
{
	int mc = 0;

	CHECK(bc250_hwmon_temperature_mc(0u, &mc) == 0 && mc == 0);
	CHECK(bc250_hwmon_temperature_mc(128u, &mc) == 0 && mc == 500);			/* one 0.5 C step */
	CHECK(bc250_hwmon_temperature_mc(0x5300u, &mc) == 0 && mc == 83000);		/* 83.0 C at E01 */
	CHECK(bc250_hwmon_temperature_mc(0x3B80u, &mc) == 0 && mc == 59500);		/* 59.5 C at E01 */
	CHECK(bc250_hwmon_temperature_mc(0xFF00u, &mc) == 0 && mc == -1000);		/* -1 C: a signed word */
	/* The upper bound is defence and not a live path: a signed word tops out at 127.5 C, which is inside it.
	 * The lower bound is reachable, and that is the one a dead window hits. */
	CHECK(bc250_hwmon_temperature_mc(0x7FFFu, &mc) == 0 && mc == 127500);
	CHECK(bc250_hwmon_temperature_mc(0x8000u, &mc) == BC250_HWMON_REFUSED && mc == 0);

	CHECK(bc250_hwmon_duty_permille(0u) == 0u);
	CHECK(bc250_hwmon_duty_permille(245u) == 961u);		/* 96 %, what every channel read at E01 */
	CHECK(bc250_hwmon_duty_permille(255u) == 1000u);
	CHECK(bc250_hwmon_duty_permille(128u) == 502u);
}

static void plausibility(void)
{
	CHECK(bc250_hwmon_rpm_plausible(1589u, 0u));
	CHECK(bc250_hwmon_rpm_plausible(1589u, 1600u));
	CHECK(bc250_hwmon_rpm_plausible(0u, 1589u));		/* a fan that stopped is a real event */
	CHECK(bc250_hwmon_rpm_plausible(3100u, 0u));		/* this board's measured full-duty speed */
	CHECK(!bc250_hwmon_rpm_plausible(BC250_HWMON_RPM_NONE, 0u));	/* the chip's own "no reading" */
	CHECK(!bc250_hwmon_rpm_plausible(20000u, 0u));
	CHECK(!bc250_hwmon_rpm_plausible(BC250_HWMON_RPM_MAX + 1u, 0u));
	CHECK(!bc250_hwmon_rpm_plausible(1589u, 300u));		/* a jump by more than a factor of four */
	CHECK(!bc250_hwmon_rpm_plausible(300u, 1589u));
	CHECK(bc250_hwmon_rpm_plausible(1589u, 400u));		/* exactly four is still a fan spinning up */
}

/* ---- identity ------------------------------------------------------------------------------------------ */

static void identity(void)
{
	struct ec_mock ec;
	struct bc250_hwmon_io io;
	struct bc250_hwmon_identity id;

	ec_unit_a(&ec);
	ec_io(&io, &ec);
	CHECK(bc250_hwmon_identify(&io, BC250_HWMON_BASE_DEFAULT, &id) == 0);
	CHECK(id.reason == BC250_HWMON_REASON_OK);
	CHECK(id.base_port == BC250_HWMON_BASE_DEFAULT);
	CHECK(id.version == 0x0100u);					/* EC firmware 1.0 */
	CHECK(id.build == ((21u << 16) | (7u << 8) | 28u));		/* 2021-07-28 */
	CHECK(id.customer_id == EC_MOCK_UNIT_A_CUSTOMER);
	CHECK(id.monitoring == 1u);
	CHECK(id.fan_present == 0x1Fu && id.duty_present == 0x1Fu);	/* five channels each */
	/* The source walk: the channel map, not a fixed table. nct6687d's hard-coded MSI layout would get this
	 * wrong on this board. */
	CHECK(id.temperatures == 3u);
	CHECK(id.channel[0] == 0u && id.source[0] == BC250_HWMON_SOURCE_APU);
	CHECK(id.channel[1] == 1u && id.source[1] == BC250_HWMON_SOURCE_THERMISTOR14);
	CHECK(id.channel[2] == 2u && id.source[2] == BC250_HWMON_SOURCE_THERMISTOR15);
	CHECK(ec.writes_other == 0 && ec.sequence_errors == 0 && ec.outside_hold == 0);

	/* A base the chip cannot sit on is refused before any port is touched. */
	ec_unit_a(&ec);
	CHECK(bc250_hwmon_identify(&io, 0x80u, &id) == BC250_HWMON_REFUSED);
	CHECK(id.reason == BC250_HWMON_REASON_BASE && ec.reads == 0);

	/* A window nothing answers on. */
	ec_unit_a(&ec);
	ec.answer_fixed = 1;
	ec.fixed = 0xFF;
	CHECK(bc250_hwmon_identify(&io, BC250_HWMON_BASE_DEFAULT, &id) == BC250_HWMON_REFUSED);
	CHECK(id.reason == BC250_HWMON_REASON_IDENTITY);
	ec_unit_a(&ec);
	ec.answer_fixed = 1;
	ec.fixed = 0x00;
	CHECK(bc250_hwmon_identify(&io, BC250_HWMON_BASE_DEFAULT, &id) == BC250_HWMON_REFUSED);
	CHECK(id.reason == BC250_HWMON_REASON_IDENTITY);

	/* A build date that is not a date. */
	ec_unit_a(&ec);
	ec_put8(&ec, BC250_HWMON_REG_BUILD_MONTH, 13);
	CHECK(bc250_hwmon_identify(&io, BC250_HWMON_BASE_DEFAULT, &id) == BC250_HWMON_REFUSED);
	CHECK(id.reason == BC250_HWMON_REASON_IDENTITY);
	ec_unit_a(&ec);
	ec_put8(&ec, BC250_HWMON_REG_BUILD_DAY, 32);
	CHECK(bc250_hwmon_identify(&io, BC250_HWMON_BASE_DEFAULT, &id) == BC250_HWMON_REFUSED);

	/* No present mask at all, and every bit set on every channel: neither is a chip. */
	ec_unit_a(&ec);
	ec_put8(&ec, BC250_HWMON_REG_FAN_PRESENT(0), 0);
	ec_put8(&ec, BC250_HWMON_REG_FAN_PRESENT(1), 0);
	ec_put8(&ec, BC250_HWMON_REG_FAN_PRESENT(2), 0);
	ec_put8(&ec, BC250_HWMON_REG_FAN_PRESENT(3), 0);
	ec_put8(&ec, BC250_HWMON_REG_FAN_PRESENT(4), 0);
	CHECK(bc250_hwmon_identify(&io, BC250_HWMON_BASE_DEFAULT, &id) == BC250_HWMON_REFUSED);
	CHECK(id.reason == BC250_HWMON_REASON_IDENTITY);
	ec_unit_a(&ec);
	ec_put8(&ec, BC250_HWMON_REG_DUTY_PRESENT(2), 0xFF);
	CHECK(bc250_hwmon_identify(&io, BC250_HWMON_BASE_DEFAULT, &id) == BC250_HWMON_REFUSED);

	/* The firmware is not monitoring. Upstream nct6683_init_device would write HWM_CFG bit 7 here; we
	 * refuse instead, because that is a configuration change and this is a reader. */
	ec_unit_a(&ec);
	ec_put8(&ec, BC250_HWMON_REG_CFG, 0x01);
	CHECK(bc250_hwmon_identify(&io, BC250_HWMON_BASE_DEFAULT, &id) == BC250_HWMON_REFUSED);
	CHECK(id.reason == BC250_HWMON_REASON_MONITORING);
	CHECK(ec.writes_other == 0);
}

/* ---- one sample ---------------------------------------------------------------------------------------- */

static void sampling(void)
{
	struct ec_mock ec;
	struct bc250_hwmon_io io;
	struct bc250_hwmon_identity id;
	struct bc250_hwmon_sample first, second;
	unsigned int i;

	ec_unit_a(&ec);
	ec_io(&io, &ec);
	CHECK(bc250_hwmon_identify(&io, BC250_HWMON_BASE_DEFAULT, &id) == 0);
	CHECK(bc250_hwmon_sample(&io, &id, 0, &first) == 0);
	/* Unit A, as E01 read it through the Linux labels. */
	CHECK(first.rpm[1] == 1589u && (first.rpm_valid & 2u) != 0u);
	CHECK(first.rpm[0] == 0u && first.rpm[2] == 0u);
	CHECK(first.rpm_valid == 0x1Fu);		/* all five channels answered; four of them with 0 */
	CHECK(first.duty_valid == 0x1Fu);
	for (i = 0; i < 5u; i++)
		CHECK(first.duty[i] == 245u);
	CHECK(first.temperature_valid == 7u);
	CHECK(first.temperature_mc[0] == 83000 && first.temperature_mc[1] == 59500);
	CHECK(first.mode_mask == 0u);			/* Standard Mode: the EC curve owns our channel */
	CHECK(first.refusals == 0 && first.retries == 0);
	CHECK(ec.writes_other == 0 && ec.sequence_errors == 0 && ec.outside_hold == 0);

	/* The chip's own "no reading" never becomes a number. */
	ec_unit_a(&ec);
	ec_put16(&ec, BC250_HWMON_REG_FAN(1), BC250_HWMON_RPM_NONE);
	CHECK(bc250_hwmon_sample(&io, &id, 0, &second) == 0);
	CHECK(second.rpm[1] == 0u && (second.rpm_valid & 2u) == 0u && second.refusals >= 1u);

	/* Nor does a speed this board cannot turn at. */
	ec_unit_a(&ec);
	ec_put16(&ec, BC250_HWMON_REG_FAN(1), 20000u);
	CHECK(bc250_hwmon_sample(&io, &id, 0, &second) == 0);
	CHECK(second.rpm[1] == 0u && (second.rpm_valid & 2u) == 0u);

	/* A jump by more than a factor of four is re-read once, because a collision on this unarbitrated window
	 * is a one-off; a value that stays implausible is published as zero, never as a guess. */
	ec_unit_a(&ec);
	ec_put16(&ec, BC250_HWMON_REG_FAN(1), 300u);
	CHECK(bc250_hwmon_sample(&io, &id, &first, &second) == 0);
	CHECK(second.retries == 1u && second.rpm[1] == 0u && (second.rpm_valid & 2u) == 0u);
	/* ...and a plausible value after the retry is taken. */
	ec_unit_a(&ec);
	ec_put16(&ec, BC250_HWMON_REG_FAN(1), 1600u);
	CHECK(bc250_hwmon_sample(&io, &id, &first, &second) == 0);
	CHECK(second.retries == 0u && second.rpm[1] == 1600u);

	/* A whole window that answers 0xFF: no reading at all, and still no write. The duty read-back DOES come
	 * back (0xFF is a legal full duty), and that is exactly why a duty alone may not make a sample: it cannot
	 * tell a chip from a dead window. The temperature can, so the sample falls. */
	ec_unit_a(&ec);
	ec.answer_fixed = 1;
	ec.fixed = 0xFF;
	CHECK(bc250_hwmon_sample(&io, &id, 0, &second) == BC250_HWMON_REFUSED);
	CHECK(second.rpm_valid == 0u && second.temperature_valid == 0u && second.duty_valid == 0x1Fu);
	CHECK(ec.writes_other == 0);

	/* The other half of a dead window: every byte 0x00. The tachometers look like stopped fans and the duty
	 * outputs like idle ones, both perfectly legal; the monitor channels read 0x0000, which the conversion
	 * would hand back as 0.0 C. Refusing the raw word is what keeps a dead window from looking cold. */
	ec_unit_a(&ec);
	ec.answer_fixed = 1;
	ec.fixed = 0x00;
	CHECK(bc250_hwmon_sample(&io, &id, 0, &second) == BC250_HWMON_REFUSED);
	CHECK(second.temperature_valid == 0u && second.rpm_valid == 0x1Fu);     /* read, but not a reading */
	CHECK(ec.writes_other == 0);

	/* One dead monitor channel among three is not a dead window. */
	ec_unit_a(&ec);
	ec_put16(&ec, BC250_HWMON_REG_MON(1), BC250_HWMON_MON_NONE);
	CHECK(bc250_hwmon_sample(&io, &id, 0, &second) == 0);
	CHECK(second.temperature_valid == 5u && second.temperature_mc[1] == 0);
	CHECK(second.temperature_mc[0] == 83000 && second.temperature_mc[2] == 59500);
	ec_unit_a(&ec);
	ec_put16(&ec, BC250_HWMON_REG_MON(1), BC250_HWMON_MON_EMPTY);
	CHECK(bc250_hwmon_sample(&io, &id, 0, &second) == 0);
	CHECK(second.temperature_valid == 5u && second.retries == 1u);

	/* A board with no temperature channel at all would still be a reading, on its tachometers alone. No such
	 * board is known; the rule is written so that the refusal above is about the window and not about us. */
	ec_unit_a(&ec);
	{
		struct bc250_hwmon_identity fans_only = id;

		fans_only.temperatures = 0;
		CHECK(bc250_hwmon_sample(&io, &fans_only, 0, &second) == 0);
		CHECK(second.rpm[1] == 1589u && second.temperature_valid == 0u);
	}

	/* An identity that did not pass never samples. */
	ec_unit_a(&ec);
	id.reason = BC250_HWMON_REASON_IDENTITY;
	CHECK(bc250_hwmon_sample(&io, &id, 0, &second) == BC250_HWMON_REFUSED);
	CHECK(ec.reads == 0);
}

int main(void)
{
	sequence();
	allowlist();
	base();
	conversions();
	plausibility();
	identity();
	sampling();
	printf("hardware monitor policy: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
