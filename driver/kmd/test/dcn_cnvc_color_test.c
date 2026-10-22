#include "../dcn_cnvc_color.h"
#include "../plane_format.h"
#include "../display_io.h"
#include "dcn_2_0_1_sh_mask.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %d: %s\n",__LINE__,#x); } } while (0)
static void Refused(BC250_CNVC_REQUEST r)
{
    BC250_CNVC_PLAN p, zero = {0};
    memset(&p,0xff,sizeof(p));
    CHECK(!Bc250CnvcPrepare(&r,&p)); CHECK(!memcmp(&p,&zero,sizeof(p)));
}
static void Formats(void)
{
    unsigned f, c, i;
    static const unsigned expected[] = {8,8,10,12,8};
    for (f = 0; f < BC250_LINEAR_FORMAT_COUNT; ++f)
    for (c = 0; c < BC250_INPUT_COLOR_COUNT; ++c) {
        BC250_CNVC_REQUEST r = {f,c};
        BC250_CNVC_PLAN p;
        unsigned yuv = f == BC250_LINEAR_AMD_AYCRCB8;
        if (yuv != (c != BC250_INPUT_RGB_FULL)) { Refused(r); continue; }
        CHECK(Bc250CnvcPrepare(&r,&p));
        CHECK(p.Ready && p.Format == f && p.Color == c && p.CscRequired == yuv);
        CHECK(p.PixelFormat == expected[f]);
        CHECK(p.HasPixelAlpha == (f != BC250_LINEAR_BGRX8));
        CHECK(p.FormatControl == (CNVC_CFG0_FORMAT_CONTROL__FORMAT_EXPANSION_MODE_MASK |
              (p.HasPixelAlpha ? CNVC_CFG0_FORMAT_CONTROL__ALPHA_EN_MASK : 0ul)));
        CHECK(p.WriteAlphaLut == (f == BC250_LINEAR_RGB10A2));
        CHECK(p.AlphaLut == (p.WriteAlphaLut ? 0xffaa5500ul : 0ul));
        // The fixed two-bit alpha expansion has both endpoints and equal steps.
        if (p.WriteAlphaLut) for (i = 0; i < 4; ++i)
            CHECK(((p.AlphaLut >> (i*8)) & 255u) == i*85u);
        if (!yuv) for (i = 0; i < 12; ++i) CHECK(p.Matrix[i] == 0);
        if (f < 3) CHECK(p.PixelFormat == Bc250PlaneEncoding(f+1)->CnvcPixelFormat);
        // Reusing plan storage must not retain CSC coefficients or an old LUT.
        memset(&p,0xff,sizeof(p));
        CHECK(Bc250CnvcPrepare(&r,&p));
        CHECK(p.AlphaLut == (p.WriteAlphaLut ? 0xffaa5500ul : 0ul));
        if (!yuv) for (i = 0; i < 12; ++i) CHECK(p.Matrix[i] == 0);
    }
    Refused((BC250_CNVC_REQUEST){BC250_LINEAR_FORMAT_COUNT,0});
    Refused((BC250_CNVC_REQUEST){UINT32_MAX,0});
    Refused((BC250_CNVC_REQUEST){0,BC250_INPUT_COLOR_COUNT});
    Refused((BC250_CNVC_REQUEST){0,UINT32_MAX});
}
static void AliasingAndNull(void)
{
    BC250_CNVC_REQUEST r = {BC250_LINEAR_AMD_AYCRCB8,BC250_INPUT_709_LIMITED};
    BC250_CNVC_PLAN p, zero = {0}, reference;
    union { BC250_CNVC_REQUEST request; BC250_CNVC_PLAN plan; } overlap;
    CHECK(Bc250CnvcPrepare(&r,&reference));
    memset(&overlap,0xff,sizeof(overlap)); overlap.request = r;
    CHECK(Bc250CnvcPrepare(&overlap.request,&overlap.plan));
    CHECK(!memcmp(&reference,&overlap.plan,sizeof(reference)));
    overlap.request.Format = UINT32_MAX;
    CHECK(!Bc250CnvcPrepare(&overlap.request,&overlap.plan));
    CHECK(!memcmp(&zero,&overlap.plan,sizeof(zero)));
    memset(&p,0xff,sizeof(p));
    CHECK(!Bc250CnvcPrepare(NULL,&p)); CHECK(!memcmp(&p,&zero,sizeof(p)));
    CHECK(!Bc250CnvcPrepare(&r,NULL)); CHECK(!Bc250CnvcPrepare(NULL,NULL));
}
int main(int argc, char** argv)
{
    if (argc == 2 && !strcmp(argv[1],"--matrices")) {
        unsigned c, i;
        for (c = BC250_INPUT_601_FULL; c <= BC250_INPUT_709_LIMITED; ++c) {
            BC250_CNVC_REQUEST r = {BC250_LINEAR_AMD_AYCRCB8,c};
            BC250_CNVC_PLAN p;
            if (!Bc250CnvcPrepare(&r,&p)) return 1;
            printf("%u",c);
            for (i = 0; i < 12; ++i) printf(" %u",(unsigned)p.Matrix[i]);
            puts("");
        }
        return 0;
    }
    Formats(); AliasingAndNull();
    printf("CNVC color and alpha: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
