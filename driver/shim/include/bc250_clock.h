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
};
struct bc250_clock_report {
    unsigned int requested_mhz, requested_mv, expected_vid;
    unsigned int initial_mhz, initial_vid, staged_vid, voltage_staged;
    unsigned int observed_mhz, observed_vid;
    unsigned int messages_attempted, messages_completed;
    int temperature_mc, status;
    unsigned int ready;
};
#define BC250_CLOCK_INVALID (-22)
#define BC250_CLOCK_TOO_HOT (-1001)
#define BC250_CLOCK_MISMATCH (-1002)
#define BC250_CLOCK_STATE_INVALID (-1003)
/* PASSIVE/sleepable owner only. No allocation, MMIO mapping or async work.
 * Readback matches encoded VID, not an exact analogue voltage claim.
 * On partial failure do not undo a successful downclock by raising frequency.
 * Caller must prohibit GPU startup unless report.ready and status zero. */
int bc250_clock_prepare(const struct bc250_clock_io *io, unsigned int mhz,
                        unsigned int mv, struct bc250_clock_report *report);
#endif
