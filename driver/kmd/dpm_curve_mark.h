/* dpm_curve_mark.h - the pending mark of the stored V/F curve over the starts (0.7.210, BD-343).
 *
 * Why it exists. A stored curve (the tuner's KEEP) runs only behind a pending mark: every start that applies it
 * writes DpmCurvePending (the curve's checksum) first, and only a confirmation of that start deletes the mark. A
 * start that finds a mark deletes the curve and runs the table's own line. So a curve that survives the desktop and
 * not the next boot costs the curve, never the machine (dpm.c DpmConfigureCurve).
 *
 * BD-343 adds a start without output. CONFIRM certifies it by the rule without output (start_health.c,
 * HealthConfirmableHeadless): a full start, admitted engines and a minute with no fault, but no workload and no
 * display. That minute is the same witness for a curve as a display minute: neither proves an hour of load. It is
 * no proof for a curve that no start with a display ever confirmed (review F1). Thus:
 *   - a confirmation by the display milestone clears the mark of the curve this start applied (DpmConfirm);
 *   - a confirmation by the rule without output clears it only when DpmCurveConfirmed already holds the checksum of
 *     that curve (DpmConfirmKnownCurve). A curve that no display start confirmed stays pending, and the next start
 *     drops it, as after any start that nobody confirmed;
 *   - a start that nobody confirms clears nothing.
 *
 * Plain C with no kernel call, so the host test (driver/kmd/test/dpm_curve_mark_test.c) runs every sequence of
 * starts. dpm.c owns the registry and the curve itself.
 */
#ifndef BC250_DPM_CURVE_MARK_H
#define BC250_DPM_CURVE_MARK_H

/* At the top of a start: the stored curve runs (and is marked pending again) unless a mark is still on disk. */
static __inline int Bc250DpmCurveDropAtStart(int Stored, int PendingPresent)
{
    return Stored && PendingPresent;
}

/* At a confirmation of the start. Pending: this start applied a stored curve and marked it. Display: CONFIRM met
 * the display milestone. ConfirmedPresent and Confirmed: DpmCurveConfirmed as the state store holds it. Sum: the
 * checksum of the curve this start applied. Nonzero: delete the mark. */
static __inline int Bc250DpmCurveClearOnConfirm(int Pending, int Display, int ConfirmedPresent, unsigned int Confirmed,
                                                unsigned int Sum)
{
    if (!Pending) return 0;
    if (Display) return 1;
    return ConfirmedPresent && Confirmed == Sum;
}

#endif
