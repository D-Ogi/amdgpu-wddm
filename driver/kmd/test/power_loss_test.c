/* BD-188 D-2 (docs/design/power-loss.md): the pure rules of power_loss.h. run_power_loss.ps1 builds it with the
 * KMD's compiler; the same script reads wddm.c for the properties that the host cannot compile. */
#include <stdio.h>
#include "power_loss.h"
#include "power_phase.h"

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL %u: %s\n", __LINE__, #x); } } while (0)

/* The pattern proves something only if it is neither the power-on value nor what a refused bus read gives. */
typedef char PatternNotZeroOrOnes[(BC250_POWER_WITNESS_PATTERN != 0u && BC250_POWER_WITNESS_PATTERN != 0xFFFFFFFFu) ? 1 : -1];
typedef char PatternHalvesNotZero[((BC250_POWER_WITNESS_PATTERN & 0xFFFFu) != 0u && (BC250_POWER_WITNESS_PATTERN >> 16) != 0u) ? 1 : -1];

int main(void)
{
    /* Condition (a): a D3 target, or a known S1-S3 or S4, and nothing else. */
    CHECK(Bc250PowerLossExpected(4u, 0u));
    CHECK(Bc250PowerLossExpected(4u, 6u));
    CHECK(Bc250PowerLossExpected(2u, BC250_POWER_ACTION_SLEEP));
    CHECK(Bc250PowerLossExpected(3u, BC250_POWER_ACTION_HIBERNATE));
    CHECK(!Bc250PowerLossExpected(2u, 0u));
    CHECK(!Bc250PowerLossExpected(3u, 8u));
    CHECK(!Bc250PowerLossExpected(1u, 0u));

    /* Lead condition 2 and condition (f): the witness comes from the per-IP row, never from a board constant. */
    CHECK(Bc250PowerWitnessFor(BC250_IP_VERSION(10, 1, 3)) == BC250_POWER_WITNESS_GC_SCRATCH_REG1);
    CHECK(Bc250PowerWitnessFor(BC250_IP_VERSION(10, 1, 0)) == BC250_POWER_WITNESS_GC_SCRATCH_REG1);
    CHECK(Bc250PowerWitnessFor(BC250_IP_VERSION(10, 3, 0)) == BC250_POWER_WITNESS_NONE);
    CHECK(Bc250PowerWitnessFor(BC250_IP_VERSION(11, 0, 0)) == BC250_POWER_WITNESS_NONE);
    CHECK(Bc250PowerWitnessFor(BC250_IP_VERSION(9, 1, 0)) == BC250_POWER_WITNESS_NONE);
    CHECK(Bc250PowerWitnessOfDevice(0x13FE1002u) == BC250_POWER_WITNESS_GC_SCRATCH_REG1);
    CHECK(Bc250PowerWitnessOfDevice(0x13FF1002u) == BC250_POWER_WITNESS_NONE);   /* not in asic_ip.h */
    CHECK(Bc250PowerWitnessOfDevice(0x13FE1022u) == BC250_POWER_WITNESS_NONE);   /* another vendor */
    CHECK(Bc250PowerWitnessOfDevice(0u) == BC250_POWER_WITNESS_NONE);            /* a failed configuration read */

    /* pnp.c: the display repair after a failed way down, never after an unsaved one (its engines are down). */
    {
        volatile long phase=BC250_POWER_FAILED;
        CHECK(Bc250PowerDownRepairsDisplay(phase));
        phase=BC250_POWER_SUSPENDING;
        CHECK(Bc250PowerDownRepairsDisplay(phase));
        phase=BC250_POWER_SUSPENDED_UNSAVED;
        CHECK(!Bc250PowerDownRepairsDisplay(phase));
    }

    /* The verdict: only 0 is a power loss, only the pattern is power kept, everything else is no proof. */
    CHECK(Bc250PowerWitnessVerdict(1, 1, 0u) == BC250_POWER_WITNESS_LOST);
    CHECK(Bc250PowerWitnessVerdict(1, 1, BC250_POWER_WITNESS_PATTERN) == BC250_POWER_WITNESS_KEPT);
    CHECK(Bc250PowerWitnessVerdict(1, 1, 0xFFFFFFFFu) == BC250_POWER_WITNESS_UNPROVEN);
    CHECK(Bc250PowerWitnessVerdict(1, 1, 0xDEADBEEFu) == BC250_POWER_WITNESS_UNPROVEN);
    CHECK(Bc250PowerWitnessVerdict(1, 0, 0u) == BC250_POWER_WITNESS_UNPROVEN);   /* a refused read */
    CHECK(Bc250PowerWitnessVerdict(0, 1, 0u) == BC250_POWER_WITNESS_UNPROVEN);   /* never armed */

    /* Condition (e), "a TDR after D0 is recovered": ResetEngine refuses only the jobs that the power took, so the
     * adapter reset retires them; a later job of the node is the normal case again. Sequences wrap. */
    CHECK(Bc250HangPowerLost(1, 100u, 100u));
    CHECK(Bc250HangPowerLost(1, 99u, 100u));
    CHECK(!Bc250HangPowerLost(1, 101u, 100u));
    CHECK(!Bc250HangPowerLost(0, 100u, 100u));
    CHECK(!Bc250HangPowerLost(1, 100u, 0u));
    CHECK(Bc250HangPowerLost(1, 0xFFFFFFFEu, 0xFFFFFFFFu));
    CHECK(!Bc250HangPowerLost(1, 1u, 0xFFFFFFFFu));

    printf("Power loss: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
