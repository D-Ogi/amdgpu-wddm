/* vadd_expect.h - the numerical oracle of samples/vadd.hip, in one place so that the sample and
 * the host test tests/host/test_vadd_oracle.c cannot disagree about it.
 *
 * Why it exists. The step-2 acceptance criterion of M16 claimed that the sample proves the
 * event between its two streams: the second kernel must run after the first one. It did not.
 * The second kernel multiplied C by 1.0, so running it first, running it last or leaving it out
 * gave the same accepted sums, because the vector addition overwrites C in every order
 * (independent audit of 2026-10-10, finding HS-3).
 *
 * The second kernel therefore multiplies by a factor that is not the identity, and the expected
 * sum carries that factor. The other two orders are kept as named negative controls: the
 * program can be asked for them, and then the same oracle must refuse the result.
 *
 * Plain C, no HIP: the host test includes this file with no compiler of device code. */

#ifndef BC250_VADD_EXPECT_H
#define BC250_VADD_EXPECT_H

/* Not 1.0: an identity operation cannot be seen in the result. */
#define VADD_SCALE_FACTOR 2.0f

/* Where the scale kernel runs with respect to the vector addition. AFTER is what the program
 * does; the other two exist so that a trial can show the criterion fails when the order is
 * wrong. */
#define VADD_SCALE_AFTER  0
#define VADD_SCALE_BEFORE 1
#define VADD_SCALE_OMIT   2

/* What the program promises for element i. */
static float vadd_expected(const float a, const float b) {
    return (a + b) * VADD_SCALE_FACTOR;
}

/* What the device really leaves in C under each order, with correct hardware and a correct
 * event: the addition writes C whole, so a scale that runs before it is overwritten, and a
 * scale that does not run at all leaves the plain sum.
 *
 * This is the model the host test checks the oracle against. It is not what the program reads
 * from the device: on the lab the program reads the device. */
static float vadd_produced(const float a, const float b, const int order) {
    if (order == VADD_SCALE_AFTER) {
        return (a + b) * VADD_SCALE_FACTOR;
    }
    /* VADD_SCALE_BEFORE: the scale multiplies the cleared buffer and the addition overwrites it.
     * VADD_SCALE_OMIT: nothing scales the sum. Both leave the plain sum. */
    return a + b;
}

static const char* vadd_order_name(const int order) {
    if (order == VADD_SCALE_AFTER) {
        return "after";
    }
    if (order == VADD_SCALE_BEFORE) {
        return "before";
    }
    return "omit";
}

#endif /* BC250_VADD_EXPECT_H */
