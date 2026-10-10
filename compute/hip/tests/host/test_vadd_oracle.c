/* test_vadd_oracle.c - the acceptance criterion of samples/vadd.hip must be able to fail.
 *
 * The step-2 criterion of M16 said the sample proves that the second kernel runs after the
 * first one. The second kernel multiplied C by 1.0, so every order of the two kernels gave the
 * accepted result and the criterion proved nothing (independent audit of 2026-10-10, HS-3).
 *
 * This test holds the criterion to the one property that makes it a criterion: with the scale
 * after the addition the expected value is what the device leaves behind, and with the scale
 * before it, or with no scale at all, it is not. Both halves come from samples/vadd_expect.h,
 * which the sample itself uses, so the sample and this test cannot drift apart.
 *
 * It needs no device and no GPU compiler: the oracle is plain C arithmetic.
 */
#include "test_common.h"

#include "samples/vadd_expect.h"

/* The sample's own inputs: a[i] = i, b[i] = 2 i. Index 0 is the one element where the sums of
 * every order agree (0 is 2 x 0), which is why the discrimination is checked away from it. */
static float sample_a(const int i)
{
    return (float)i;
}

static float sample_b(const int i)
{
    return (float)(2 * i);
}

static void check_expected_matches_the_right_order(void)
{
    int i;
    for (i = 0; i < 4096; ++i) {
        const float a = sample_a(i);
        const float b = sample_b(i);
        CHECK(vadd_expected(a, b) == vadd_produced(a, b, VADD_SCALE_AFTER));
    }
    /* The factor really is in the expected value: 3 i times the factor, and not 3 i. The
     * volatile copy keeps the compiler from folding the comparison, which /W4 /WX refuses as a
     * constant conditional expression. */
    {
        const volatile float factor = VADD_SCALE_FACTOR;
        CHECK(vadd_expected(1.0f, 2.0f) == 3.0f * factor);
        CHECK(factor != 1.0f);
    }
}

/* The negative controls. A wrong order, and a missing second kernel, must be visible in the
 * numbers for all but the one element whose sum is zero. */
static void check_wrong_orders_are_refused(void)
{
    const int orders[] = {VADD_SCALE_BEFORE, VADD_SCALE_OMIT};
    uint32_t  o;
    for (o = 0; o < TEST_COUNT(orders); ++o) {
        int mismatches = 0;
        int i;
        for (i = 0; i < 4096; ++i) {
            const float a = sample_a(i);
            const float b = sample_b(i);
            if (vadd_produced(a, b, orders[o]) != vadd_expected(a, b)) {
                mismatches++;
            }
        }
        /* Every element but index 0, whose sum is zero. */
        CHECK_U64((unsigned)mismatches, 4095u);
        CHECK(vadd_produced(1.0f, 2.0f, orders[o]) != vadd_expected(1.0f, 2.0f));
        printf("test_vadd_oracle: order %s leaves %d of 4096 elements wrong\n",
               vadd_order_name(orders[o]), mismatches);
    }
    /* And the old criterion, for the record: with a factor of 1 the wrong order was accepted.
     * The expression below is what the sample computed until 2026-10-10. */
    {
        const volatile float identity = 1.0f;
        CHECK((1.0f + 2.0f) * identity == 1.0f + 2.0f);
    }
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    check_expected_matches_the_right_order();
    check_wrong_orders_are_refused();
    return test_report("test_vadd_oracle");
}
