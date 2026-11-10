/* Host test of the stored curve's pending mark over a sequence of starts (dpm_curve_mark.h, BD-343). The state
 * store is modelled as dpm.c writes it: DpmConfigureCurve at the top of a start, the tuner's KEEP, DpmConfirm at a
 * confirmation by the display milestone and DpmConfirmKnownCurve at a confirmation by the rule without output.
 * The pending mark is the shim's own 16-bit checksum (bc250_clock.c, linked in); DpmCurveConfirmed is the seal.
 * run_dpm_curve_mark.ps1 checks that dpm.c calls these rules at those places.
 */
#include <stdio.h>
#include <string.h>
#include "bc250_clock.h"
#include "dpm_curve_mark.h"

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

enum confirm { NOBODY, DISPLAY, HEADLESS };

struct store {
    int stored;                     /* a curve is on disk */
    struct bc250_clock_curve curve; /* DpmCurve1000..DpmCurve2000 */
    int pending_present;            /* DpmCurvePending */
    unsigned int pending;
    int confirmed_present;          /* DpmCurveConfirmed */
    unsigned int confirmed;
    int drops;                      /* starts that deleted the curve */
};

static unsigned int seal(const struct bc250_clock_curve* c) { return Bc250DpmCurveSeal(c->mv, BC250_CURVE_POINTS); }

static void make(struct bc250_clock_curve* c, unsigned int base)
{
    unsigned int i;
    for (i = 0; i < BC250_CURVE_POINTS; i++) c->mv[i] = base + 10u * i;
}

/* The tuner's KEEP (dpm.c, the curve KEEP): the values, the pending mark away, the seal. */
static void keep(struct store* g, const struct bc250_clock_curve* c)
{
    g->stored = 1; g->curve = *c;
    g->pending_present = 0;
    g->confirmed_present = 1; g->confirmed = seal(c);
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
    g->pending_present = 1; g->pending = bc250_clock_curve_checksum(&g->curve);  /* durable before the curve runs */
    pending = 1;
    if (how == DISPLAY && Bc250DpmCurveClearOnConfirm(pending, 1, g->confirmed_present, g->confirmed, seal(&g->curve))) {
        g->pending_present = 0; g->confirmed_present = 1; g->confirmed = seal(&g->curve);
    }
    if (how == HEADLESS && Bc250DpmCurveClearOnConfirm(pending, 0, g->confirmed_present, g->confirmed,
                                                       seal(&g->curve)))
        g->pending_present = 0;                         /* DpmConfirmKnownCurve writes no confirmed value */
    return 1;
}

/* A curve with the same 16-bit checksum as A and other values: three points move until the sums meet. */
static int collide(const struct bc250_clock_curve* a, struct bc250_clock_curve* b)
{
    unsigned int want = bc250_clock_curve_checksum(a), x, y, z;
    for (x = 820u; x < 1000u; x++)
        for (y = 820u; y < 1000u; y++)
            for (z = 820u; z < 1000u; z++) {
                *b = *a; b->mv[2] = x; b->mv[5] = y; b->mv[8] = z;
                if (memcmp(a, b, sizeof(*a)) != 0 && bc250_clock_curve_checksum(b) == want) return 1;
            }
    return 0;
}

int main(void)
{
    struct store g;
    struct bc250_clock_curve a, b;
    unsigned int i, j, low = 0, same = 0;

    make(&a, 850u);

    /* A curve that the tuner kept on a start with a display survives three starts without output. */
    memset(&g, 0, sizeof(g)); keep(&g, &a);
    for (i = 0; i < 3; i++) {
        CHECK(start(&g, HEADLESS));
        CHECK(g.stored && !g.pending_present && g.drops == 0 && g.confirmed == seal(&a));
    }
    CHECK(start(&g, DISPLAY) && !g.pending_present && g.drops == 0);

    /* A curve that a display start confirmed (not a KEEP) survives them too. */
    memset(&g, 0, sizeof(g)); g.stored = 1; make(&g.curve, 860u);
    CHECK(start(&g, DISPLAY) && g.confirmed_present && g.confirmed == seal(&g.curve));
    for (i = 0; i < 3; i++) CHECK(start(&g, HEADLESS) && !g.pending_present && g.drops == 0);

    /* A curve that no display start confirmed stays pending after a start without output, and the next start
     * drops it, as after any start that nobody confirmed. */
    memset(&g, 0, sizeof(g)); g.stored = 1; make(&g.curve, 870u);
    CHECK(start(&g, HEADLESS) && g.pending_present && g.pending == bc250_clock_curve_checksum(&g.curve));
    CHECK(!start(&g, HEADLESS) && !g.stored && g.drops == 1);

    /* A changed curve: one value edited outside the driver, so the seal differs and the mark is not cleared. */
    memset(&g, 0, sizeof(g)); keep(&g, &a); g.curve.mv[5] += 1u;
    CHECK(start(&g, HEADLESS) && g.pending_present);
    CHECK(!start(&g, DISPLAY) && !g.stored && g.drops == 1);

    /* A start without output that nobody confirms (it crashed or hung in its minute) keeps the mark, and the next
     * start drops even a curve that was confirmed before. */
    memset(&g, 0, sizeof(g)); keep(&g, &a);
    CHECK(start(&g, NOBODY) && g.pending_present);
    CHECK(!start(&g, HEADLESS) && !g.stored && g.drops == 1);

    /* The display milestone clears the mark of any curve this start applied, as before BD-343. */
    memset(&g, 0, sizeof(g)); g.stored = 1; make(&g.curve, 880u); g.confirmed_present = 1; g.confirmed = 0x1u;
    CHECK(start(&g, DISPLAY) && !g.pending_present && g.confirmed == seal(&g.curve));

    /* Review C1: a hand-written curve with the same 16-bit checksum as the confirmed one is not taken for it. */
    CHECK(collide(&a, &b));
    CHECK(bc250_clock_curve_checksum(&a) == bc250_clock_curve_checksum(&b) && seal(&a) != seal(&b));
    memset(&g, 0, sizeof(g)); keep(&g, &a); g.curve = b;
    CHECK(start(&g, HEADLESS) && g.pending_present);
    CHECK(!start(&g, HEADLESS) && !g.stored && g.drops == 1);

    /* The upgrade: a store of an older driver holds the 16-bit checksum in DpmCurveConfirmed. A start without
     * output clears no mark, and the next start drops the curve. */
    memset(&g, 0, sizeof(g)); g.stored = 1; g.curve = a;
    g.confirmed_present = 1; g.confirmed = bc250_clock_curve_checksum(&a);
    CHECK(start(&g, HEADLESS) && g.pending_present);
    CHECK(!start(&g, HEADLESS) && !g.stored && g.drops == 1);
    /* Or a start with a display confirms it first and writes the seal; then starts without output keep it. */
    memset(&g, 0, sizeof(g)); g.stored = 1; g.curve = a;
    g.confirmed_present = 1; g.confirmed = bc250_clock_curve_checksum(&a);
    CHECK(start(&g, DISPLAY) && !g.pending_present && g.confirmed == seal(&a));
    for (i = 0; i < 3; i++) CHECK(start(&g, HEADLESS) && !g.pending_present && g.drops == 0);

    /* The seal alone: never below 0x10000 (so never an older store's checksum, never 0), and a change of one value
     * always gives another seal, over every point and a band of values. */
    for (i = 0; i < BC250_CURVE_POINTS; i++)
        for (j = 1; j < 400u; j++) {
            b = a; b.mv[i] += j;
            if (seal(&b) < 0x10000u) low++;
            if (seal(&b) == seal(&a)) same++;
        }
    CHECK(seal(&a) >= 0x10000u && low == 0 && same == 0);

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
