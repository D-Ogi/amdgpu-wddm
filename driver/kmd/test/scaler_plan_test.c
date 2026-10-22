#include "../../shim/bc250_scaler_plan.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static BC250_SCALER_REQUEST Request(unsigned sw, unsigned sh, unsigned tw, unsigned th)
{
    BC250_SCALER_REQUEST r = {0};
    r.Geometry.Horizontal = (BC250_SCALER_AXIS_INPUT){sw, tw, 0, (int)sw, 0, (int)tw, 0, (int)tw, 0, 0};
    r.Geometry.Vertical = (BC250_SCALER_AXIS_INPUT){sh, th, 0, (int)sh, 0, (int)th, 0, (int)th, 0, 0};
    r.Geometry.ChromaDivisor = 1;
    return r;
}

static int Zero(const BC250_SCALER_PREPARED* p)
{
    const BC250_SCALER_PREPARED zero = {0};
    return memcmp(p, &zero, sizeof(zero)) == 0;
}

static void Controls(void)
{
    BC250_SCALER_REQUEST r = Request(1920, 1080, 1920, 1200);
    BC250_SCALER_PREPARED p;
    CHECK(Bc250ScalerPrepare(&r, &p) == 0);
    CHECK(p.LumaH.Taps == 1 && p.LumaV.Taps == 4);
    CHECK(p.ChromaH.Taps == 1 && p.ChromaV.Taps == 2);
    CHECK(p.LumaH.CoefficientCount == 0 && p.LumaV.CoefficientCount == 132);
    r.Geometry.ChromaDivisor = 2;
    CHECK(Bc250ScalerPrepare(&r, &p) == 0);
    CHECK(p.ChromaH.Taps == 2 && p.ChromaV.Taps == 2);
    r.FilterKind = BC250_SCALER_FILTER_LINEAR;
    CHECK(Bc250ScalerPrepare(&r, &p) == 0);
    CHECK(p.LumaV.Taps == 2 && p.ChromaV.Coefficients[32] == 3072);
    r = Request(40, 40, 10, 10);
    CHECK(Bc250ScalerPrepare(&r, &p) == 0 && p.LumaH.Taps == 4 && p.LumaV.Taps == 4);
    r = Request(41, 41, 10, 10);
    CHECK(Bc250ScalerPrepare(&r, &p) == 0 && p.LumaH.Taps == 8 && p.LumaV.Taps == 8);
    r.Geometry.ChromaTapsH = 7;
    CHECK(Bc250ScalerPrepare(&r, &p) == 0 && p.ChromaH.Taps == 6);
    r = Request(80, 80, 10, 10);
    CHECK(Bc250ScalerPrepare(&r, &p) == 0 && p.Geometry.LumaH.Ratio19 == 4194303);
    r = Request(81, 80, 10, 10);
    CHECK(Bc250ScalerPrepare(&r, &p) == BC250_SCALER_GEOMETRY_UNSUPPORTED && Zero(&p));
    r = Request(4116, 720, 5000, 1200);
    CHECK(Bc250ScalerPrepare(&r, &p) == 0 && p.Buffer.Config == 0);
    r = Request(4117, 720, 5000, 1200);
    CHECK(Bc250ScalerPrepare(&r, &p) == BC250_SCALER_GEOMETRY_UNSUPPORTED && Zero(&p));
    r.FilterKind = BC250_SCALER_FILTER_LINEAR;
    CHECK(Bc250ScalerPrepare(&r, &p) == 0 && p.LumaV.Taps == 2);
    r.Geometry.Horizontal.Taps = 4;
    CHECK(Bc250ScalerPrepare(&r, &p) == BC250_SCALER_GEOMETRY_INVALID && Zero(&p));
    r = Request(1920, 1080, 1920, 1200);
    r.Geometry.Horizontal.ClipEnd = 0;
    CHECK(Bc250ScalerPrepare(&r, &p) == BC250_SCALER_GEOMETRY_EMPTY && Zero(&p));
    r.Geometry.Vertical.SourceEnd = 1081;
    CHECK(Bc250ScalerPrepare(&r, &p) == BC250_SCALER_GEOMETRY_INVALID && Zero(&p));
    CHECK(Bc250ScalerPrepare(NULL, &p) == BC250_SCALER_GEOMETRY_INVALID && Zero(&p));
    CHECK(Bc250ScalerPrepare(&r, NULL) == BC250_SCALER_GEOMETRY_INVALID);
}

static void Composition(void)
{
    const unsigned source[] = {2, 3, 16, 41, 240, 1920, 4117, 16383};
    const unsigned target[] = {2, 10, 240, 1920, 5000, 16383};
    unsigned s, t, div, kind, mirror, clip, accepted = 0, refused = 0;
    for (s = 0; s < sizeof(source)/sizeof(source[0]); ++s)
    for (t = 0; t < sizeof(target)/sizeof(target[0]); ++t)
    for (div = 1; div <= 2; ++div)
    for (kind = 0; kind <= 1; ++kind)
    for (mirror = 0; mirror <= 1; ++mirror)
    for (clip = 0; clip < 3; ++clip) {
        BC250_SCALER_REQUEST r = Request(source[s], source[(s + 3) % 8], target[t], target[(t + 2) % 6]);
        BC250_SCALER_PREPARED p;
        BC250_SCALER_PLANE_PLAN geometry;
        BC250_SCALER_PLANE_INPUT selected;
        BC250_SCALER_BUFFER_INPUT input = {0};
        BC250_SCALER_BUFFER_PLAN buffer;
        BC250_SCALER_FILTER_PLAN filter;
        unsigned original_taps;
        int result;
        r.Geometry.ChromaDivisor = div; r.FilterKind = kind; r.Alpha = mirror;
        r.Geometry.Horizontal.Mirror = mirror;
        r.Geometry.Vertical.Mirror = 1 - mirror;
        if (clip == 1) {
            r.Geometry.Horizontal.DestinationBegin = -7;
            r.Geometry.Horizontal.ClipBegin = 1;
            r.Geometry.Vertical.ClipBegin = 1;
        } else if (clip == 2) {
            r.Geometry.Horizontal.SourceBegin = 1; // 420 alignment refusal
        }
        memset(&p, 0xa5, sizeof(p));
        result = Bc250ScalerPrepare(&r, &p);
        if (result != 0) { CHECK(Zero(&p)); ++refused; continue; }
        ++accepted;
        selected = r.Geometry;
        selected.Horizontal.Taps = p.LumaH.Taps; selected.Vertical.Taps = p.LumaV.Taps;
        selected.ChromaTapsH = p.ChromaH.Taps; selected.ChromaTapsV = p.ChromaV.Taps;
        CHECK(Bc250ScalerPlaneGeometry(&selected, &geometry) == 0);
        CHECK(memcmp(&geometry, &p.Geometry, sizeof(geometry)) == 0);
        input.ViewportWidth = geometry.LumaH.ViewportSize;
        input.ChromaViewportWidth = geometry.ChromaH.ViewportSize;
        input.RecoutWidth = geometry.LumaH.RecoutSize;
        input.VerticalRatio19 = geometry.LumaV.Ratio19;
        input.ChromaVerticalRatio19 = geometry.ChromaV.Ratio19;
        input.VerticalTaps = p.LumaV.Taps; input.ChromaVerticalTaps = p.ChromaV.Taps;
        input.Is420 = div == 2; input.Alpha = r.Alpha;
        CHECK(Bc250ScalerBufferPlan(&input, &buffer));
        CHECK(memcmp(&buffer, &p.Buffer, sizeof(buffer)) == 0);
        CHECK(Bc250ScalerFilterPlan(kind, p.ChromaH.Taps, geometry.ChromaH.Ratio19, &filter));
        CHECK(memcmp(&filter, &p.ChromaH, sizeof(filter)) == 0);
        original_taps = p.LumaV.Taps;
        CHECK(original_taps == (geometry.LumaV.Ratio19 == 524288 ? 1u : kind ? 2u :
              geometry.LumaV.Ratio19 > 4 * 524288 ? 8u : 4u));
        CHECK(p.ChromaH.Taps == 1 || p.ChromaH.Taps % 2 == 0);
    }
    CHECK(accepted > 100 && refused > 100);
    printf("Prepared plans: %u accepted, %u empty/refused\n", accepted, refused);
}

int main(void)
{
    Controls(); Composition();
    printf("scaler plan: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
