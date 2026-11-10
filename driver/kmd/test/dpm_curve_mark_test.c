/* Host test of the stored curve's pending mark over a sequence of starts (dpm_curve_mark.h, BD-343). The state
 * store is modelled as dpm.c writes it: DpmConfigureCurve at the top of a start, the tuner's KEEP, DpmConfirm at a
 * confirmation by the display milestone and DpmConfirmKnownCurve at a confirmation by the rule without output.
 * run_dpm_curve_mark.ps1 checks that dpm.c calls these rules at those places.
 */
#include <stdio.h>
#include <string.h>
#include "dpm_curve_mark.h"

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

enum confirm { NOBODY, DISPLAY, HEADLESS };

struct store {
    int stored;                 /* a curve is on disk */
    unsigned int sum;           /* its checksum */
    int pending_present;        /* DpmCurvePending */
    unsigned int pending;
    int confirmed_present;      /* DpmCurveConfirmed */
    unsigned int confirmed;
    int drops;                  /* starts that deleted the curve */
};

/* The tuner's KEEP (dpm.c, the curve KEEP): the values, the pending mark away, the confirmed mark. */
static void keep(struct store* g, unsigned int sum)
{
    g->stored = 1; g->sum = sum;
    g->pending_present = 0;
    g->confirmed_present = 1; g->confirmed = sum;
}

/* One start. Returns 1 when the curve ran in it. */
static int start(struct store* g, enum confirm how)
{
    int pending;
    if (Bc250DpmCurveDropAtStart(g->stored, g->pending_present)) {
        g->stored = 0; g->pending_present = 0; g->confirmed_present = 0; g->drops++;
        return 0;
    }
    if (!g->stored) return 0;
    g->pending_present = 1; g->pending = g->sum;        /* durable before the curve runs */
    pending = 1;
    if (how == DISPLAY && Bc250DpmCurveClearOnConfirm(pending, 1, g->confirmed_present, g->confirmed, g->sum)) {
        g->pending_present = 0; g->confirmed_present = 1; g->confirmed = g->sum;
    }
    if (how == HEADLESS && Bc250DpmCurveClearOnConfirm(pending, 0, g->confirmed_present, g->confirmed, g->sum))
        g->pending_present = 0;                         /* DpmConfirmKnownCurve writes no confirmed mark */
    return 1;
}

int main(void)
{
    struct store g;
    int i;

    /* A curve that the tuner kept on a start with a display survives three starts without output. */
    memset(&g, 0, sizeof(g)); keep(&g, 0x1234u);
    for (i = 0; i < 3; i++) {
        CHECK(start(&g, HEADLESS));
        CHECK(g.stored && !g.pending_present && g.drops == 0 && g.confirmed == 0x1234u);
    }
    CHECK(start(&g, DISPLAY) && !g.pending_present && g.drops == 0);

    /* A curve that a display start confirmed (not a KEEP) survives them too. */
    memset(&g, 0, sizeof(g)); g.stored = 1; g.sum = 0x2222u;
    CHECK(start(&g, DISPLAY) && g.confirmed_present && g.confirmed == 0x2222u);
    for (i = 0; i < 3; i++) CHECK(start(&g, HEADLESS) && !g.pending_present && g.drops == 0);

    /* A curve that no display start confirmed stays pending after a start without output, and the next start
     * drops it, as after any start that nobody confirmed. */
    memset(&g, 0, sizeof(g)); g.stored = 1; g.sum = 0x3333u;
    CHECK(start(&g, HEADLESS) && g.pending_present && g.pending == 0x3333u);
    CHECK(!start(&g, HEADLESS) && !g.stored && g.drops == 1);

    /* A changed curve: the confirmed mark is another checksum, so the mark is not cleared. */
    memset(&g, 0, sizeof(g)); keep(&g, 0x4444u); g.sum = 0x4445u;
    CHECK(start(&g, HEADLESS) && g.pending_present);
    CHECK(!start(&g, DISPLAY) && !g.stored && g.drops == 1);

    /* A start without output that nobody confirms (it crashed or hung in its minute) keeps the mark, and the next
     * start drops even a curve that was confirmed before. */
    memset(&g, 0, sizeof(g)); keep(&g, 0x5555u);
    CHECK(start(&g, NOBODY) && g.pending_present);
    CHECK(!start(&g, HEADLESS) && !g.stored && g.drops == 1);

    /* The display milestone clears the mark of any curve this start applied, as before BD-343. */
    memset(&g, 0, sizeof(g)); g.stored = 1; g.sum = 0x6666u; g.confirmed_present = 1; g.confirmed = 0x1u;
    CHECK(start(&g, DISPLAY) && !g.pending_present && g.confirmed == 0x6666u);

    /* The rule alone. */
    CHECK(!Bc250DpmCurveClearOnConfirm(0, 1, 1, 7u, 7u));
    CHECK(!Bc250DpmCurveClearOnConfirm(0, 0, 1, 7u, 7u));
    CHECK(Bc250DpmCurveClearOnConfirm(1, 0, 1, 7u, 7u));
    CHECK(!Bc250DpmCurveClearOnConfirm(1, 0, 0, 7u, 7u));
    CHECK(!Bc250DpmCurveClearOnConfirm(1, 0, 1, 8u, 7u));
    CHECK(Bc250DpmCurveDropAtStart(1, 1) && !Bc250DpmCurveDropAtStart(1, 0) && !Bc250DpmCurveDropAtStart(0, 1));

    printf("dpm curve mark: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
