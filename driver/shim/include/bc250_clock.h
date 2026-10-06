/* Fixed-clock startup transaction. Caller supplies the single serialized SMU owner. */
#ifndef BC250_CLOCK_H
#define BC250_CLOCK_H

struct bc250_clock_io {
    void *context;
    /* begin/end exclude every mailbox client for the entire transaction. A
     * driver-local lock is insufficient if bc250rd still accesses SMU directly. */
    int (*begin)(void *context);
    void (*end)(void *context);
    int (*temperature)(void *context, int *millidegrees_c);
    /* Return zero only after transport and firmware both report success. */
    int (*message)(void *context, unsigned int message, unsigned int parameter,
                   unsigned int *value);
    /* Optional (NULL: re-read at once). Waits at least usec between the settle reads below. */
    void (*delay)(void *context, unsigned int usec);
};
struct bc250_clock_report {
    unsigned int requested_mhz, requested_mv, expected_vid;
    unsigned int initial_mhz, initial_vid, staged_vid, voltage_staged;
    unsigned int observed_mhz, observed_vid;
    unsigned int messages_attempted, messages_completed;
    int temperature_mc, status;
    unsigned int ready;
    unsigned int settle_reads;  /* GetGfxFrequency re-reads while the clock was on its way */
};
#define BC250_CLOCK_INVALID (-22)
#define BC250_CLOCK_TOO_HOT (-1001)
#define BC250_CLOCK_MISMATCH (-1002)
#define BC250_CLOCK_STATE_INVALID (-1003)

/* The operating points this driver may request (docs/design/dpm.md). One table serves the fixed
 * lab point (level BC250_DPM_FLOOR_LEVEL), the governor's steps and the administrator's escape: a point is
 * admitted only on the 100 MHz grid, with a voltage at or above the table's for that clock and at or below
 * the ceiling. Anchors: 1000 MHz / 820 mV, the lab point under Windows since E04 (facts M22);
 * 1500 MHz / 918.75 mV (VID 101), the firmware's own point (facts M22, M47); 2000 MHz / 1000 mV, the
 * owner's DPM ceiling (2026-09-30), 61 mV above the community curve's interpolated 939 mV and 129 mV
 * below amdgpu's overdrive maximum of 1129 mV. Linear between anchors, rounded up to a whole mV.
 * Below the lab floor the table holds five points, 900 down to 500 MHz, all at the floor's 820 mV
 * (VID 116): a lower clock at the voltage the part is known to run at, never an undervolt, and the
 * anchors' line is not extrapolated below its lowest anchor. Two of them, 900 and 800 MHz, are the
 * thermal cap's (0.7.205, owner decision 2026-10-05: a clock under 1000 MHz is allowed when Tctl reaches
 * 87 C). 500 MHz is the idle point (0.7.207, owner decision 2026-10-05: "jak lab nie pracuje, to ustawiaj
 * mu zegar gpu na 500 MHz" - when the lab does not work, set its GPU clock to 500 MHz). 700 and 600 MHz
 * keep the 100 MHz grid whole, so that a level stays (mhz - MIN) / STEP; no rule of the governor selects
 * them, and only an administrator's own request (DpmIdleMHz, the clock escape) reaches them.
 * The load never asks below the lab floor (the governor's BC250_DPM_FLOOR_LEVEL is 1000 MHz).
 * The power tables publish no level below 1000 MHz (facts M47: SCLK levels 1000/1500/2000, and Linux clamps
 * its sysfs there) although unit A's firmware accepts 800 and 900 MHz at VID 116 (facts M785), so the KMD
 * still treats a refused transition under the floor as "this part has no such point" (driver/kmd/dpm.c):
 * the thermal sub-floor or the idle point is withdrawn for the rest of the start. */
#define BC250_CLOCK_MIN_MHZ 500u        /* the table's lowest clock: the idle point, below the lab floor */
#define BC250_CLOCK_FLOOR_MHZ 1000u     /* the lab point, and the lowest clock the load may ask for */
#define BC250_CLOCK_CEILING_MHZ 2000u
#define BC250_CLOCK_STEP_MHZ 100u
#define BC250_CLOCK_LEVELS 16u
#define BC250_CLOCK_FLOOR_MV 820u
#define BC250_CLOCK_CEILING_MV 1000u
#define BC250_CLOCK_HOT_MC 87000     /* no raise of clock or voltage at or above this (owner, 2026-10-01; was 85000) */
/* After the commit the SMU reports the clock on its way to a raised request (unit A, KMD 0.7.176.1: 1028-1029 MHz
 * right after 1000 -> 1200, 1200 one 25 ms governor tick later; lowerings read back exact). While the readback lies
 * between the initial clock and the request, it is read again, at most this often, with this pause before each. */
#define BC250_CLOCK_SETTLE_READS 50u
#define BC250_CLOCK_SETTLE_US 1000u
struct bc250_clock_point { unsigned int mhz, mv, vid; };
extern const struct bc250_clock_point bc250_clock_points[BC250_CLOCK_LEVELS];
/* AMD's SVI2 encoding, as the imported commit computes it. Truncation: the VID's voltage is never
 * below the requested mV. */
static __inline unsigned int bc250_clock_vid(unsigned int mv) { return (1550u - mv) * 160u / 1000u; }
/* Voltage of a VID in microvolts (6.25 mV steps). */
static __inline unsigned int bc250_clock_vid_uv(unsigned int vid) { return 1550000u - vid * 6250u; }
/* The table's voltage for a clock on the grid (BC250_CLOCK_MIN_MHZ..BC250_CLOCK_CEILING_MHZ); 0 for any
 * other clock. */
unsigned int bc250_clock_min_mv(unsigned int mhz);
/* Clock on the grid within the table, voltage between the table's and the ceiling. */
int bc250_clock_point_allowed(unsigned int mhz, unsigned int mv);
/* The only SMU messages the native owner sends; smu.c refuses every other one. */
int bc250_clock_message_allowed(unsigned int message);

/* PASSIVE/sleepable owner only. No allocation, MMIO mapping or async work.
 * Readback matches encoded VID, not an exact analogue voltage claim. The clock readback must end exactly at
 * the request; readings on the way there are re-read (BC250_CLOCK_SETTLE_READS), anything else is MISMATCH.
 * On partial failure do not undo a successful downclock by raising frequency.
 * At or above BC250_CLOCK_HOT_MC a transition that raises clock or voltage is refused (TOO_HOT)
 * after the two readbacks; one that raises neither is carried out, so a hot part can always be
 * clocked down. One exception (0.7.205): a request up to BC250_CLOCK_FLOOR_MHZ that does not raise
 * the voltage is carried out however hot the part is, so that the lab point - the one operating point
 * this part is known to run at - is reachable from the two thermal-only points below it. Without it the
 * fixed start/resume request, the stop and power-down applies and the resync would all fail at a hot part.
 * A clock below CYAN_SKILLFISH_SCLK_MIN (every point under the lab floor) cannot go through the imported
 * commit, which refuses it; the same two messages are then sent directly, in the same order (bc250_clock.c).
 * Caller must prohibit GPU startup unless report.ready and status zero. */
int bc250_clock_prepare(const struct bc250_clock_io *io, unsigned int mhz,
                        unsigned int mv, struct bc250_clock_report *report);
#endif
