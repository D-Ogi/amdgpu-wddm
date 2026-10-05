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

/* wddm.c's WddmSurfacePixelBytes before the format table, transcribed: every
 * admission the table answers must equal the geometry at these bytes. The
 * table's two additions to type 0 are mapped in SurfaceFormats. */
static unsigned long PixelBytesBeforeTable(unsigned long Format)
{
    if (Format==28) return 1;
    return Format==21 || Format==22 || Format==32 || Format==33 ? 4 : 0;
}

static void SurfaceFormats(void)
{
    static const unsigned long widths[]={1,3,64,127,1366,1920}, heights[]={1,3,79,1080};
    BC250_WDDM_ALLOCATION_PRIVATE s={0};
    unsigned long f,t,rule,bpp,accepted=0,refused=0,accepted31=0,refused31=0,accepted113=0,refused113=0;
    unsigned long long bytes;
    unsigned int i,j;
    int rgb10,rgba8;
    s.Magic=BC250_WDDM_ALLOCATION_PRIVATE_MAGIC;s.Version=1;
    for(f=0;f<256;f++) {
        CHECK(WddmSurfaceFormatBpp(f,BC250_SURFACE_GDI)==PixelBytesBeforeTable(f));
        CHECK(WddmSurfaceFormatBpp(f,BC250_SURFACE_SCANOUT)==(f==21 || f==22 ? 4ul : 0ul));
        CHECK(!WddmSurfaceFormatBpp(f,0));
    }
    /* Shapes: the DXGI shared-surface layout at 4 and 8 bytes a pixel, GDI's
     * 1- and 4-byte layouts, and a pitch one pixel short. Each type and format
     * against the old answer. */
    for(i=0;i<sizeof(widths)/sizeof(widths[0]);i++)for(j=0;j<sizeof(heights)/sizeof(heights[0]);j++)
    for(t=0;t<=8;t++)for(rule=0;rule<5;rule++) {
        s.Width=widths[i];s.Height=heights[j];
        if (!rule || rule==4) {
            s.Pitch=(s.Width*(rule ? 8u : 4u)+255u)&~255ul;
            s.Size=(unsigned long long)s.Pitch*((s.Height+3u)&~3ul);
        } else if (rule<3) {
            if (!WddmGdiLayout(s.Width,s.Height,t ? t : 2,rule==1 ? 1 : 4,&s.Pitch,&s.Size)) continue;
        } else {
            s.Pitch=s.Width*4u-4u;s.Size=(unsigned long long)s.Width*4u*s.Height;
        }
        for(f=0;f<256;f++) {
            s.Format=f;
            /* The table's changes are type 0 only: A2B10G10R10 (DXGI
             * R10G10B10A2) at 4 bytes a pixel, admitted wherever A8B8G8R8 is,
             * and A16B16G16R16F (DXGI R16G16B16A16_FLOAT) at 8. */
            bpp=f==113 && !t ? 8 : PixelBytesBeforeTable(f==31 && !t ? 32 : f);
            CHECK(WddmSurfaceAdmitted(&s,t)==WddmSurfaceGeometry(&s,t,bpp));
            if (WddmSurfaceAdmitted(&s,t)) accepted++; else refused++;
        }
        s.Format=31;rgb10=WddmSurfaceAdmitted(&s,t);
        s.Format=32;rgba8=WddmSurfaceAdmitted(&s,t);
        CHECK(t ? !rgb10 : rgb10==rgba8);
        if (!t) { if (rgb10) accepted31++; else refused31++; }
        /* FP16 only where the pitch holds 8 bytes a pixel, and never at a GDI type. */
        s.Format=113;
        CHECK(WddmSurfaceAdmitted(&s,t)==(!t && DcnLinearSurfaceBytes(s.Width,s.Height,s.Pitch,8,&bytes) && s.Size>=bytes));
        if (!t) { if (WddmSurfaceAdmitted(&s,t)) accepted113++; else refused113++; }
    }
    CHECK(accepted && refused && accepted31 && refused31 && accepted113 && refused113);
    CHECK(WddmSurfaceFormatBpp(31,BC250_SURFACE_COMPOSED)==4);
    CHECK(!WddmSurfaceFormatBpp(31,BC250_SURFACE_GDI) && !WddmSurfaceFormatBpp(31,BC250_SURFACE_SCANOUT));
    CHECK(WddmSurfaceFormatBpp(113,BC250_SURFACE_COMPOSED)==8);
    CHECK(!WddmSurfaceFormatBpp(113,BC250_SURFACE_GDI) && !WddmSurfaceFormatBpp(113,BC250_SURFACE_SCANOUT));
    /* A8 (D3DDDIFMT_A8, DXGI A8_UNORM) has a COMPOSED row at 1 byte a pixel since the shared table gained it,
     * where the KMD answered 0 before. No composed surface is admitted at that size: DcnLinearSurfaceBytes takes
     * 4 and 8 bytes a pixel only, so a type-0 A8 blob is refused however well its pitch fits. Pinned here so that
     * a later 1-byte layout cannot make A8 a composed surface without a gate saying so. */
    CHECK(WddmSurfaceFormatBpp(28,BC250_SURFACE_COMPOSED)==1);
    CHECK(WddmSurfaceFormatBpp(28,BC250_SURFACE_GDI)==1 && !WddmSurfaceFormatBpp(28,BC250_SURFACE_SCANOUT));
    CHECK(!DcnLinearSurfaceBytes(1920,1080,1920,1,&bytes) && !bytes);
    {
        BC250_WDDM_ALLOCATION_PRIVATE a8={0};
        a8.Magic=BC250_WDDM_ALLOCATION_PRIVATE_MAGIC;a8.Version=1;
        a8.Width=1920;a8.Height=1080;a8.Pitch=1920;a8.Size=1920ull*1080u;a8.Format=28;
        CHECK(!WddmSurfaceAdmitted(&a8,0));
        a8.Pitch=7680;a8.Size=7680ull*1080u;
        CHECK(!WddmSurfaceAdmitted(&a8,0));
    }
    /* The 8-byte geometry itself: a whole number of pixels per row, the row inside the pitch. */
    CHECK(DcnLinearSurfaceBytes(256,64,2048,8,&bytes) && bytes==131072ull);
    CHECK(!DcnLinearSurfaceBytes(256,64,2044,8,&bytes) && !bytes);
    CHECK(!DcnLinearSurfaceBytes(256,64,2052,8,&bytes) && !bytes);
    CHECK(DcnLinearSurfaceBytes(256,64,2056,8,&bytes) && bytes==2056ull*64u);
    CHECK(DcnLinearSurfaceBytes(256,64,1024,4,&bytes) && bytes==65536ull);
    CHECK(!DcnLinearSurfaceBytes(256,64,2048,2,&bytes) && !DcnLinearSurfaceBytes(256,64,2048,16,&bytes));
    CHECK(DcnSurfaceBytes(256,64,1024,&bytes) && bytes==65536ull && !DcnSurfaceBytes(256,64,1020,&bytes));
    /* One 1080p DXGI shared surface: the 4-byte formats in, the unknown ones
     * (A2R10G10B10, A2B10G10R10_XR_BIAS, UNKNOWN) out, and A16B16G16R16F out
     * at a 4-byte pitch. At an 8-byte pitch only A16B16G16R16F is new. */
    s.Width=1920;s.Height=1080;s.Pitch=7680;s.Size=7680ull*1080u;
    for(f=0;f<256;f++) {
        s.Format=f;
        if (f==21 || f==22 || f==31 || f==32 || f==33) CHECK(WddmSurfaceAdmitted(&s,0));
        if (f==0 || f==35 || f==113 || f==119) CHECK(!WddmSurfaceAdmitted(&s,0));
    }
    s.Pitch=15360;s.Size=15360ull*1080u;
    for(f=0;f<256;f++) {
        s.Format=f;
        if (f==21 || f==22 || f==31 || f==32 || f==33 || f==113) CHECK(WddmSurfaceAdmitted(&s,0));
        if (f==0 || f==35 || f==119) CHECK(!WddmSurfaceAdmitted(&s,0));
    }
    s.Format=113;s.Size=15360ull*1080u-1u;CHECK(!WddmSurfaceAdmitted(&s,0));
    s.Size=15360ull*1080u;CHECK(!WddmSurfaceAdmitted(&s,1) && !WddmSurfaceAdmitted(&s,2));
    CHECK(!WddmSurfaceAdmitted(0,0));
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
    SurfaceFormats();
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
