// Host contract checks and a numeric protocol for the independent rational oracle.
#include "../../shim/bc250_scaler_geometry.h"
#include "../../shim/bc250_scaler_buffer.h"
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
    unsigned s, d, t, k, m, divisor;
    for (s = 0; s < sizeof(sizes)/sizeof(sizes[0]); ++s)
    for (d = 0; d < sizeof(sizes)/sizeof(sizes[0]); ++d)
    for (t = 1; t <= 8; ++t)
    for (k = 0; k <= 8; ++k)
    for (m = 0; m <= 1; ++m)
    for (divisor = 1; divisor <= 2; ++divisor) {
        BC250_SCALER_AXIS_INPUT in = {0};
        BC250_SCALER_AXIS_PLAN p;
        int result;
        uint64_t ratio, base_init, phase, expected;
        unsigned forward;
        in.SurfaceSize = sizes[s]; in.SourceEnd = (int)sizes[s];
        in.ActiveSize = sizes[d]; in.DestinationEnd = (int)sizes[d];
        in.ClipBegin = (int)(sizes[d] * k / 8); in.ClipEnd = (int)sizes[d];
        in.Taps = t; in.Mirror = m;
        result = Bc250ScalerComponentAxisPlan(&in, divisor, &p);
        if (result != BC250_SCALER_GEOMETRY_OK) {
            CHECK(IsZero(&p));
            continue;
        }
        CHECK(p.ViewportSize > 0 && p.ViewportBegin + p.ViewportSize <= sizes[s] / divisor);
        CHECK(p.RecoutBegin == (unsigned)in.ClipBegin && p.RecoutSize == sizes[d] - p.RecoutBegin);
        CHECK(p.Ratio19 > 0 && p.Ratio19 < 8 * 524288 && p.Init19 < 16 * 524288);
        // Clipping must preserve the first sample's position in the original
        // source, even when taps force the viewport backwards. No invocation of
        // the upstream viewport routine is used for this conservation equation.
        ratio = (((uint64_t)sizes[s] << 32) + sizes[d] / 2) / sizes[d];
        ratio /= divisor;
        ratio = (ratio >> 13) << 13;
        if (ratio == (8ULL << 32)) --ratio;
        base_init = (ratio + ((uint64_t)(t + 1) << 32) + 1) / 2;
        expected = (base_init + ratio * p.RecoutBegin) >> 13;
        forward = m ? sizes[s] / divisor - p.ViewportBegin - p.ViewportSize : p.ViewportBegin;
        phase = ((uint64_t)forward << 19) + p.Init19;
        CHECK(phase == expected);
    }
}

static void ChromaAndPlane(void)
{
    BC250_SCALER_AXIS_INPUT h = {1920, 1920, 0, 1920, -128, 2432, 1, 1920, 2, 0};
    BC250_SCALER_AXIS_PLAN c;
    BC250_SCALER_PLANE_INPUT in = {0};
    BC250_SCALER_PLANE_PLAN p, zero = {0};
    CHECK(Bc250ScalerComponentAxisPlan(&h, 2, &c) == BC250_SCALER_GEOMETRY_OK);
    CHECK(c.RecoutBegin == 1 && c.RecoutSize == 1919); // no chroma alignment of output clip
    CHECK(c.ViewportBegin == 48 && c.ViewportSize == 721);
    CHECK(c.Ratio19 == 196608 && c.Init19 == 1081344); // 0.375, 2.0625
    h = (BC250_SCALER_AXIS_INPUT){128, 128, 2, 9, 0, 7, 0, 7, 2, 0};
    CHECK(Bc250ScalerComponentAxisPlan(&h, 2, &c) == BC250_SCALER_GEOMETRY_OK);
    CHECK(c.ViewportBegin == 1 && c.ViewportSize == 3);
    CHECK(c.Ratio19 == 262144); // 7/7/2, not floor(7/2)/7
    h.SourceBegin = 1;
    CHECK(Bc250ScalerComponentAxisPlan(&h, 2, &c) == BC250_SCALER_GEOMETRY_UNSUPPORTED && IsZero(&c));
    h.SourceBegin = 2;
    CHECK(Bc250ScalerComponentAxisPlan(&h, 0, &c) == BC250_SCALER_GEOMETRY_INVALID && IsZero(&c));
    CHECK(Bc250ScalerComponentAxisPlan(&h, 3, &c) == BC250_SCALER_GEOMETRY_INVALID && IsZero(&c));
    h = (BC250_SCALER_AXIS_INPUT){8192, 1024, 0, 8192, 0, 1024, 0, 1024, 4, 0};
    CHECK(Bc250ScalerComponentAxisPlan(&h, 2, &c) == BC250_SCALER_GEOMETRY_OK);
    CHECK(c.Ratio19 == 4 * 524288); // do not halve the clamped luma 8:1 ratio
    in.Horizontal = (BC250_SCALER_AXIS_INPUT){1920, 1920, 0, 1920, 0, 1920, 1, 1919, 1, 0};
    in.Vertical = (BC250_SCALER_AXIS_INPUT){1080, 1200, 0, 1080, 0, 1200, 1, 1199, 4, 0};
    in.ChromaDivisor = 2; in.ChromaTapsH = 2; in.ChromaTapsV = 2;
    CHECK(Bc250ScalerPlaneGeometry(&in, &p) == BC250_SCALER_GEOMETRY_OK);
    CHECK(p.ChromaH.RecoutBegin == p.LumaH.RecoutBegin && p.ChromaH.RecoutSize == p.LumaH.RecoutSize);
    CHECK(p.ChromaV.RecoutBegin == p.LumaV.RecoutBegin && p.ChromaV.RecoutSize == p.LumaV.RecoutSize);
    CHECK(p.ChromaH.ViewportBegin + p.ChromaH.ViewportSize <= 960);
    CHECK(p.ChromaV.ViewportBegin + p.ChromaV.ViewportSize <= 540);
    in.ChromaTapsV = 0; // last subplan fails after the other three succeeded
    CHECK(Bc250ScalerPlaneGeometry(&in, &p) == BC250_SCALER_GEOMETRY_INVALID);
    CHECK(memcmp(&p, &zero, sizeof(p)) == 0);
    in.ChromaTapsV = 2;
    in.Vertical.ClipBegin = in.Vertical.ClipEnd;
    CHECK(Bc250ScalerPlaneGeometry(&in, &p) == BC250_SCALER_GEOMETRY_EMPTY);
    CHECK(memcmp(&p, &zero, sizeof(p)) == 0);
    in.Horizontal.SourceEnd = INT_MAX;
    CHECK(Bc250ScalerPlaneGeometry(&in, &p) == BC250_SCALER_GEOMETRY_INVALID);
    CHECK(memcmp(&p, &zero, sizeof(p)) == 0);
    CHECK(Bc250ScalerPlaneGeometry(NULL, &p) == BC250_SCALER_GEOMETRY_INVALID);
    CHECK(Bc250ScalerPlaneGeometry(&in, NULL) == BC250_SCALER_GEOMETRY_INVALID);
}

static void BufferCapacity(void)
{
    BC250_SCALER_BUFFER_INPUT in = {4096, 2048, 4096, 524288, 262144, 5, 2, 1, 0};
    BC250_SCALER_BUFFER_PLAN p, zero = {0};
    const unsigned widths[] = {1, 6, 7, 1280, 1920, 3840, 4096, 7680, 16383};
    const unsigned ratios[] = {1, 524288, 1048576, 1048577, 2097152, 4194303};
    unsigned w, r, taps, alpha, yuv;
    CHECK(Bc250ScalerBufferPlan(&in, &p) == 1);
    CHECK(p.Config == 3 && p.LumaPartitions == 5 && p.ChromaPartitions == 6);
    in.Alpha = 1;
    CHECK(Bc250ScalerBufferPlan(&in, &p) == 0 && memcmp(&p, &zero, sizeof(p)) == 0);
    in.Alpha = 0; in.Is420 = 0;
    CHECK(Bc250ScalerBufferPlan(&in, &p) == 0 && memcmp(&p, &zero, sizeof(p)) == 0);
    in.VerticalTaps = 4;
    CHECK(Bc250ScalerBufferPlan(&in, &p) == 1 && p.Config == 0);
    in.ViewportWidth = in.ChromaViewportWidth = in.RecoutWidth = 16383;
    in.VerticalTaps = in.ChromaVerticalTaps = 2;
    CHECK(Bc250ScalerBufferPlan(&in, &p) == 0 && memcmp(&p, &zero, sizeof(p)) == 0);
    CHECK(Bc250ScalerBufferPlan(NULL, &p) == 0 && memcmp(&p, &zero, sizeof(p)) == 0);
    CHECK(Bc250ScalerBufferPlan(&in, NULL) == 0);
    in.VerticalRatio19 = 8u << 19;
    CHECK(Bc250ScalerBufferPlan(&in, &p) == 0);
    in.VerticalRatio19 = UINT_MAX;
    CHECK(Bc250ScalerBufferPlan(&in, &p) == 0);
    for (w = 0; w < sizeof(widths)/sizeof(widths[0]); ++w)
    for (r = 0; r < sizeof(ratios)/sizeof(ratios[0]); ++r)
    for (taps = 1; taps <= 8; ++taps)
    for (alpha = 0; alpha <= 1; ++alpha)
    for (yuv = 0; yuv <= 1; ++yuv) {
        const unsigned configs[] = {1, 2, 3, 0};
        const unsigned capacity_y[] = {2744, 970, 1290, 3712};
        const unsigned capacity_c[] = {2744, 970, 1290, 2260};
        const unsigned capacity_a[] = {2744, 970, 1290, 2744};
        unsigned required_y, required_c, line_y, line_c, index, found = 0;
        int result;
        in.ViewportWidth = widths[w]; in.ChromaViewportWidth = (widths[w] + yuv) / (1 + yuv);
        in.RecoutWidth = widths[w] / 2 + 1;
        in.VerticalRatio19 = ratios[r];
        in.ChromaVerticalRatio19 = ratios[r] / (1 + yuv);
        if (!in.ChromaVerticalRatio19) in.ChromaVerticalRatio19 = 1;
        in.VerticalTaps = taps; in.ChromaVerticalTaps = (taps + 1) / 2;
        in.Alpha = alpha; in.Is420 = yuv;
        result = Bc250ScalerBufferPlan(&in, &p);
        line_y = in.ViewportWidth < in.RecoutWidth ? in.ViewportWidth : in.RecoutWidth;
        line_c = in.ChromaViewportWidth < in.RecoutWidth ? in.ChromaViewportWidth : in.RecoutWidth;
        line_y = (line_y + 5) / 6; line_c = (line_c + 5) / 6;
        required_y = (in.VerticalRatio19 + 524287) / 524288;
        required_c = (in.ChromaVerticalRatio19 + 524287) / 524288;
        required_y = in.VerticalTaps + (required_y > 2 ? required_y - 2 : 0);
        required_c = in.ChromaVerticalTaps + (required_c > 2 ? required_c - 2 : 0);
        for (index = 0; index < 4; ++index) {
            unsigned cfg = configs[index], py, pc, pa;
            if (cfg == 3 && !yuv) continue;
            py = capacity_y[cfg] / line_y; pc = capacity_c[cfg] / line_c;
            pa = capacity_a[cfg] / line_y;
            if (alpha && pa < py) py = pa;
            if (py > 64) py = 64;
            if (pc > 64) pc = 64;
            if (py < required_y || pc < required_c) continue;
            found = 1;
            CHECK(result == 1 && p.Config == cfg);
            CHECK(p.LumaPartitions == py && p.ChromaPartitions == pc);
            break;
        }
        if (!found) CHECK(result == 0 && memcmp(&p, &zero, sizeof(p)) == 0);
    }
}

int main(int argc, char** argv)
{
    if (argc == 2 && (strcmp(argv[1], "--stream") == 0 || strcmp(argv[1], "--stream-chroma") == 0)) {
        BC250_SCALER_AXIS_INPUT in;
        BC250_SCALER_AXIS_PLAN p;
        int n, status;
        while ((n = scanf_s("%u %u %d %d %d %d %d %d %u %u",
                &in.SurfaceSize, &in.ActiveSize, &in.SourceBegin, &in.SourceEnd,
                &in.DestinationBegin, &in.DestinationEnd, &in.ClipBegin, &in.ClipEnd,
                &in.Taps, &in.Mirror)) == 10) {
            status = Bc250ScalerComponentAxisPlan(&in,
                strcmp(argv[1], "--stream-chroma") == 0 ? 2u : 1u, &p);
            printf("%d %u %u %u %u %u %u\n", status, p.ViewportBegin,
                   p.ViewportSize, p.RecoutBegin, p.RecoutSize, p.Ratio19, p.Init19);
        }
        return n == EOF ? 0 : 2;
    }
    Controls();
    Properties();
    ChromaAndPlane();
    BufferCapacity();
    printf("scaler geometry: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
