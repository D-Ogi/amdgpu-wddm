/* dpm_sustain_log.h - the lines of the sustained raise (BD-329) in the driver log (BD-336).
 *
 * Why it exists. The start line of the sustained raise ("dpm: sustained raise: avg at T permille for H ms raises -> on")
 * comes after the 256 kept lines of the KMD log ring, and the rotating part of the ring overwrites it within seconds of
 * the boot (the rc3 read of 2026-11-09: 154 lines lost 42 s after the boot). Before BD-336 the log summary printed the
 * counters line of the rule only after the rule chose a raise or refused one, so no read after the boot could show
 * whether the rule was on.
 *
 * What this object decides.
 *   - The word of the start, "on" or why the rule is off. dpm.c prints it in the start line and in every summary.
 *   - Which lines one log block prints: the summary prints the configuration line always, and the counters line only
 *     when a counter is not 0, as before BD-336. The periodic telemetry block prints only the counters line.
 * The format strings stay in dpm.c, where the guardlog-width gate reads them. The host test reads them from dpm.c
 * (driver/kmd/test/run_dpm_sustain_log.ps1) and prints the lines of each case with them.
 *
 * Plain C with no kernel call, so the host test (driver/kmd/test/dpm_sustain_log_test.c) runs every case.
 * It decides nothing about the clock. The answer only moves lines of evidence.
 */
#ifndef BC250_DPM_SUSTAIN_LOG_H
#define BC250_DPM_SUSTAIN_LOG_H

#define BC250_DPM_SUSTAIN_STATE_ON "on"
#define BC250_DPM_SUSTAIN_STATE_NOT_GOVERNING "off: this start does not govern the clock"
#define BC250_DPM_SUSTAIN_STATE_HOLD_REFUSED "off: the hold is out of the allowed range"

/* The word of a start. Governing: this start governs the clock. Accepted: bc250_dpm_sustain_config took the hold. */
static __inline const char* Bc250DpmSustainState(int Governing, int Accepted)
{
    if (!Governing) return BC250_DPM_SUSTAIN_STATE_NOT_GOVERNING;
    return Accepted ? BC250_DPM_SUSTAIN_STATE_ON : BC250_DPM_SUSTAIN_STATE_HOLD_REFUSED;
}

#define BC250_DPM_SUSTAIN_LINE_CONFIG   0x1u   /* "dpm: summary rules: sustained raise: ..." */
#define BC250_DPM_SUSTAIN_LINE_COUNTERS 0x2u   /* "dpm: <what> sustained raises N refused R, hold H ms" */

/* The lines of one block. Summary: the log summary (BC250_ESCAPE_LOG_SUMMARY); otherwise the telemetry block or the
 * stop line. */
static __inline unsigned int Bc250DpmSustainLines(int Summary, unsigned long Raises, unsigned long Refusals)
{
    unsigned int lines = 0u;
    if (Summary) lines |= BC250_DPM_SUSTAIN_LINE_CONFIG;
    if (Raises != 0u || Refusals != 0u) lines |= BC250_DPM_SUSTAIN_LINE_COUNTERS;
    return lines;
}

#endif
