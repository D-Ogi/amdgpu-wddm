#include "../present_range.h"
#include "gdi_private.h"
// Host controls for the supplied DCN address geometry. All windows are synthetic,
// including the retained 256 MiB fixture. Larger fixtures prove arithmetic only,
// not firmware support, installed RAM, usable application capacity or residency.
#include <stdio.h>
#include "../dcn_translate.h"

static int g_failures, g_checks;

#define CHECK(cond) \
    do { g_checks++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

typedef struct { unsigned long long Pages[4]; int Missing,System,Calls; } RANGE_TEST;
static int RangeTranslate(void* Context,unsigned long long Va,unsigned long long* Physical,int* System)
{
    RANGE_TEST* t=(RANGE_TEST*)Context;
    unsigned page=(unsigned)(Va/4096);
    t->Calls++;
    if (page>=4 || (int)page==t->Missing) return 0;
    *Physical=t->Pages[page]+(Va&4095); *System=(int)page==t->System;
    return 1;
}
static void PresentRanges(void)
{
    RANGE_TEST t={{0x10000,0x11000,0x12000,0x13000},-1,-1,0};
    unsigned long long first=7,last=9;
    CHECK(Bc250PresentVramRange(&t,RangeTranslate,0,4*4096,&first,&last));
    CHECK(first==0x10000 && last==0x13fff && t.Calls==4);
    t.Calls=0;
    CHECK(Bc250PresentVramRange(&t,RangeTranslate,4095,4098,&first,&last));
    CHECK(first==0x10fff && last==0x12000 && t.Calls==3);
    // Matching first/last physical addresses conceal a foreign middle page.
    t.Pages[1]=0x90000; first=7;last=9;
    CHECK(!Bc250PresentVramRange(&t,RangeTranslate,0,4*4096,&first,&last));
    CHECK(first==7 && last==9);
    t.Pages[1]=0x11000;t.Missing=2;
    CHECK(!Bc250PresentVramRange(&t,RangeTranslate,0,4*4096,&first,&last));
    t.Missing=-1;t.System=1;
    CHECK(!Bc250PresentVramRange(&t,RangeTranslate,0,4*4096,&first,&last));
    t.System=-1;t.Calls=0;
    CHECK(!Bc250PresentVramRange(&t,RangeTranslate,0,0,&first,&last));
    CHECK(!Bc250PresentVramRange(&t,RangeTranslate,~0ull,2,&first,&last));
    CHECK(!Bc250PresentVramRange(&t,RangeTranslate,0,1,0,&last));
    CHECK(!Bc250PresentVramRange(&t,0,0,1,&first,&last));
    CHECK(t.Calls==0);
    t.Pages[0]=~0ull-100;
    CHECK(!Bc250PresentVramRange(&t,RangeTranslate,0,4096,&first,&last));
    CHECK(first==7 && last==9);
}

static void Geometry(unsigned long long mcBase, unsigned long long vramBase,
                     unsigned long long vramLength)
{
    const unsigned long long surfaceBytes = 1920ull * 1200ull * 4ull;    // BC250_DCNFLIP_SURFACE_BYTES's own shape
    unsigned long long physical;

    // 1. Offset zero translates to the supplied CPU-visible origin.
    CHECK(DcnTranslateCardAddress(mcBase, mcBase, vramBase, vramLength, &physical));
    CHECK(physical == vramBase);
    CHECK(DcnAddressFits(physical, vramBase, vramLength, surfaceBytes));

    // 2. An offset inside the supplied window.
    CHECK(DcnTranslateCardAddress(mcBase + 0x1000000ull, mcBase, vramBase, vramLength, &physical));
    CHECK(physical == vramBase + 0x1000000ull);
    CHECK(DcnAddressFits(physical, vramBase, vramLength, surfaceBytes));

    // 3. Below McBase: refused, *Physical zeroed rather than left whatever the caller had in it.
    physical = 0xDEADBEEFull;
    CHECK(!DcnTranslateCardAddress(mcBase - 0x1000ull, mcBase, vramBase, vramLength, &physical));
    CHECK(physical == 0);

    // 4. At, and past, the top of the carve-out: refused either way (a half-open range).
    CHECK(!DcnTranslateCardAddress(mcBase + vramLength, mcBase, vramBase, vramLength, &physical));
    CHECK(!DcnTranslateCardAddress(mcBase + vramLength + 0x1000ull, mcBase, vramBase, vramLength, &physical));

    // 5. Translates fine but is not 4 KiB aligned: DcnAddressFits refuses it, the escape's own rule.
    CHECK(DcnTranslateCardAddress(mcBase + 0x1004ull, mcBase, vramBase, vramLength, &physical));
    CHECK(!DcnAddressFits(physical, vramBase, vramLength, surfaceBytes));

    // 6. Aligned, inside the carve-out, but the fixed surface would run past its top.
    CHECK(DcnTranslateCardAddress(mcBase + vramLength - 0x1000ull, mcBase, vramBase, vramLength, &physical));
    CHECK(!DcnAddressFits(physical, vramBase, vramLength, surfaceBytes));

    // 7. A zero-length carve-out (VramStart's own zeroed state with EnableVram closed): every address refused.
    physical = 0xDEADBEEFull;
    CHECK(!DcnTranslateCardAddress(mcBase, mcBase, vramBase, 0, &physical));

    // Last complete pitched surface fits, including windows larger than 8 GiB.
    CHECK(DcnTranslateCardAddress(mcBase + vramLength - surfaceBytes,
                                 mcBase, vramBase, vramLength, &physical));
    CHECK(physical == vramBase + vramLength - surfaceBytes);
    CHECK(DcnAddressFits(physical, vramBase, vramLength, surfaceBytes));
    if (vramLength > (8ull << 30)) {
        CHECK(DcnTranslateCardAddress(mcBase + (8ull << 30), mcBase, vramBase, vramLength, &physical));
        CHECK(physical == vramBase + (8ull << 30));
        CHECK(DcnAddressFits(physical, vramBase, vramLength, surfaceBytes));
    }
}

static void GdiAllocationPolicies(void)
{
    /* WDK26100 contract fixtures: type, shared/cached request, then expected
     * CPU visibility, aperture, cache and physical access. Legacy rows preserve
     * existing resource policy; standard rows override irrelevant UMD hints. */
    const int rows[][7]={
        {0,0,0,1,0,0,1},{0,0,1,1,0,0,1},
        {0,1,0,1,1,0,0},{0,1,1,1,1,1,0},
        {1,1,1,0,0,0,0},{2,0,0,1,1,1,0},
        {3,1,1,0,0,0,0},{4,1,1,0,0,0,0}
    };
    unsigned int i;
    BC250_GDI_ALLOCATION_POLICY p;
    for(i=0;i<sizeof(rows)/sizeof(rows[0]);i++) {
        CHECK(WddmGdiAllocationPolicy((unsigned long)rows[i][0],rows[i][1],rows[i][2],&p));
        CHECK(p.CpuVisible==rows[i][3] && p.Aperture==rows[i][4] &&
            p.Cached==rows[i][5] && p.AccessedPhysically==rows[i][6]);
    }
    for(i=5;i<10;i++) {
        p.CpuVisible=p.Aperture=p.Cached=p.AccessedPhysically=7;
        CHECK(!WddmGdiAllocationPolicy(i,1,1,&p));
        CHECK(p.CpuVisible==7 && p.Aperture==7 && p.Cached==7 && p.AccessedPhysically==7);
    }
    CHECK(!WddmGdiAllocationPolicy(0xfffffffful,0,0,&p));
    CHECK(!WddmGdiAllocationPolicy(1,0,0,0));
}

int main(void)
{
    unsigned long w,h,pitch;unsigned long long bytes;
    BC250_GDI_PRIVATE gdi={0};
    unsigned long type=99;
    unsigned int n;
    typedef char GdiAbiSize[(sizeof(gdi.Surface)==32 && sizeof(gdi)==48)?1:-1];
    GdiAbiSize abi={0};
    GdiAllocationPolicies();
    CHECK(abi[0]==0);
    CHECK(!WddmGdiPrivate(0,32,&type) && type==0);
    CHECK(WddmGdiPrivate(&gdi,32,&type) && type==0);
    gdi.Surface.Version=1;gdi.Magic=BC250_GDI_PRIVATE_MAGIC;
    for(n=1;n<=8;n++) {
        gdi.Type=n;
        CHECK(WddmGdiPrivate(&gdi,48,&type) && type==n);
    }
    for(n=0;n<64;n++) if(n!=32 && n!=48)
        CHECK(!WddmGdiPrivate(&gdi,n,&type) && type==0);
    gdi.Type=0;CHECK(!WddmGdiPrivate(&gdi,48,&type));
    gdi.Type=9;CHECK(!WddmGdiPrivate(&gdi,48,&type));
    gdi.Type=2;gdi.Flags=1;CHECK(!WddmGdiPrivate(&gdi,48,&type));
    gdi.Flags=0;gdi.Reserved=1;CHECK(!WddmGdiPrivate(&gdi,48,&type));
    gdi.Reserved=0;gdi.Magic^=1;CHECK(!WddmGdiPrivate(&gdi,48,&type));
    gdi.Magic^=1;gdi.Surface.Version=2;CHECK(!WddmGdiPrivate(&gdi,48,&type));
    /* Validate the received blob independently of producer allocation calls. */
    gdi.Surface.Magic=BC250_WDDM_ALLOCATION_PRIVATE_MAGIC;
    gdi.Surface.Version=1;
    for(w=1;w<=129;w++)for(h=1;h<=9;h++) {
        unsigned long kind;
        gdi.Surface.Width=w;gdi.Surface.Height=h;
        for(kind=1;kind<=8;kind++) {
            unsigned long bpp=(kind>=2 && kind<=4)?1:4;
            CHECK(WddmGdiLayout(w,h,kind,bpp,&gdi.Surface.Pitch,&gdi.Surface.Size));
            CHECK(WddmSurfaceGeometry(&gdi.Surface,kind,bpp));
            gdi.Surface.Size--;
            CHECK(!WddmSurfaceGeometry(&gdi.Surface,kind,bpp));
            gdi.Surface.Size+=2;
            CHECK(!WddmSurfaceGeometry(&gdi.Surface,kind,bpp));
            gdi.Surface.Size--;
            gdi.Surface.Pitch+=4;
            CHECK(!WddmSurfaceGeometry(&gdi.Surface,kind,bpp));
        }
    }
    gdi.Surface.Width=3;gdi.Surface.Height=3;
    gdi.Surface.Pitch=256;gdi.Surface.Size=1024;
    CHECK(WddmSurfaceGeometry(&gdi.Surface,0,4)); /* legacy padded UMD */
    CHECK(!WddmSurfaceGeometry(&gdi.Surface,0,1));
    gdi.Surface.Size=767;CHECK(!WddmSurfaceGeometry(&gdi.Surface,0,4));
    gdi.Surface.Size=~0ull;CHECK(!WddmSurfaceGeometry(&gdi.Surface,0,4));
    gdi.Surface.Size=1024;gdi.Surface.Version=2;
    CHECK(!WddmSurfaceGeometry(&gdi.Surface,0,4));
    gdi.Surface.Version=1;gdi.Surface.Magic^=1;
    CHECK(!WddmSurfaceGeometry(&gdi.Surface,0,4));
    gdi.Surface.Magic^=1;gdi.Surface.Pitch=8;
    CHECK(!WddmSurfaceGeometry(&gdi.Surface,0,4));
    gdi.Surface.Pitch=13;gdi.Surface.Size=1024;
    CHECK(!WddmSurfaceGeometry(&gdi.Surface,0,4)); /* misaligned legacy pitch */
    gdi.Surface.Width=1;gdi.Surface.Height=0xfffffffeul;
    gdi.Surface.Pitch=0xfffffffcul;
    gdi.Surface.Size=(unsigned long long)0xfffffffcul*0xfffffffeul;
    CHECK(WddmSurfaceGeometry(&gdi.Surface,0,4)); /* 64-bit product */
    gdi.Surface.Size--;
    CHECK(!WddmSurfaceGeometry(&gdi.Surface,0,4));
    CHECK(!WddmSurfaceGeometry(&gdi.Surface,0,0)); /* unsupported format */
    CHECK(!WddmGdiLayout(3,3,4,4,&pitch,&bytes)); /* lookup is A8 only */
    CHECK(WddmGdiLayout(257,1,4,1,&pitch,&bytes) && pitch==260 && bytes==260);
    for(w=1;w<=129;w++)for(h=1;h<=9;h++) {
        gdi.Surface.Width=w;gdi.Surface.Height=h;
        /* Mesa DxgiFns shared-surface layout, independently calculated. */
        gdi.Surface.Pitch=((w*4u+255u)/256u)*256u;
        gdi.Surface.Size=(unsigned long long)gdi.Surface.Pitch*((h+3u)/4u)*4u;
        CHECK(WddmSurfaceGeometry(&gdi.Surface,0,4));
    }
    CHECK(!WddmGdiLayout(3,3,1,1,&pitch,&bytes));
    CHECK(!WddmGdiLayout(3,3,2,0,&pitch,&bytes));
    for(w=1;w<=129;w++)for(h=1;h<=9;h++) {
        CHECK(DcnSharedTextureLayout(w,h,&pitch,&bytes));
        // Independent consumer contract: Resource.cpp OpenResource on a0ad8af5.
        CHECK(!(pitch&15u));
        CHECK(pitch>=((w+3u)&~3u)*4u);
        CHECK(bytes>=(unsigned long long)pitch*((h+3u)&~3u));
    }
    CHECK(DcnSharedTextureLayout(1,1,&pitch,&bytes) && pitch==256 && bytes==1024);
    CHECK(!DcnSharedTextureLayout(0,1,&pitch,&bytes) && !pitch && !bytes);
    CHECK(!DcnSharedTextureLayout(1,0,&pitch,&bytes) && !pitch && !bytes);
    CHECK(!DcnSharedTextureLayout(1,0xfffffffful,&pitch,&bytes) && !pitch && !bytes);
    CHECK(!DcnSharedTextureLayout(0xfffffffful,1,&pitch,&bytes) && !pitch && !bytes);
    for(w=1;w<=129;w++)for(h=1;h<=9;h++) {
        CHECK(DcnStagingLayout(w,h,1,&pitch,&bytes));
        CHECK(!(pitch&3u) && pitch>=w && pitch-w<4);
        CHECK(bytes==(unsigned long long)pitch*h);
        CHECK(DcnStagingLayout(w,h,4,&pitch,&bytes));
        CHECK(pitch==w*4u && bytes==(unsigned long long)w*4u*h);
    }
    CHECK(!DcnStagingLayout(1,1,2,&pitch,&bytes) && !pitch && !bytes);
    CHECK(!DcnStagingLayout(0,1,1,&pitch,&bytes) && !pitch && !bytes);
    CHECK(!DcnStagingLayout(1,0,4,&pitch,&bytes) && !pitch && !bytes);
    CHECK(!DcnStagingLayout(0xfffffffful,1,1,&pitch,&bytes) && !pitch && !bytes);
    CHECK(!DcnStagingLayout(0x40000000ul,1,4,&pitch,&bytes) && !pitch && !bytes);
    PresentRanges();
    Geometry(0xF400000000ull, 0x270000000ull, 256ull << 20);
    Geometry(0xF400000000ull, 0x270000000ull, 8ull << 30);
    Geometry(0xE800000000ull, 0x670000000ull, 12ull << 30);
    Geometry(0xDC00000000ull, 0x1270000000ull, 16ull << 30);
    printf("dcn_translate_test: 4 synthetic geometries, %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
