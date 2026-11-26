/* Host control of present_contextless.h (BD-403): from which WDDM level the adapter reports
 * DXGK_DRIVERCAPS.MiscCaps.SupportContextlessPresent, and what DxgkDdiPresent does with a call that has no context.
 *
 * The cases: the cap is clear at 2.0, 2.1 and 2.2 and set at every level of the set from 2.4 (the default 2.9
 * among them); a call with a context always goes to the producers, whatever it asks for; a call without a context
 * that asks for no copy, no fill and has no DMA buffer is a token with nothing to build; a call without a context
 * that asks for a copy, a fill or carries a DMA buffer is refused. Two negative controls of the runner change one
 * rule each in a copy of the header and must fail this program.
 */
#include <stdio.h>
#include "present_contextless.h"

#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

int main(void)
{
    unsigned long checks = 0, failures = 0, i;
    int context, copy, dma;

    /* ---- the cap, at every level of the set ------------------------------------------------------------------ */
    for (i = 0; i < BC250_WDDM_LEVEL_SET_COUNT; i++)
        CHECK(Bc250ContextlessPresentCap(Bc250WddmLevels[i]) == (Bc250WddmLevels[i] >= BC250_WDDM_LEVEL_2_4));
    CHECK(!Bc250ContextlessPresentCap(BC250_WDDM_LEVEL_2_0));
    CHECK(!Bc250ContextlessPresentCap(BC250_WDDM_LEVEL_2_2));
    CHECK(Bc250ContextlessPresentCap(BC250_WDDM_LEVEL_2_4));
    CHECK(Bc250ContextlessPresentCap(BC250_WDDM_LEVEL_DEFAULT));
    CHECK(Bc250ContextlessPresentCap(BC250_WDDM_LEVEL_MAX));

    /* ---- the route: all eight combinations ------------------------------------------------------------------- */
    for (context = 0; context < 2; context++)
        for (copy = 0; copy < 2; copy++)
            for (dma = 0; dma < 2; dma++) {
                enum bc250_present_route r = Bc250PresentRoute(context, copy, dma);
                if (context) CHECK(r == BC250_PRESENT_WITH_CONTEXT);
                else if (copy || dma) CHECK(r == BC250_PRESENT_CONTEXTLESS_REFUSED);
                else CHECK(r == BC250_PRESENT_CONTEXTLESS_TOKEN);
            }
    /* The shape dxgkrnl 22621 builds for an independent-flip token: RedirectedFlip only, no DMA buffer. */
    CHECK(Bc250PresentRoute(0, 0, 0) == BC250_PRESENT_CONTEXTLESS_TOKEN);
    /* A Blt never loses its context in silence: without one it is refused, not dropped with success. */
    CHECK(Bc250PresentRoute(0, 1, 1) == BC250_PRESENT_CONTEXTLESS_REFUSED);
    CHECK(Bc250PresentRoute(0, 1, 0) == BC250_PRESENT_CONTEXTLESS_REFUSED);

    printf("present_contextless: %lu checks, %lu failures\n", checks, failures);
    return failures ? 1 : 0;
}
