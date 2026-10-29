#include "../dcn_surface_layout.h"
#include "../display_io.h"
#include "dcn_2_0_1_sh_mask.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %d: %s\n",__LINE__,#x); } } while (0)
static unsigned long long Page(unsigned long long n) { return (n + 4095) & ~4095ull; }
static BC250_NV12_LAYOUT_REQUEST Request(unsigned width, unsigned height, unsigned extraChroma)
{
    BC250_NV12_LAYOUT_REQUEST r = {0};
    r.Width = width; r.Height = height;
    r.PitchBytes = (width + 255) & ~255u;
    r.ChromaPitchBytes = r.PitchBytes + extraChroma;
    r.McBase = 0x10000000000ull; r.VramPhysical = 0x300000000ull; r.VramLength = 1ull << 30;
    r.AllocationMcAddress = r.McBase + 8192;
    r.ChromaOffset = Page((unsigned long long)r.PitchBytes * height);
    r.AllocationBytes = r.ChromaOffset + (unsigned long long)r.ChromaPitchBytes * (height / 2);
    return r;
}
static void Refused(BC250_NV12_LAYOUT_REQUEST r)
{
    BC250_NV12_LAYOUT p, zero = {0};
    memset(&p,0xff,sizeof(p));
    CHECK(!Bc250Nv12LayoutPrepare(&r,&p));
    CHECK(!memcmp(&p,&zero,sizeof(p)));
}
static void CheckPlan(const BC250_NV12_LAYOUT_REQUEST* r)
{
    BC250_NV12_LAYOUT p;
    unsigned long long base = r->VramPhysical + r->AllocationMcAddress - r->McBase;
    CHECK(Bc250Nv12LayoutPrepare(r,&p));
    CHECK(p.Ready == 1 && p.Width == r->Width && p.Height == r->Height);
    CHECK(p.PitchBytes == r->PitchBytes && p.ChromaPitchBytes == r->ChromaPitchBytes);
    CHECK(p.Pitch == r->PitchBytes - 1 && p.PitchC == r->ChromaPitchBytes / 2 - 1);
    // NV12 maps to the AMD YCrCb spelling, encoded 64. NV21 would be 65.
    CHECK(Bc250DispGet(p.SurfaceConfig,HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT_MASK) == 64);
    CHECK(Bc250DispGet(p.Crossbar,HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CB_B_MASK) == 2);
    CHECK(Bc250DispGet(p.Crossbar,HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CR_R_MASK) == 3);
    CHECK(p.Physical == base + r->SurfaceOffset && p.ChromaPhysical == base + r->ChromaOffset);
    CHECK((((unsigned long long)p.AddressHigh << 32) | p.AddressLow) == p.Physical);
    CHECK((((unsigned long long)p.ChromaAddressHigh << 32) | p.ChromaAddressLow) == p.ChromaPhysical);
    CHECK(p.SurfaceBytes == (unsigned long long)r->PitchBytes * r->Height);
    CHECK(p.ChromaBytes == (unsigned long long)r->ChromaPitchBytes * (r->Height / 2));
}
static void Cases(void)
{
    static const unsigned heights[] = {2,1080,16382};
    unsigned width,h,extra;
    for (width = 2; width < 16383; width += 254)
    for (h = 0; h < sizeof(heights)/sizeof(heights[0]); ++h)
    for (extra = 0; extra <= 512; extra += 256) {
        BC250_NV12_LAYOUT_REQUEST r = Request(width,heights[h],extra);
        CheckPlan(&r);
        --r.AllocationBytes; Refused(r); // Exact UV end, not merely the Y end.
    }
    BC250_NV12_LAYOUT_REQUEST r = Request(1920,1080,256);
    CheckPlan(&r);
    // A gap is explicit allocation layout, not silently inferred from Y size.
    r.ChromaOffset += 8192; r.AllocationBytes += 8192; CheckPlan(&r);
    // Either order is valid when the actual component ranges are disjoint.
    r.ChromaOffset = 0;
    r.SurfaceOffset = Page((unsigned long long)r.ChromaPitchBytes * (r.Height/2));
    r.AllocationBytes = r.SurfaceOffset + (unsigned long long)r.PitchBytes * r.Height;
    CheckPlan(&r);
    --r.AllocationBytes; Refused(r); // Exact Y end when Y comes last.
    r = Request(16382,16382,0); r.ChromaPitchBytes = 32768;
    r.AllocationBytes = r.ChromaOffset + (unsigned long long)r.ChromaPitchBytes * (r.Height/2);
    CheckPlan(&r); // Both pitch fields at their maximum sample count.
}
static void Boundaries(void)
{
    BC250_NV12_LAYOUT_REQUEST base = Request(1920,1080,256), r;
    BC250_NV12_LAYOUT p, zero = {0};
    union { BC250_NV12_LAYOUT_REQUEST r; BC250_NV12_LAYOUT p; } alias;
#define BAD(field,value) do { r = base; r.field = (value); Refused(r); } while (0)
    BAD(Width,0); BAD(Height,0); BAD(Width,1919); BAD(Height,1079);
    BAD(Width,16384); BAD(Height,16384); BAD(Width,2050);
    BAD(PitchBytes,0); BAD(PitchBytes,1792); BAD(PitchBytes,1920); BAD(PitchBytes,2049);
    BAD(PitchBytes,16640); BAD(PitchBytes,UINT32_MAX);
    BAD(ChromaPitchBytes,0); BAD(ChromaPitchBytes,1792); BAD(ChromaPitchBytes,1920);
    BAD(ChromaPitchBytes,2305); BAD(ChromaPitchBytes,33024); BAD(ChromaPitchBytes,UINT32_MAX);
    BAD(ChromaOffset,0); BAD(ChromaOffset,base.ChromaOffset-4096); BAD(ChromaOffset,base.ChromaOffset+1);
    BAD(ChromaOffset,base.AllocationBytes); BAD(ChromaOffset,UINT64_MAX);
    BAD(SurfaceOffset,1); BAD(SurfaceOffset,base.ChromaOffset); BAD(SurfaceOffset,UINT64_MAX);
    BAD(AllocationBytes,0); BAD(AllocationBytes,UINT64_MAX);
    BAD(AllocationMcAddress,base.McBase-1); BAD(AllocationMcAddress,base.McBase+base.VramLength);
    BAD(VramLength,0); BAD(VramLength,UINT64_MAX); BAD(VramPhysical,1ull<<48); BAD(VramPhysical,UINT64_MAX);
    r = base; r.VramPhysical = (1ull<<48)-r.VramLength+1; Refused(r);
    r = base; r.McBase = UINT64_MAX-4095; r.AllocationMcAddress = r.McBase; r.VramLength = 4096; Refused(r);
    r = base; r.AllocationMcAddress = r.McBase+r.VramLength-r.AllocationBytes+1; Refused(r);
    // UV overlaps only Y row padding, not its two active bytes. Both ranges
    // fit the allocation, so the overlap rule itself must refuse this.
    r = Request(2,2,0); r.PitchBytes = 8192; r.ChromaOffset = 12288;
    r.AllocationBytes = 32768; Refused(r);
    r = Request(2,2,0); r.VramLength = 8192; r.AllocationMcAddress = r.McBase;
    r.VramPhysical = (1ull<<48)-8192; CheckPlan(&r);
    CHECK(Bc250Nv12LayoutPrepare(&r,&p) && p.ChromaAddressHigh == 0xffff && p.ChromaAddressLow == 0xfffff000ul);
    r.VramPhysical = 0; Refused(r); // Zero luma address is refused by the selected AMD writer.
    r = base; r.VramPhysical = 0x4ffffe000ull; // Y and UV cross a high-word boundary.
    CheckPlan(&r);
    CHECK(Bc250Nv12LayoutPrepare(&r,&p) && p.AddressHigh == 5 && p.ChromaAddressHigh == 5);
    r.VramPhysical -= 4096;
    CHECK(Bc250Nv12LayoutPrepare(&r,&p) && p.AddressHigh == 4 && p.ChromaAddressHigh == 5);
    alias.r = r;
    CHECK(Bc250Nv12LayoutPrepare(&alias.r,&alias.p));
    CHECK(!memcmp(&alias.p,&p,sizeof(p)));
    memset(&p,0xff,sizeof(p));
    CHECK(!Bc250Nv12LayoutPrepare(NULL,&p) && !memcmp(&p,&zero,sizeof(p)));
    CHECK(!Bc250Nv12LayoutPrepare(&base,NULL));
#undef BAD
}
int main(void)
{
    Cases(); Boundaries();
    printf("NV12 surface layout: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
