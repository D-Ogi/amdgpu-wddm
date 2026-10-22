// Host contract checks and a numeric protocol for the independent rational oracle.
#include "../../shim/bc250_scaler_geometry.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static int IsZero(const BC250_SCALER_AXIS_PLAN* p)
{
    const BC250_SCALER_AXIS_PLAN z = {0};
    return memcmp(p, &z, sizeof(z)) == 0;
}

static void Controls(void)
{
    BC250_SCALER_AXIS_INPUT in = {1920, 1920, 0, 1920, -128, 2432, 0, 1920, 4, 0};
    BC250_SCALER_AXIS_PLAN p;
    CHECK(Bc250ScalerAxisPlan(&in, &p) == BC250_SCALER_GEOMETRY_OK);
    CHECK(p.RecoutBegin == 0 && p.RecoutSize == 1920);
    CHECK(p.ViewportBegin == 94 && p.ViewportSize == 1444);
    CHECK(p.Ratio19 == 393216 && p.Init19 == 2555904); // 0.75 and 4.875
    in.Mirror = 1;
    CHECK(Bc250ScalerAxisPlan(&in, &p) == BC250_SCALER_GEOMETRY_OK);
    CHECK(p.ViewportBegin == 382 && p.ViewportSize == 1444);
    in = (BC250_SCALER_AXIS_INPUT){300, 200, 50, 250, -100, 100, 20, 200, 1, 0};
    CHECK(Bc250ScalerAxisPlan(&in, &p) == BC250_SCALER_GEOMETRY_OK);
    CHECK(p.ViewportBegin == 170 && p.ViewportSize == 80);
    CHECK(p.RecoutBegin == 20 && p.RecoutSize == 80);
    CHECK(p.Ratio19 == 524288 && p.Init19 == 786432);
    in = (BC250_SCALER_AXIS_INPUT){8, 1, 0, 8, 0, 1, 0, 1, 4, 0};
    CHECK(Bc250ScalerAxisPlan(&in, &p) == BC250_SCALER_GEOMETRY_OK);
    CHECK(p.Ratio19 == 4194303 && p.Init19 == 3407872); // 8-epsilon, 6.5
    CHECK(p.ViewportBegin == 0 && p.ViewportSize == 6);
    CHECK(Bc250ScalerAxisPlan(&in, NULL) == BC250_SCALER_GEOMETRY_INVALID);
    CHECK(Bc250ScalerAxisPlan(NULL, &p) == BC250_SCALER_GEOMETRY_INVALID && IsZero(&p));
    in.ClipBegin = in.ClipEnd;
    CHECK(Bc250ScalerAxisPlan(&in, &p) == BC250_SCALER_GEOMETRY_EMPTY && IsZero(&p));
    in.ClipBegin = 0;
    in.DestinationBegin = INT_MIN;
    in.DestinationEnd = INT_MAX;
    CHECK(Bc250ScalerAxisPlan(&in, &p) == BC250_SCALER_GEOMETRY_UNSUPPORTED && IsZero(&p));
    in.SourceBegin = INT_MIN;
    CHECK(Bc250ScalerAxisPlan(&in, &p) == BC250_SCALER_GEOMETRY_INVALID && IsZero(&p));
}

static void Properties(void)
{
    const unsigned sizes[] = {1, 2, 3, 16, 127, 720, 1080, 1920, 4096, 16383};
    unsigned s, d, t, k, m;
    for (s = 0; s < sizeof(sizes)/sizeof(sizes[0]); ++s)
    for (d = 0; d < sizeof(sizes)/sizeof(sizes[0]); ++d)
    for (t = 1; t <= 8; ++t)
    for (k = 0; k <= 8; ++k)
    for (m = 0; m <= 1; ++m) {
        BC250_SCALER_AXIS_INPUT in = {0};
        BC250_SCALER_AXIS_PLAN p;
        int result;
        uint64_t ratio, base_init, phase, expected;
        unsigned forward;
        in.SurfaceSize = sizes[s]; in.SourceEnd = (int)sizes[s];
        in.ActiveSize = sizes[d]; in.DestinationEnd = (int)sizes[d];
        in.ClipBegin = (int)(sizes[d] * k / 8); in.ClipEnd = (int)sizes[d];
        in.Taps = t; in.Mirror = m;
        result = Bc250ScalerAxisPlan(&in, &p);
        if (result != BC250_SCALER_GEOMETRY_OK) {
            CHECK(IsZero(&p));
            continue;
        }
        CHECK(p.ViewportSize > 0 && p.ViewportBegin + p.ViewportSize <= sizes[s]);
        CHECK(p.RecoutBegin == (unsigned)in.ClipBegin && p.RecoutSize == sizes[d] - p.RecoutBegin);
        CHECK(p.Ratio19 > 0 && p.Ratio19 < 8 * 524288 && p.Init19 < 16 * 524288);
        // Clipping must preserve the first sample's position in the original
        // source, even when taps force the viewport backwards. No invocation of
        // the upstream viewport routine is used for this conservation equation.
        ratio = ((uint64_t)sizes[s] << 19) / sizes[d];
        ratio <<= 13;
        if (ratio == (8ULL << 32)) --ratio;
        base_init = (ratio + ((uint64_t)(t + 1) << 32) + 1) / 2;
        expected = (base_init + ratio * p.RecoutBegin) >> 13;
        forward = m ? sizes[s] - p.ViewportBegin - p.ViewportSize : p.ViewportBegin;
        phase = ((uint64_t)forward << 19) + p.Init19;
        CHECK(phase == expected);
    }
}

int main(int argc, char** argv)
{
    if (argc == 2 && strcmp(argv[1], "--stream") == 0) {
        BC250_SCALER_AXIS_INPUT in;
        BC250_SCALER_AXIS_PLAN p;
        int n, status;
        while ((n = scanf_s("%u %u %d %d %d %d %d %d %u %u",
                &in.SurfaceSize, &in.ActiveSize, &in.SourceBegin, &in.SourceEnd,
                &in.DestinationBegin, &in.DestinationEnd, &in.ClipBegin, &in.ClipEnd,
                &in.Taps, &in.Mirror)) == 10) {
            status = Bc250ScalerAxisPlan(&in, &p);
            printf("%d %u %u %u %u %u %u\n", status, p.ViewportBegin,
                   p.ViewportSize, p.RecoutBegin, p.RecoutSize, p.Ratio19, p.Init19);
        }
        return n == EOF ? 0 : 2;
    }
    Controls();
    Properties();
    printf("scaler geometry: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
