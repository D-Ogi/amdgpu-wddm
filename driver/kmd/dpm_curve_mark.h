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
 *   - a confirmation by the rule without output clears it only when DpmCurveConfirmed holds the seal of that
 *     curve (DpmConfirmKnownCurve). A curve that no display start confirmed stays pending, and the next start
 *     drops it, as after any start that nobody confirmed;
 *   - a start that nobody confirms clears nothing.
 *
 * The seal (BD-343, review C1). DpmCurveConfirmed holds Bc250DpmCurveSeal of the confirmed curve, which a KEEP and a
 * display confirmation write. It is not the 16-bit checksum of the pending mark: two curves with the same checksum
 * are easy to find, and the rule without output must not take a curve that nobody confirmed for the confirmed one.
 * The seal is a 32-bit mix of the eleven values in their order. Each step of the mix is a bijection, so before the
 * last line two curves that differ in one value never have the same mix. Two different curves have the same seal
 * with a chance of about 1 in 2^32 for an edit that does not search for it. The last line keeps the seal at or
 * above 0x10000. A store of an older driver (0.7.210 to BD-343) holds the 16-bit checksum in DpmCurveConfirmed,
 * and no seal is equal to such a value. Thus at the first start after an update, a start without output clears no
 * mark: the curve stays pending, and the next start drops it, unless a start with a display confirms it first and
 * writes its seal.
 * The limit: the curve is sealed, and the boundary is the state store. The seal is not a defence against a writer
 * of the state store. Such a writer can write DpmCurveConfirmed itself, and the mix has no key, so a writer can
 * also search for another curve with the same seal (about 2^32 tries) and write that curve. The seal keeps a curve
 * that an edit outside the driver changed, without such a search, from passing as confirmed.
 * The value stays a DWORD record of the same name, so the state catalog does not change.
 *
 * Plain C with no kernel call, so the host test (driver/kmd/test/dpm_curve_mark_test.c) runs every sequence of
 * starts. dpm.c owns the registry and the curve itself.
 */
#ifndef BC250_DPM_CURVE_MARK_H
#define BC250_DPM_CURVE_MARK_H

/* The seal of a curve: Mv is the curve's values (struct bc250_clock_curve mv), Count their number. */
static __inline unsigned int Bc250DpmCurveSeal(const unsigned int* Mv, unsigned int Count)
{
    unsigned int h = 0x6A09E667u ^ Count, i;
    for (i = 0; i < Count; i++) {
        h ^= Mv[i];
        h *= 0x85EBCA6Bu;
        h ^= h >> 13;
        h *= 0xC2B2AE35u;
        h ^= h >> 16;
    }
    if (h < 0x10000u) h |= 0x80000000u;     /* never equal to a checksum of an older store, never 0 */
    return h;
}

/* At the top of a start: the stored curve runs (and is marked pending again) unless a mark is still on disk. */
static __inline int Bc250DpmCurveDropAtStart(int Stored, int PendingPresent)
{
    return Stored && PendingPresent;
}

/* At a confirmation of the start. Pending: this start applied a stored curve and marked it. Display: CONFIRM met
 * the display milestone. ConfirmedPresent and Confirmed: DpmCurveConfirmed as the state store holds it. Seal:
 * Bc250DpmCurveSeal of the curve this start applied. Nonzero: delete the mark. */
static __inline int Bc250DpmCurveClearOnConfirm(int Pending, int Display, int ConfirmedPresent, unsigned int Confirmed,
                                                unsigned int Seal)
{
    if (!Pending) return 0;
    if (Display) return 1;
    return ConfirmedPresent && Confirmed == Seal;
}

#endif
