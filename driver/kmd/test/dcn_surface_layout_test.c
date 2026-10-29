#include "../dcn_surface_layout.h"
#include "../plane_format.h"
#include "../display_io.h"
#include "dcn_2_0_1_sh_mask.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %d: %s\n",__LINE__,#x); } } while (0)
static BC250_SURFACE_LAYOUT_REQUEST Request(unsigned format, unsigned width, unsigned height)
{
    BC250_SURFACE_LAYOUT_REQUEST r = {0};
    r.Format = format; r.Width = width; r.Height = height;
    r.PitchBytes = ((width + 31) & ~31u) * 4;
    r.McBase = 0x10000000000ull; r.VramPhysical = 0x300000000ull; r.VramLength = 0x40000000ull;
    r.AllocationMcAddress = r.McBase + 0x2000; r.AllocationBytes = (unsigned long long)r.PitchBytes * height;
    return r;
}
static void Refused(BC250_SURFACE_LAYOUT_REQUEST r)
{
    BC250_SURFACE_LAYOUT p, zero = {0};
    memset(&p,0xff,sizeof(p));
    CHECK(!Bc250SurfaceLayoutPrepare(&r,&p));
    CHECK(!memcmp(&p,&zero,sizeof(p)));
}
static void Cases(void)
{
    unsigned format, width, h;
    static const unsigned heights[] = {1,1080,16383};
    static const unsigned expectedFormat[] = {8,8,10,12,8};
    for (format = 0; format < BC250_LINEAR_FORMAT_COUNT; ++format)
    for (width = 1; width <= 16383; width += 31)
    for (h = 0; h < sizeof(heights)/sizeof(heights[0]); ++h) {
        BC250_SURFACE_LAYOUT_REQUEST r = Request(format,width,heights[h]);
        BC250_SURFACE_LAYOUT p;
        unsigned swapped = format == BC250_LINEAR_RGBA8 || format == BC250_LINEAR_RGB10A2;
        CHECK(Bc250SurfaceLayoutPrepare(&r,&p));
        CHECK(p.Ready && p.Format == format && p.Width == width && p.Height == heights[h]);
        CHECK(p.Physical == r.VramPhysical + 0x2000 && p.SurfaceBytes == r.AllocationBytes);
        CHECK((((unsigned long long)p.AddressHigh << 32) | p.AddressLow) == p.Physical);
        CHECK(p.Pitch == r.PitchBytes / 4 - 1 && p.PitchC == 0);
        CHECK(Bc250DispGet(p.SurfaceConfig,HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT_MASK) == expectedFormat[format]);
        CHECK(Bc250DispGet(p.Crossbar,HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CB_B_MASK) == (swapped ? 3u : 2u));
        CHECK(Bc250DispGet(p.Crossbar,HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CR_R_MASK) == (swapped ? 2u : 3u));
        CHECK(p.HasPixelAlpha == (format != BC250_LINEAR_BGRX8) && p.Yuv == (format == BC250_LINEAR_AMD_AYCRCB8));
        // Exact allocation boundary, including row padding; one missing byte refuses.
        --r.AllocationBytes; Refused(r);
    }
    // The imported encodings agree with the already-shipped RGB format table.
    for (format = 0; format < 3; ++format) {
        BC250_SURFACE_LAYOUT_REQUEST r = Request(format,1920,1080);
        BC250_SURFACE_LAYOUT p;
        const BC250_PLANE_ENCODING* e = Bc250PlaneEncoding(format + 1);
        CHECK(Bc250SurfaceLayoutPrepare(&r,&p) && e != NULL);
        CHECK(Bc250DispGet(p.SurfaceConfig,HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT_MASK) == e->HubpPixelFormat);
        CHECK(Bc250DispGet(p.Crossbar,HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CB_B_MASK) == e->CrossbarCbB);
        CHECK(Bc250DispGet(p.Crossbar,HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CR_R_MASK) == e->CrossbarCrR);
    }
}
static void Boundaries(void)
{
    BC250_SURFACE_LAYOUT_REQUEST base = Request(0,1920,1080), r;
    BC250_SURFACE_LAYOUT p, zero = {0};
    union { BC250_SURFACE_LAYOUT_REQUEST r; BC250_SURFACE_LAYOUT p; } alias;
#define BAD(field,value) do { r = base; r.field = (value); Refused(r); } while (0)
    BAD(Format,BC250_LINEAR_FORMAT_COUNT); BAD(Format,UINT32_MAX);
    BAD(Width,0); BAD(Width,1921); BAD(Width,16384); BAD(Height,0); BAD(Height,16384);
    BAD(PitchBytes,0); BAD(PitchBytes,7681); BAD(PitchBytes,65536+128); BAD(PitchBytes,UINT32_MAX);
    BAD(Rotation,1); BAD(MirrorH,1); BAD(MirrorV,1);
    BAD(AllocationMcAddress,base.McBase-1); BAD(AllocationMcAddress,base.McBase+base.VramLength);
    BAD(AllocationBytes,0); BAD(AllocationBytes,UINT64_MAX);
    BAD(SurfaceOffset,1); BAD(SurfaceOffset,base.AllocationBytes); BAD(SurfaceOffset,UINT64_MAX);
    BAD(VramLength,0); BAD(VramLength,UINT64_MAX);
    BAD(VramPhysical,1ull<<48); BAD(VramPhysical,UINT64_MAX);
    r = base; r.McBase = UINT64_MAX-4095; r.AllocationMcAddress = r.McBase; r.VramLength = 4096; Refused(r);
    r = base; r.VramPhysical = (1ull<<48)-base.VramLength+1; Refused(r);
    r = base; r.VramPhysical += 1; Refused(r);
    r = base; r.AllocationMcAddress = r.McBase+r.VramLength-r.AllocationBytes+1; Refused(r);
    r = Request(0,32,1); r.AllocationMcAddress = r.McBase; r.VramPhysical = (1ull<<48)-4096; r.VramLength = 4096;
    CHECK(Bc250SurfaceLayoutPrepare(&r,&p));
    CHECK(p.AddressHigh == 0xffff && p.AddressLow == 0xfffff000ul);
    r = base; r.SurfaceOffset = 4096; r.AllocationBytes += 4096;
    CHECK(Bc250SurfaceLayoutPrepare(&r,&p) && p.Physical == r.VramPhysical + 0x3000);
    alias.r = r;
    CHECK(Bc250SurfaceLayoutPrepare(&alias.r,&alias.p));
    CHECK(!memcmp(&p,&alias.p,sizeof(p)));
    memset(&p,0xff,sizeof(p));
    CHECK(!Bc250SurfaceLayoutPrepare(NULL,&p) && !memcmp(&p,&zero,sizeof(p)));
    CHECK(!Bc250SurfaceLayoutPrepare(&base,NULL));
    r = Request(BC250_LINEAR_AMD_AYCRCB8,16383,16383);
    CHECK(Bc250SurfaceLayoutPrepare(&r,&p) && p.Pitch == 16383);
#undef BAD
}
int main(void)
{
    Cases(); Boundaries();
    printf("Surface layout: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
