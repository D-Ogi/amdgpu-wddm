/* The fan control policy (driver/shim/bc250_fan.c), driven on the host against the model of the chip in
 * hwmon_ec_mock.h, which follows the handshake that M803 measured on unit A.
 *
 * What this test holds the policy to: every write is one of the three admitted registers and lands inside an
 * open phase, the handshake runs in the measured order, the restore record is taken once and never records our own
 * bit as the board's, every exit path leaves the chip at the board's own values, doubt means full speed and then
 * the board, the duty never goes under the floor, the emergency, the slope rule, the lease and the watchdog's
 * hold-back. Against the real driver/shim/bc250_fan.c, not a copy of it.
 *
 *   pwsh driver\shim\test\run_hwmon.ps1
 */
#include <stdio.h>
#include "hwmon_ec_mock.h"
#include "bc250_fan.h"

static int checks, failures;

#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

#define TARGET1 BC250_HWMON_REG_DUTY_WRITE(BC250_FAN_CHANNEL)
#define BIT1 (1u << BC250_FAN_CHANNEL)

/* ---- helpers ------------------------------------------------------------------------------------------------- */

/* Every data-port write of the run went to an admitted register, through the latch, and nothing else was written. */
static int clean_writes(const struct ec_mock *ec)
{
	unsigned int i;

	for (i = 0; i < ec->logged; i++) {
		unsigned int reg = ec->log[i].reg;

		if (!bc250_hwmon_write_allowed(reg))
			return 0;
		if (reg != BC250_HWMON_REG_FAN_CTRL && reg != BC250_HWMON_REG_MODE && reg != TARGET1)
			return 0;
		if (reg == TARGET1 && ec->log[i].value < bc250_fan_pct_to_raw(BC250_FAN_FLOOR_PCT))
			return 0;	/* never under the floor, and never 0 */
	}
	return ec->writes_other == 0 && ec->sequence_errors == 0;
}

/* The chip as the board's own curve runs it: our bit clear, the target back at the board's value. */
static int at_rest(const struct ec_mock *ec, unsigned int target)
{
	return (ec_peek8(ec, BC250_HWMON_REG_MODE) & BIT1) == 0u && ec_peek8(ec, BC250_HWMON_REG_MODE) ==
	       BC250_HWMON_MODE_REST && ec_peek8(ec, TARGET1) == target &&
	       (ec_peek8(ec, BC250_HWMON_REG_ENGINE) & BC250_HWMON_ENGINE_LOCK) != 0u;
}

static unsigned int model_read(struct ec_mock *ec, unsigned int reg)
{
	int live = ec_live(ec, reg);

	return live >= 0 ? (unsigned int)live : ec_peek8(ec, reg);
}

/* One second of inputs as the miniport gathers them: Tctl, the EC's own reading of the same die, the tachometer and
 * the duty read-back of fan 1 out of the model. */
static struct bc250_fan_input input(struct ec_mock *ec, int tctl_mc)
{
	struct bc250_fan_input in;

	memset(&in, 0, sizeof(in));
	in.dt_ms = 1000;
	in.tctl_mc = tctl_mc;
	in.tctl_valid = 1;
	in.tsi_mc = tctl_mc;
	in.tsi_valid = 1;
	in.tsi_mapped = 1;
	in.reader_valid = 1;
	in.rpm = (model_read(ec, BC250_HWMON_REG_FAN(BC250_FAN_CHANNEL)) << 8) |
		 model_read(ec, BC250_HWMON_REG_FAN(BC250_FAN_CHANNEL) + 1u);
	in.rpm_valid = 1;
	in.readback_raw = model_read(ec, BC250_HWMON_REG_DUTY(BC250_FAN_CHANNEL));
	in.readback_valid = 1;
	return in;
}

static int tick_at(struct bc250_hwmon_io *io, struct ec_mock *ec, struct bc250_fan_ctl *ctl, int mc)
{
	struct bc250_fan_input in = input(ec, mc);

	return bc250_fan_tick(io, ctl, &in);
}

static void start(struct ec_mock *ec, struct bc250_hwmon_io *io, struct bc250_fan_ctl *ctl)
{
	ec_unit_a_m803(ec);
	ec_io_write(io, ec);
	memset(ctl, 0, sizeof(*ctl));
	bc250_fan_init(ctl, 1, NULL);
}

/* ---- curves -------------------------------------------------------------------------------------------------- */

static void curves(void)
{
	struct bc250_fan_curve c;
	unsigned int p;

	for (p = BC250_FAN_PROFILE_STANDARD; p < BC250_FAN_PROFILE_COUNT; p++) {
		CHECK(bc250_fan_profile_curve(p, &c) == BC250_FAN_ERROR_OK);
		CHECK(bc250_fan_curve_check(&c) == BC250_FAN_ERROR_OK);
		/* Every preset reaches full speed before the DPM's hot step and the emergency at 87 C. */
		CHECK(bc250_fan_curve_eval(&c, 86000) == 100u);
	}
	CHECK(bc250_fan_profile_curve(BC250_FAN_PROFILE_CUSTOM, &c) == BC250_FAN_ERROR_PROFILE);
	CHECK(bc250_fan_profile_curve(BC250_FAN_PROFILE_COUNT, &c) == BC250_FAN_ERROR_PROFILE);

	/* The default never runs the fan slower than the BIOS Standard Mode did at the four points unit A measured:
	 * 65 C 65 % and 69 C 77 % (b20 read trial), 70 C 80 % (M803 at rest), 83 C 96 % (E01). */
	CHECK(bc250_fan_profile_curve(BC250_FAN_PROFILE_STANDARD, &c) == 0);
	CHECK(bc250_fan_curve_eval(&c, 65000) >= 65u && bc250_fan_curve_eval(&c, 65000) == 76u);
	CHECK(bc250_fan_curve_eval(&c, 69000) >= 77u);
	CHECK(bc250_fan_curve_eval(&c, 70000) >= 80u && bc250_fan_curve_eval(&c, 70000) == 82u);
	CHECK(bc250_fan_curve_eval(&c, 83000) >= 96u);
	/* At 80 C at or above the 1360..1590 RPM the EC held (95 % is about 1630 RPM at 1720 RPM full), and 100 % by 85 C. */
	CHECK(bc250_fan_curve_eval(&c, 80000) == 95u);
	CHECK(bc250_fan_curve_eval(&c, 85000) == 100u && bc250_fan_curve_eval(&c, 84000) == 99u);
	/* Flat at both ends, never below the floor. */
	CHECK(bc250_fan_curve_eval(&c, 20000) == 50u && bc250_fan_curve_eval(&c, -5000) == 50u);
	CHECK(bc250_fan_curve_eval(&c, 120000) == 100u);
	/* Rounded up between two points: 60.1 C is 70.12 % on the line, 71 % from the evaluator. */
	CHECK(bc250_fan_curve_eval(&c, 60100) == 71u);

	/* The checks of a custom curve. */
	memset(&c, 0, sizeof(c));
	c.points = 1; c.p[0].c = 50; c.p[0].pct = 50;
	CHECK(bc250_fan_curve_check(&c) == BC250_FAN_ERROR_POINTS);
	c.points = 9;
	CHECK(bc250_fan_curve_check(&c) == BC250_FAN_ERROR_POINTS);
	c.points = 2; c.p[1].c = 50; c.p[1].pct = 60;
	CHECK(bc250_fan_curve_check(&c) == BC250_FAN_ERROR_TEMPERATURE);	/* temperatures must rise */
	c.p[1].c = 96;
	CHECK(bc250_fan_curve_check(&c) == BC250_FAN_ERROR_TEMPERATURE);
	c.p[1].c = 70; c.p[0].c = 19;
	CHECK(bc250_fan_curve_check(&c) == BC250_FAN_ERROR_TEMPERATURE);
	c.p[0].c = 40; c.p[1].pct = 40;
	CHECK(bc250_fan_curve_check(&c) == BC250_FAN_ERROR_DUTY);		/* duties never fall */
	c.p[1].pct = 101;
	CHECK(bc250_fan_curve_check(&c) == BC250_FAN_ERROR_DUTY);
	c.p[1].pct = 60; c.p[0].pct = 19;
	CHECK(bc250_fan_curve_check(&c) == BC250_FAN_ERROR_DUTY);		/* the floor */
	c.p[0].pct = 20;
	CHECK(bc250_fan_curve_check(&c) == BC250_FAN_ERROR_OK);

	CHECK(bc250_fan_pct_to_raw(100) == 255u && bc250_fan_pct_to_raw(40) == 102u && bc250_fan_pct_to_raw(20) == 51u);
	CHECK(bc250_fan_pct_to_raw(150) == 255u);
	CHECK(bc250_fan_raw_to_pct(255) == 100u && bc250_fan_raw_to_pct(204) == 80u && bc250_fan_raw_to_pct(102) == 40u);
}

static void requests(void)
{
	struct bc250_fan_request r;
	struct bc250_fan_curve c;
	struct bc250_fan_ctl ctl;

	memset(&r, 0, sizeof(r));
	r.mode = BC250_FAN_MODE_BOARD;
	CHECK(bc250_fan_request_check(&r, &c) == BC250_FAN_ERROR_OK);
	r.mode = BC250_FAN_MODE_COUNT;
	CHECK(bc250_fan_request_check(&r, &c) == BC250_FAN_ERROR_MODE);
	r.mode = BC250_FAN_MODE_FIXED;
	r.fixed_pct = 60;
	CHECK(bc250_fan_request_check(&r, &c) == BC250_FAN_ERROR_LEASE);	/* a fixed duty is never durable */
	r.lease_ms = 4999;
	CHECK(bc250_fan_request_check(&r, &c) == BC250_FAN_ERROR_LEASE);
	r.lease_ms = 300001;
	CHECK(bc250_fan_request_check(&r, &c) == BC250_FAN_ERROR_LEASE);
	r.lease_ms = 30000;
	CHECK(bc250_fan_request_check(&r, &c) == BC250_FAN_ERROR_OK);
	r.fixed_pct = 19;
	CHECK(bc250_fan_request_check(&r, &c) == BC250_FAN_ERROR_DUTY);	/* the floor holds for a fixed duty too */
	r.fixed_pct = 0;
	CHECK(bc250_fan_request_check(&r, &c) == BC250_FAN_ERROR_DUTY);
	memset(&r, 0, sizeof(r));
	r.mode = BC250_FAN_MODE_CURVE;
	r.profile = BC250_FAN_PROFILE_COUNT;
	CHECK(bc250_fan_request_check(&r, &c) == BC250_FAN_ERROR_PROFILE);
	r.profile = BC250_FAN_PROFILE_QUIET;
	CHECK(bc250_fan_request_check(&r, &c) == BC250_FAN_ERROR_OK && c.points == 5u);
	r.profile = BC250_FAN_PROFILE_CUSTOM;
	CHECK(bc250_fan_request_check(&r, &c) == BC250_FAN_ERROR_POINTS && c.points == 0u);

	/* A start never runs a leased or a fixed mode, and a stored curve that fails the check is the default. */
	memset(&ctl, 0, sizeof(ctl));
	r.mode = BC250_FAN_MODE_FIXED; r.fixed_pct = 50; r.lease_ms = 30000;
	bc250_fan_init(&ctl, 1, &r);
	CHECK(ctl.mode == BC250_FAN_MODE_CURVE && ctl.profile == BC250_FAN_PROFILE_STANDARD && ctl.lease_ms == 0u);
	memset(&r, 0, sizeof(r));
	r.mode = BC250_FAN_MODE_CURVE; r.profile = BC250_FAN_PROFILE_CUSTOM; r.curve.points = 1;
	bc250_fan_init(&ctl, 1, &r);
	CHECK(ctl.profile == BC250_FAN_PROFILE_STANDARD && ctl.curve.points == 5u);
	r.mode = BC250_FAN_MODE_BOARD;
	bc250_fan_init(&ctl, 1, &r);
	CHECK(ctl.mode == BC250_FAN_MODE_BOARD && ctl.state == BC250_FAN_STATE_BOARD);
	bc250_fan_init(&ctl, 0, NULL);
	CHECK(ctl.state == BC250_FAN_STATE_OFF);
}

/* ---- the handshake and the take-over ------------------------------------------------------------------------- */

static void handshake(void)
{
	struct ec_mock ec;
	struct bc250_hwmon_io io;
	struct bc250_fan_ctl ctl;
	unsigned int raw = bc250_fan_pct_to_raw(82);

	start(&ec, &io, &ctl);
	/* The first tick at 70 C takes the fan: one phase, the record, our bit, the target, the close. */
	CHECK(tick_at(&io, &ec, &ctl, 70000) == 0);
	CHECK(ctl.controlling && ctl.state == BC250_FAN_STATE_CURVE && ctl.takeovers == 1u);
	CHECK(ec.logged == 4u);
	CHECK(ec.log[0].reg == BC250_HWMON_REG_FAN_CTRL && ec.log[0].value == BC250_HWMON_FAN_CFG_REQUEST);
	CHECK(ec.log[1].reg == BC250_HWMON_REG_MODE && ec.log[1].value == (BC250_HWMON_MODE_REST | BIT1));
	CHECK(ec.log[2].reg == TARGET1 && ec.log[2].value == raw);
	CHECK(ec.log[3].reg == BC250_HWMON_REG_FAN_CTRL && ec.log[3].value == BC250_HWMON_FAN_CFG_DONE);
	CHECK(ec.protocol_errors == 0 && clean_writes(&ec));
	CHECK(ctl.restore.valid && ctl.restore.mode == BC250_HWMON_MODE_REST &&
	      ctl.restore.target == BC250_HWMON_TARGET_REST && !ctl.restore.substituted);
	CHECK(ec_peek8(&ec, BC250_HWMON_REG_ENGINE) == (BC250_HWMON_ENGINE_CHECK_DONE | BC250_HWMON_ENGINE_LOCK));
	/* The fan now follows our duty: the read-back is the target, the tachometer is the model's speed. */
	CHECK(model_read(&ec, BC250_HWMON_REG_DUTY(BC250_FAN_CHANNEL)) == raw);

	/* The same duty next second writes nothing. A rise writes the target alone, in its own phase. */
	CHECK(tick_at(&io, &ec, &ctl, 70000) == 0 && ec.logged == 4u);
	CHECK(tick_at(&io, &ec, &ctl, 80000) == 0);
	CHECK(ec.logged == 7u && ec.log[4].value == BC250_HWMON_FAN_CFG_REQUEST && ec.log[5].reg == TARGET1 &&
	      ec.log[5].value == bc250_fan_pct_to_raw(95) && ec.log[6].value == BC250_HWMON_FAN_CFG_DONE);

	/* The handback: the target first, then the mode with our bit clear, in one phase (rule 3). */
	CHECK(bc250_fan_handback(&io, &ctl, BC250_FAN_REASON_USER) == 0);
	CHECK(ec.logged == 11u);
	CHECK(ec.log[7].value == BC250_HWMON_FAN_CFG_REQUEST);
	CHECK(ec.log[8].reg == TARGET1 && ec.log[8].value == BC250_HWMON_TARGET_REST);
	CHECK(ec.log[9].reg == BC250_HWMON_REG_MODE && ec.log[9].value == BC250_HWMON_MODE_REST);
	CHECK(ec.log[10].value == BC250_HWMON_FAN_CFG_DONE);
	CHECK(at_rest(&ec, BC250_HWMON_TARGET_REST) && !ctl.controlling && ctl.handbacks == 1u);
	CHECK(model_read(&ec, BC250_HWMON_REG_DUTY(BC250_FAN_CHANNEL)) == 204u);	/* the EC curve again */
	CHECK(ec.protocol_errors == 0 && clean_writes(&ec));

	/* The open waits for a pending request to clear before it adds its own, and a slow engine is waited for. */
	start(&ec, &io, &ctl);
	ec.open_delay = 30;
	ec.close_delay = 40;
	ec_put8(&ec, BC250_HWMON_REG_FAN_CTRL, BC250_HWMON_FAN_CFG_REQUEST);
	CHECK(bc250_fan_open(&io) == BC250_FAN_E_BUSY && ec.logged == 0u);	/* someone else's request never clears */
	ec_put8(&ec, BC250_HWMON_REG_FAN_CTRL, 0);
	CHECK(bc250_fan_open(&io) == 0 && bc250_fan_close(&io) == 0);
	CHECK(ec.logged == 2u && clean_writes(&ec));
}

static void restore_record(void)
{
	struct ec_mock ec;
	struct bc250_hwmon_io io;
	struct bc250_fan_ctl ctl;

	/* A previous driver died holding the fan: our bit is set and its duty is in the target. Neither is recorded as
	 * the board's. The record holds the M803 rest values, and the handback writes those. */
	start(&ec, &io, &ctl);
	ec_put8(&ec, BC250_HWMON_REG_MODE, BC250_HWMON_MODE_REST | BIT1);
	ec_put8(&ec, TARGET1, 77);
	CHECK(tick_at(&io, &ec, &ctl, 60000) == 0 && ctl.controlling);
	CHECK(ctl.restore.valid && ctl.restore.substituted && ctl.restore.mode == BC250_HWMON_MODE_REST &&
	      ctl.restore.target == BC250_HWMON_TARGET_REST);
	CHECK(bc250_fan_handback(&io, &ctl, BC250_FAN_REASON_STOP) == 0 && at_rest(&ec, BC250_HWMON_TARGET_REST));

	/* Once per power-on: a stop and a start keep the record, so a second take does not read our own target. */
	start(&ec, &io, &ctl);
	ec_put8(&ec, TARGET1, 140);			/* the board's own target on this chip */
	CHECK(tick_at(&io, &ec, &ctl, 60000) == 0 && ctl.restore.target == 140u);
	CHECK(bc250_fan_handback(&io, &ctl, BC250_FAN_REASON_STOP) == 0 && at_rest(&ec, 140u));
	bc250_fan_init(&ctl, 1, NULL);			/* the next device start */
	CHECK(ctl.restore.valid && ctl.restore.target == 140u);
	ec_put8(&ec, TARGET1, 99);			/* whatever the chip holds now is not read again */
	CHECK(tick_at(&io, &ec, &ctl, 60000) == 0 && ctl.restore.target == 140u);
	CHECK(bc250_fan_handback(&io, &ctl, BC250_FAN_REASON_STOP) == 0 && at_rest(&ec, 140u));
	CHECK(clean_writes(&ec) && ec.protocol_errors == 0);
}

/* ---- every exit path ----------------------------------------------------------------------------------------- */

static void exit_paths(void)
{
	static const unsigned int direct[] = { BC250_FAN_REASON_STOP, BC250_FAN_REASON_POWER,
					       BC250_FAN_REASON_UNLOAD, BC250_FAN_REASON_WATCHDOG };
	struct ec_mock ec;
	struct bc250_hwmon_io io;
	struct bc250_fan_ctl ctl;
	struct bc250_fan_request r;
	unsigned int i;

	/* The miniport's own paths: device stop and remove, a transition out of D0, unload, the watchdog. Each one is
	 * the same call, and each one must leave the chip at rest. */
	for (i = 0; i < sizeof(direct) / sizeof(direct[0]); i++) {
		start(&ec, &io, &ctl);
		CHECK(tick_at(&io, &ec, &ctl, 75000) == 0 && ctl.controlling);
		CHECK(bc250_fan_handback(&io, &ctl, direct[i]) == 0);
		CHECK(at_rest(&ec, BC250_HWMON_TARGET_REST) && !ctl.controlling && ctl.reason == direct[i]);
		CHECK(ctl.state == BC250_FAN_STATE_BOARD && clean_writes(&ec) && ec.protocol_errors == 0);
		/* A second exit path after the first writes nothing. */
		ec.logged = 0;
		CHECK(bc250_fan_handback(&io, &ctl, direct[i]) == 0 && ec.logged == 0u);
	}

	/* The user asks for the board's own curve. */
	start(&ec, &io, &ctl);
	CHECK(tick_at(&io, &ec, &ctl, 75000) == 0 && ctl.controlling);
	memset(&r, 0, sizeof(r));
	r.mode = BC250_FAN_MODE_BOARD;
	CHECK(bc250_fan_set(&ctl, &r) == BC250_FAN_ERROR_OK);
	CHECK(tick_at(&io, &ec, &ctl, 75000) == 0);
	CHECK(at_rest(&ec, BC250_HWMON_TARGET_REST) && ctl.reason == BC250_FAN_REASON_USER &&
	      ctl.state == BC250_FAN_STATE_BOARD);
	ec.logged = 0;
	CHECK(tick_at(&io, &ec, &ctl, 90000) == 0 && ec.logged == 0u);	/* the board's choice: no emergency write */

	/* The control is switched off while the driver holds the fan. */
	start(&ec, &io, &ctl);
	CHECK(tick_at(&io, &ec, &ctl, 75000) == 0 && ctl.controlling);
	ctl.enabled = 0;
	CHECK(tick_at(&io, &ec, &ctl, 75000) == 0);
	CHECK(at_rest(&ec, BC250_HWMON_TARGET_REST) && ctl.reason == BC250_FAN_REASON_DISABLED &&
	      ctl.state == BC250_FAN_STATE_OFF);

	/* The bugcheck path: no lock (the transport has none), a bounded poll, no verification. */
	start(&ec, &io, &ctl);
	CHECK(tick_at(&io, &ec, &ctl, 75000) == 0 && ctl.controlling);
	io.lock = NULL;
	io.unlock = NULL;
	ec.logged = 0;
	{
		int holds = ec.holds;

		bc250_fan_handback_blind(&io, &ctl);
		CHECK(ec.holds == holds);		/* not one lock was taken */
	}
	CHECK(!ctl.controlling && ctl.reason == BC250_FAN_REASON_BUGCHECK);
	CHECK(ec.logged == 4u && ec.log[0].value == BC250_HWMON_FAN_CFG_REQUEST && ec.log[1].reg == TARGET1 &&
	      ec.log[2].reg == BC250_HWMON_REG_MODE && ec.log[3].value == BC250_HWMON_FAN_CFG_DONE);
	/* The model closes the phase after its close delay, i.e. at the next polls of the engine. */
	(void)model_read(&ec, BC250_HWMON_REG_ENGINE);
	(void)model_read(&ec, BC250_HWMON_REG_ENGINE);
	(void)model_read(&ec, BC250_HWMON_REG_ENGINE);
	(void)model_read(&ec, BC250_HWMON_REG_ENGINE);
	CHECK(at_rest(&ec, BC250_HWMON_TARGET_REST) && clean_writes(&ec) && ec.protocol_errors == 0);
	/* And on a fan the driver does not hold, the bugcheck path writes nothing at all. */
	ec.logged = 0;
	bc250_fan_handback_blind(&io, &ctl);
	CHECK(ec.logged == 0u);
	/* An engine that never grants the phase costs the bugcheck path at most its 20 polls. */
	start(&ec, &io, &ctl);
	CHECK(tick_at(&io, &ec, &ctl, 75000) == 0);
	ec.stuck_open = 1;
	io.lock = NULL;
	io.unlock = NULL;
	{
		unsigned int before = ec.engine_reads;

		bc250_fan_handback_blind(&io, &ctl);
		CHECK(ec.engine_reads - before <= BC250_FAN_BLIND_POLL_MAX);
	}
}

/* ---- leases -------------------------------------------------------------------------------------------------- */

static void leases(void)
{
	struct ec_mock ec;
	struct bc250_hwmon_io io;
	struct bc250_fan_ctl ctl;
	struct bc250_fan_request r;
	unsigned int i;

	start(&ec, &io, &ctl);
	memset(&r, 0, sizeof(r));
	r.mode = BC250_FAN_MODE_FIXED;
	r.fixed_pct = 40;
	r.lease_ms = 5000;
	CHECK(bc250_fan_set(&ctl, &r) == BC250_FAN_ERROR_OK);
	CHECK(tick_at(&io, &ec, &ctl, 60000) == 0 && ctl.state == BC250_FAN_STATE_FIXED);
	CHECK(ec_peek8(&ec, TARGET1) == 102u);		/* the M803 duty: 771..774 RPM on unit A */
	/* A renewal restarts the lease; a renewal of a durable mode is refused. */
	CHECK(tick_at(&io, &ec, &ctl, 60000) == 0 && ctl.lease_ms == 3000u);
	CHECK(bc250_fan_renew(&ctl, 5000) == 0 && ctl.lease_ms == 5000u);
	CHECK(bc250_fan_renew(&ctl, 1000) != 0);
	for (i = 0; i < 4u; i++)
		CHECK(tick_at(&io, &ec, &ctl, 60000) == 0 && ctl.controlling);
	/* The lease runs out: the board gets the fan back and keeps it. */
	CHECK(tick_at(&io, &ec, &ctl, 60000) == 0);
	CHECK(!ctl.controlling && ctl.reason == BC250_FAN_REASON_LEASE && ctl.mode == BC250_FAN_MODE_BOARD);
	CHECK(ctl.lease_expiries == 1u && at_rest(&ec, BC250_HWMON_TARGET_REST));
	ec.logged = 0;
	for (i = 0; i < 40u; i++)
		CHECK(tick_at(&io, &ec, &ctl, 60000) == 0);
	CHECK(ec.logged == 0u && !ctl.controlling);
	/* A leased curve ends the same way; a durable one has nothing to renew. */
	memset(&r, 0, sizeof(r));
	r.mode = BC250_FAN_MODE_CURVE;
	r.profile = BC250_FAN_PROFILE_PERFORMANCE;
	r.lease_ms = 5000;
	CHECK(bc250_fan_set(&ctl, &r) == BC250_FAN_ERROR_OK);
	for (i = 0; i < 5u; i++)
		CHECK(tick_at(&io, &ec, &ctl, 60000) == 0);
	CHECK(!ctl.controlling && ctl.reason == BC250_FAN_REASON_LEASE);
	r.lease_ms = 0;
	CHECK(bc250_fan_set(&ctl, &r) == BC250_FAN_ERROR_OK && bc250_fan_renew(&ctl, 5000) != 0);
	CHECK(clean_writes(&ec) && ec.protocol_errors == 0);
}

/* ---- doubt --------------------------------------------------------------------------------------------------- */

static void doubt(void)
{
	struct ec_mock ec;
	struct bc250_hwmon_io io;
	struct bc250_fan_ctl ctl;
	struct bc250_fan_input in;
	unsigned int i;

	/* No Tctl: full speed at once, and the board after 5 s of it. */
	start(&ec, &io, &ctl);
	CHECK(tick_at(&io, &ec, &ctl, 60000) == 0 && ec_peek8(&ec, TARGET1) == bc250_fan_pct_to_raw(70));
	for (i = 0; i < 4u; i++) {
		in = input(&ec, 60000);
		in.tctl_valid = 0;
		CHECK(bc250_fan_tick(&io, &ctl, &in) == 0);
		CHECK(ctl.state == BC250_FAN_STATE_DOUBT && ec_peek8(&ec, TARGET1) == 255u && ctl.controlling);
	}
	in = input(&ec, 60000);
	in.tctl_valid = 0;
	CHECK(bc250_fan_tick(&io, &ctl, &in) == 0);
	CHECK(!ctl.controlling && ctl.reason == BC250_FAN_REASON_TEMPERATURE && ctl.held_back);
	CHECK(at_rest(&ec, BC250_HWMON_TARGET_REST) && ctl.doubts == 1u);
	/* Clean inputs for 30 s, and the driver takes the fan again; not one second earlier. */
	for (i = 0; i < 29u; i++)
		CHECK(tick_at(&io, &ec, &ctl, 60000) == 0 && !ctl.controlling && ctl.state == BC250_FAN_STATE_BOARD);
	CHECK(tick_at(&io, &ec, &ctl, 60000) == 0 && ctl.controlling && ctl.retakes == 1u);

	/* Tctl and the EC's own reading 12 C apart: a doubt, and 100 %. */
	in = input(&ec, 60000);
	in.tsi_mc = 72000;
	CHECK(bc250_fan_tick(&io, &ctl, &in) == 0 && ctl.doubt == BC250_FAN_REASON_TEMPERATURE &&
	      ec_peek8(&ec, TARGET1) == 255u);
	/* 9 C apart is believed, and the guard is the hotter one. */
	in = input(&ec, 60000);
	in.tsi_mc = 69000;
	CHECK(bc250_fan_tick(&io, &ctl, &in) == 0 && ctl.doubt == 0u && ctl.guard_mc == 69000);
	/* One refused SB-TSI value is a collision on an unarbitrated window: Tctl carries the tick. */
	in = input(&ec, 60000);
	in.tsi_valid = 0;
	CHECK(bc250_fan_tick(&io, &ctl, &in) == 0 && ctl.doubt == 0u && ctl.guard_mc == 60000);
	/* A stale reader is a doubt. */
	in = input(&ec, 60000);
	in.reader_valid = 0;
	CHECK(bc250_fan_tick(&io, &ctl, &in) == 0 && ctl.doubt == BC250_FAN_REASON_READER &&
	      ec_peek8(&ec, TARGET1) == 255u);

	/* A driver that does not hold the fan does not take it on a doubt. */
	start(&ec, &io, &ctl);
	in = input(&ec, 60000);
	in.tctl_valid = 0;
	CHECK(bc250_fan_tick(&io, &ctl, &in) == 0 && !ctl.controlling && ec.logged == 0u);

	/* The retake budget: after three, the board keeps the fan for the rest of the start. */
	start(&ec, &io, &ctl);
	ctl.retakes = BC250_FAN_RETAKES_MAX;
	ctl.held_back = 1;
	for (i = 0; i < 60u; i++)
		CHECK(tick_at(&io, &ec, &ctl, 60000) == 0);
	CHECK(!ctl.controlling && ec.logged == 0u);
	CHECK(clean_writes(&ec));
}

/* ---- the chip refuses ---------------------------------------------------------------------------------------- */

static void refusals(void)
{
	struct ec_mock ec;
	struct bc250_hwmon_io io;
	struct bc250_fan_ctl ctl;
	unsigned int i;

	/* The phase is never granted: the request went in and nothing else. No hold of the fan, a fault, and no
	 * further write in this start. */
	start(&ec, &io, &ctl);
	ec.stuck_open = 1;
	CHECK(tick_at(&io, &ec, &ctl, 70000) == BC250_FAN_E_OPEN);
	CHECK(!ctl.controlling && ctl.fault && ctl.state == BC250_FAN_STATE_FAULT && ctl.reason == BC250_FAN_REASON_HANDSHAKE);
	CHECK(ec.logged == 1u && ec.log[0].value == BC250_HWMON_FAN_CFG_REQUEST);
	ec.stuck_open = 0;
	for (i = 0; i < 30u; i++)
		CHECK(tick_at(&io, &ec, &ctl, 70000) == 0);
	CHECK(ec.logged == 1u && ctl.state == BC250_FAN_STATE_FAULT);
	CHECK(ec.engine_reads <= BC250_FAN_POLL_MAX + 2u);	/* the poll is bounded */

	/* The close never completes: the fan may be ours, so `controlling` stays and the restore is tried again
	 * every 10 s until it succeeds. */
	start(&ec, &io, &ctl);
	ec.stuck_close = 1;
	CHECK(tick_at(&io, &ec, &ctl, 70000) == BC250_FAN_E_CLOSE);
	CHECK(ctl.controlling && ctl.fault && ctl.state == BC250_FAN_STATE_FAULT);
	ec.stuck_close = 0;
	for (i = 0; i < 9u; i++)
		CHECK(tick_at(&io, &ec, &ctl, 70000) == 0 && ctl.controlling);
	CHECK(tick_at(&io, &ec, &ctl, 70000) == 0 && !ctl.controlling);
	CHECK(at_rest(&ec, BC250_HWMON_TARGET_REST) && ctl.state == BC250_FAN_STATE_FAULT);

	/* The chip rejects the configuration; LOCK stays clear; the target does not land; the bit does not stick. */
	{
		static const int expect[] = { BC250_FAN_E_INVALID, BC250_FAN_E_UNLOCKED, BC250_FAN_E_VERIFY, BC250_FAN_E_MODE };
		static const unsigned int reason[] = { BC250_FAN_REASON_HANDSHAKE, BC250_FAN_REASON_HANDSHAKE,
						       BC250_FAN_REASON_READBACK, BC250_FAN_REASON_MODE };

		for (i = 0; i < 4u; i++) {
			start(&ec, &io, &ctl);
			ec.invalid_on_close = i == 0u;
			ec.unlocked_on_close = i == 1u;
			ec.ignore_target = i == 2u;
			ec.drop_mode_at_close = i == 3u;
			CHECK(tick_at(&io, &ec, &ctl, 70000) == expect[i]);
			CHECK(ctl.fault && ctl.state == BC250_FAN_STATE_FAULT && ctl.failures >= 1u);
			/* The way back ran at once, and the reason names the first refusal. */
			CHECK(ec_writes_to(&ec, BC250_HWMON_REG_FAN_CTRL) >= 4u);
			CHECK(ctl.reason == reason[i] || ctl.controlling);
			CHECK(clean_writes(&ec) && ec.protocol_errors == 0);
		}
	}

	/* The duty read-back does not follow the target: the board, and a fault. */
	start(&ec, &io, &ctl);
	ec.readback_stuck = 1;
	CHECK(tick_at(&io, &ec, &ctl, 60000) == 0 && ctl.controlling);
	CHECK(tick_at(&io, &ec, &ctl, 60000) == 0 && ctl.controlling);		/* 1 s: not judged yet */
	CHECK(tick_at(&io, &ec, &ctl, 60000) == BC250_FAN_E_VERIFY);
	CHECK(!ctl.controlling && ctl.fault && ctl.reason == BC250_FAN_REASON_READBACK);
	CHECK(at_rest(&ec, BC250_HWMON_TARGET_REST));

	/* The fan stops with a duty at or above the floor: full speed first, then the board, and a fault. */
	start(&ec, &io, &ctl);
	CHECK(tick_at(&io, &ec, &ctl, 60000) == 0);
	ec.fan_stopped = 1;
	CHECK(tick_at(&io, &ec, &ctl, 60000) == 0 && ctl.controlling);
	CHECK(tick_at(&io, &ec, &ctl, 60000) == 0 && ctl.controlling);	/* sample 1 */
	CHECK(tick_at(&io, &ec, &ctl, 60000) == 0 && ctl.controlling);	/* sample 2 */
	ec.logged = 0;
	CHECK(tick_at(&io, &ec, &ctl, 60000) == 0);				/* sample 3 */
	CHECK(!ctl.controlling && ctl.fault && ctl.reason == BC250_FAN_REASON_STOPPED);
	CHECK(ec_writes_to(&ec, TARGET1) == 2u && ec.log[1].value == 255u);	/* 100 %, then the board's target */
	CHECK(at_rest(&ec, BC250_HWMON_TARGET_REST) && clean_writes(&ec));
}

/* ---- the slope rule, the hysteresis and the emergency -------------------------------------------------------- */

static void slope_rule(void)
{
	struct ec_mock ec;
	struct bc250_hwmon_io io;
	struct bc250_fan_ctl ctl;
	unsigned int i, last;

	start(&ec, &io, &ctl);
	CHECK(tick_at(&io, &ec, &ctl, 80000) == 0 && ctl.applied_pct == 95u);
	/* Down to 50 C: the curve's input stops 3 C above it, at 53 C, where the curve asks for 63 %. The output holds
	 * 95 % for 10 s. */
	for (i = 0; i < 9u; i++)
		CHECK(tick_at(&io, &ec, &ctl, 50000) == 0 && ctl.applied_pct == 95u);
	CHECK(ctl.target_pct == bc250_fan_curve_eval(&ctl.curve, 53000));
	/* Then 10 points every 2 s, never past the target. */
	CHECK(tick_at(&io, &ec, &ctl, 50000) == 0 && ctl.applied_pct == 85u);
	CHECK(tick_at(&io, &ec, &ctl, 50000) == 0 && ctl.applied_pct == 85u);
	CHECK(tick_at(&io, &ec, &ctl, 50000) == 0 && ctl.applied_pct == 75u);
	CHECK(tick_at(&io, &ec, &ctl, 50000) == 0 && ctl.applied_pct == 75u);
	CHECK(tick_at(&io, &ec, &ctl, 50000) == 0 && ctl.applied_pct == 65u);
	CHECK(tick_at(&io, &ec, &ctl, 50000) == 0 && ctl.applied_pct == 65u);
	CHECK(tick_at(&io, &ec, &ctl, 50000) == 0 && ctl.applied_pct == ctl.target_pct);
	last = ctl.applied_pct;
	/* A rise is taken at once. */
	CHECK(tick_at(&io, &ec, &ctl, 84000) == 0 && ctl.applied_pct == bc250_fan_curve_eval(&ctl.curve, 84000));
	CHECK(ctl.applied_pct > last);

	/* The hysteresis: a wobble of less than 3 C does not move the curve's input down. */
	start(&ec, &io, &ctl);
	CHECK(tick_at(&io, &ec, &ctl, 70000) == 0);
	for (i = 0; i < 30u; i++)
		CHECK(tick_at(&io, &ec, &ctl, i % 2u ? 70000 : 67500) == 0 && ctl.target_pct == 82u);
	CHECK(ec.logged == 4u);		/* one take-over and nothing after it */
	CHECK(clean_writes(&ec));
}

static void emergency(void)
{
	struct ec_mock ec;
	struct bc250_hwmon_io io;
	struct bc250_fan_ctl ctl;
	struct bc250_fan_request r;
	unsigned int i;

	/* A quiet custom curve: 30 % at 86 C. */
	start(&ec, &io, &ctl);
	memset(&r, 0, sizeof(r));
	r.mode = BC250_FAN_MODE_CURVE;
	r.profile = BC250_FAN_PROFILE_CUSTOM;
	r.curve.points = 2;
	r.curve.p[0].c = 40; r.curve.p[0].pct = 20;
	r.curve.p[1].c = 90; r.curve.p[1].pct = 30;
	CHECK(bc250_fan_set(&ctl, &r) == BC250_FAN_ERROR_OK);
	CHECK(tick_at(&io, &ec, &ctl, 86000) == 0 && ctl.applied_pct == 30u && !ctl.emergency);
	/* 87 C: 100 % at once, whatever the curve says. */
	CHECK(tick_at(&io, &ec, &ctl, 87000) == 0 && ctl.state == BC250_FAN_STATE_EMERGENCY);
	CHECK(ec_peek8(&ec, TARGET1) == 255u && ctl.emergencies == 1u);
	/* Above 82 C it holds, however long. */
	for (i = 0; i < 30u; i++)
		CHECK(tick_at(&io, &ec, &ctl, 83000) == 0 && ec_peek8(&ec, TARGET1) == 255u);
	/* At or below 82 C for 10 s it ends, and then the output falls by the slope rule, not at once. */
	for (i = 0; i < 9u; i++)
		CHECK(tick_at(&io, &ec, &ctl, 82000) == 0 && ctl.emergency);
	CHECK(tick_at(&io, &ec, &ctl, 82000) == 0 && !ctl.emergency && ctl.applied_pct == 100u);
	CHECK(ctl.state == BC250_FAN_STATE_CURVE);
	/* A fixed duty yields to the emergency as well. */
	r.mode = BC250_FAN_MODE_FIXED;
	r.fixed_pct = 30;
	r.lease_ms = 60000;
	CHECK(bc250_fan_set(&ctl, &r) == BC250_FAN_ERROR_OK);
	CHECK(tick_at(&io, &ec, &ctl, 60000) == 0 && ec_peek8(&ec, TARGET1) == bc250_fan_pct_to_raw(30));
	CHECK(tick_at(&io, &ec, &ctl, 88000) == 0 && ec_peek8(&ec, TARGET1) == 255u);
	CHECK(clean_writes(&ec) && ec.protocol_errors == 0);
}

static void disabled(void)
{
	struct ec_mock ec;
	struct bc250_hwmon_io io;
	struct bc250_fan_ctl ctl;
	unsigned int i;

	/* EnableFanControl 0: the read path of Part A and nothing else, at every temperature. */
	ec_unit_a_m803(&ec);
	ec_io_write(&io, &ec);
	memset(&ctl, 0, sizeof(ctl));
	bc250_fan_init(&ctl, 0, NULL);
	for (i = 0; i < 20u; i++)
		CHECK(tick_at(&io, &ec, &ctl, 60000 + (int)i * 2000) == 0);
	CHECK(ec.logged == 0u && ec.writes_latch == 0u && ctl.state == BC250_FAN_STATE_OFF);
}

int main(void)
{
	curves();
	requests();
	handshake();
	restore_record();
	exit_paths();
	leases();
	doubt();
	refusals();
	slope_rule();
	emergency();
	disabled();
	printf("fan control: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
