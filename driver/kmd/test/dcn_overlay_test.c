// Exercise the real layout, scaler, color, MPC and DML adapters together.
#include "../dcn_overlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %u: %s\n",__LINE__,#x); } } while (0)
static BC250_OVERLAY_REQUEST Request(void)
{
    BC250_OVERLAY_REQUEST r = {0};
    r.Count=2; r.Caps=&Bc250DcnCaps201; r.McBase=0x10000000000ull; r.VramPhysical=0x300000000ull; r.VramLength=0x10000000ull;
    r.Timing=(BC250_DML201_INPUT){1920,2080,48,32,1200,1235,3,6,154000,0,BC250_DML201_DP,4,256,0,48000,3000000,0,0,0,0,0,0};
    r.Plane[0]=(BC250_OVERLAY_INPUT){1,BC250_LINEAR_BGRA8,BC250_INPUT_RGB_FULL,BC250_OVERLAY_FILTER_AMD,0,
        1920,1200,7680,0,r.McBase+0x1000000,0x1000000,0,0,
        {0,0,1920,1200},{0,0,1920,1200},{0,0,1920,1200}};
    r.Plane[1]=(BC250_OVERLAY_INPUT){0,BC250_CNVC_NV12,BC250_INPUT_709_LIMITED,BC250_OVERLAY_FILTER_AMD,0,
        1280,720,1280,1536,r.McBase+0x4000000,0x400000,0,0x100000,
        {0,0,1280,720},{0,60,1920,1140},{0,0,1920,1200}};
    return r;
}
static void Refused(const BC250_OVERLAY_REQUEST* r, void* w, size_t bytes,
    BC250_OVERLAY_PLAN* p, unsigned expected)
{
    unsigned status;
    memset(p,0xa5,sizeof(*p));
    status=Bc250OverlayPrepare(r,w,bytes,p);
    CHECK(status==expected);
    for(size_t i=0;i<sizeof(*p);++i) if(((const unsigned char*)p)[i]) { CHECK(0); break; }
}
int main(void)
{
    BC250_OVERLAY_REQUEST r=Request(), bad, reversed;
    BC250_OVERLAY_PLAN* p=malloc(sizeof(*p)), *q=malloc(sizeof(*q));
    size_t bytes=Bc250Dml201WorkspaceSize(); void* w=malloc(bytes);
    CHECK(p && q && w); if(!p || !q || !w) return 1;
    CHECK(Bc250OverlayPrepare(&r,w,bytes,p)==BC250_OVERLAY_OK);
    CHECK(p->Ready && p->Rgb.Ready && p->Nv12.Ready && p->Mpc.Ready && p->Dml.Valid && p->Dml.Count==2);
    CHECK(p->LayerByPipe[0]==1 && p->LayerByPipe[1]==0 && p->Mpc.Output==1 && p->Mpc.EnabledMask==3);
    CHECK(p->Rgb.Physical==r.VramPhysical+0x1000000 && p->Nv12.Physical==r.VramPhysical+0x4000000);
    CHECK(p->Nv12.ChromaPhysical==r.VramPhysical+0x4100000 && p->Nv12.ChromaPitchBytes==1536);
    CHECK(p->Geometry[0].ChromaMode==0 && p->Geometry[1].ChromaMode==1);
    CHECK(p->Geometry[1].Scaler.Geometry.LumaH.RecoutSize==1920);
    CHECK(p->Color[0].Color==BC250_INPUT_RGB_FULL && p->Color[1].Color==BC250_INPUT_709_LIMITED);
    CHECK(!p->Color[0].CscRequired && p->Color[1].CscRequired);
    CHECK(p->Dml.Plane[0].Ttu.refcyc_per_req_delivery_cur0>0 && p->Dml.Plane[1].Ttu.refcyc_per_req_delivery_cur0==0);
    CHECK(p->Dml.Plane[1].Dlg.refcyc_per_line_delivery_c>0);
    // Input array order does not change physical pipe assignment or any plan byte.
    reversed=r; reversed.Plane[0]=r.Plane[1]; reversed.Plane[1]=r.Plane[0];
    CHECK(Bc250OverlayPrepare(&reversed,w,bytes,q)==BC250_OVERLAY_OK);
    CHECK(!memcmp(p,q,sizeof(*p)));
    // Layer 0 remains topmost when RGB and NV12 exchange their layer indices.
    bad=r; bad.Plane[0].LayerIndex=0; bad.Plane[1].LayerIndex=1;
    CHECK(Bc250OverlayPrepare(&bad,w,bytes,q)==BC250_OVERLAY_OK);
    CHECK(q->LayerByPipe[0]==0 && q->LayerByPipe[1]==1 && q->Mpc.Output==0);
    CHECK(!memcmp(p->Geometry,q->Geometry,sizeof(p->Geometry)));
    CHECK(!memcmp(&p->Dml,&q->Dml,sizeof(p->Dml)));
    // Independent UV pitch/offset, including reversed placement and full pitched ranges.
    bad=r; bad.Plane[1].SurfaceOffset=0x100000; bad.Plane[1].ChromaOffset=0;
    CHECK(Bc250OverlayPrepare(&bad,w,bytes,q)==BC250_OVERLAY_OK);
    CHECK(q->Nv12.Physical==r.VramPhysical+0x4100000 && q->Nv12.ChromaPhysical==r.VramPhysical+0x4000000);
    // Largest representable UV pitch: 32768 bytes are 16384 sample pairs.
    // Passing bytes unchanged to DML would incorrectly refuse this legal layout.
    bad=r; bad.Plane[1].ChromaPitchBytes=32768; bad.Plane[1].AllocationBytes=0x2000000;
    CHECK(Bc250OverlayPrepare(&bad,w,bytes,q)==BC250_OVERLAY_OK);
    CHECK(q->Nv12.ChromaPitchBytes==32768 && q->Nv12.ChromaBytes==32768ull*360);
    for(unsigned color=BC250_INPUT_601_FULL;color<BC250_INPUT_COLOR_COUNT;++color) {
        bad=r; bad.Plane[1].Color=color;
        CHECK(Bc250OverlayPrepare(&bad,w,bytes,q)==BC250_OVERLAY_OK);
        CHECK(q->Color[1].Color==color);
    }
    // Each boundary filter name reaches its own scaler kind: the AMD filter takes more than two taps here.
    bad=r; bad.Plane[1].Filter=BC250_OVERLAY_FILTER_AMD;
    CHECK(Bc250OverlayPrepare(&bad,w,bytes,q)==BC250_OVERLAY_OK);
    CHECK(q->Geometry[1].Scaler.LumaH.Taps>2);
    bad=r; bad.Plane[1].Filter=BC250_OVERLAY_FILTER_LINEAR;
    CHECK(Bc250OverlayPrepare(&bad,w,bytes,q)==BC250_OVERLAY_OK);
    CHECK(q->Geometry[1].Scaler.LumaH.Taps==2);
    // Cropping uses component geometry and its original source extents for DML.
    bad=r; bad.Plane[1].Source=(BC250_OVERLAY_RECT){64,20,1279,719};
    bad.Plane[1].Destination=(BC250_OVERLAY_RECT){-31,11,1801,1101};
    bad.Plane[1].Clip=(BC250_OVERLAY_RECT){100,80,1700,1000};
    CHECK(Bc250OverlayPrepare(&bad,w,bytes,q)==BC250_OVERLAY_OK);
    CHECK(q->Geometry[1].Scaler.Geometry.LumaH.RecoutBegin==100 && q->Geometry[1].Scaler.Geometry.LumaH.RecoutSize==1600);
    // The planner reads its input in place (no stack copy on a kernel thread): an output that overlaps the
    // request is refused and cleared, and a separate output gives the same plan as before.
    memcpy(q,&r,sizeof(r));
    CHECK(Bc250OverlayPrepare((BC250_OVERLAY_REQUEST*)q,w,bytes,q)==BC250_OVERLAY_ARGUMENT);
    {
        static const BC250_OVERLAY_PLAN zero;
        CHECK(!memcmp(q,&zero,sizeof(zero)));
    }
    CHECK(Bc250OverlayPrepare(&r,w,bytes,q)==BC250_OVERLAY_OK);
    CHECK(!memcmp(p,q,sizeof(*p)));
#define BAD(field,value,reason) do {bad=r;bad.field=(value);Refused(&bad,w,bytes,q,reason);} while(0)
    Refused(NULL,w,bytes,q,BC250_OVERLAY_ARGUMENT);
    Refused(&r,NULL,bytes,q,BC250_OVERLAY_ARGUMENT);
    Refused(&r,w,0,q,BC250_OVERLAY_DML);
    Refused(&r,(char*)w+1,bytes-1,q,BC250_OVERLAY_DML);
    CHECK(Bc250OverlayPrepare(&r,w,bytes,NULL)==BC250_OVERLAY_ARGUMENT);
    BAD(Count,0,BC250_OVERLAY_TOPOLOGY); BAD(Count,1,BC250_OVERLAY_TOPOLOGY); BAD(Count,3,BC250_OVERLAY_TOPOLOGY);
    BAD(SourceId,1,BC250_OVERLAY_TOPOLOGY); BAD(PostCompositionChanged,1,BC250_OVERLAY_TOPOLOGY);
    BAD(Plane[1].LayerIndex,1,BC250_OVERLAY_TOPOLOGY); BAD(Plane[1].LayerIndex,32,BC250_OVERLAY_TOPOLOGY);
    BAD(Plane[1].Format,BC250_LINEAR_RGBA8,BC250_OVERLAY_TOPOLOGY);
    BAD(Plane[0].Format,BC250_LINEAR_AMD_AYCRCB8,BC250_OVERLAY_FORMAT);
    BAD(Plane[0].Format,BC250_LINEAR_RGB10A2,BC250_OVERLAY_FORMAT);
    BAD(Plane[1].UnsupportedFlags,0x80000000u,BC250_OVERLAY_FLAGS);
    BAD(Plane[0].ChromaPitchBytes,256,BC250_OVERLAY_LAYOUT);
    BAD(Plane[0].ChromaOffset,4096,BC250_OVERLAY_LAYOUT);
    BAD(Plane[1].Width,1279,BC250_OVERLAY_LAYOUT);
    BAD(Plane[1].Height,719,BC250_OVERLAY_LAYOUT);
    BAD(Plane[1].PitchBytes,1279,BC250_OVERLAY_LAYOUT);
    BAD(Plane[1].ChromaPitchBytes,1281,BC250_OVERLAY_LAYOUT);
    BAD(Plane[1].ChromaPitchBytes,1024,BC250_OVERLAY_LAYOUT);
    BAD(Plane[1].ChromaOffset,4096,BC250_OVERLAY_LAYOUT);
    BAD(Plane[1].ChromaOffset,0x100001,BC250_OVERLAY_LAYOUT);
    BAD(Plane[1].AllocationBytes,0x100001,BC250_OVERLAY_LAYOUT);
    BAD(Plane[1].AllocationMcAddress,r.McBase+r.VramLength,BC250_OVERLAY_LAYOUT);
    BAD(Plane[1].AllocationMcAddress,~0ull,BC250_OVERLAY_LAYOUT);
    BAD(Plane[1].Color,BC250_INPUT_RGB_FULL,BC250_OVERLAY_COLOR);
    BAD(Plane[0].Color,BC250_INPUT_709_LIMITED,BC250_OVERLAY_COLOR);
    BAD(Plane[1].Source.Left,1,BC250_OVERLAY_GEOMETRY);
    BAD(Plane[1].Source.Right,INT_MAX,BC250_OVERLAY_GEOMETRY);
    BAD(Plane[1].Destination.Left,INT_MIN,BC250_OVERLAY_GEOMETRY);
    BAD(Plane[1].Destination.Right,319,BC250_OVERLAY_GEOMETRY);
    BAD(Plane[1].Clip.Left,2000,BC250_OVERLAY_GEOMETRY);
    BAD(Plane[1].Filter,~0u,BC250_OVERLAY_GEOMETRY);
    BAD(Timing.PixelClockKhz,0,BC250_OVERLAY_DML);
    // Check3 description: storage metadata only. No MC address takes part; success
    // is Described, never Ready, and the physical plans stay zero.
    {
        BC250_OVERLAY_DESCRIPTION d={0}, dbad; BC250_OVERLAY_PLAN zero; unsigned status;
        memset(&zero,0,sizeof(zero));
        d.Count=2; d.Plane[0]=r.Plane[0]; d.Plane[1]=r.Plane[1];
        d.Plane[0].AllocationMcAddress=0; d.Plane[1].AllocationMcAddress=0;
        memset(q,0xa5,sizeof(*q));
        CHECK(Bc250OverlayDescribe(&d,&Bc250DcnCaps201,&r.Timing,w,bytes,q)==BC250_OVERLAY_OK);
        CHECK(q->Described==1 && q->Ready==0);
        CHECK(!memcmp(&q->Rgb,&zero.Rgb,sizeof(q->Rgb)) && !memcmp(&q->Nv12,&zero.Nv12,sizeof(q->Nv12)));
        CHECK(!memcmp(q->Geometry,p->Geometry,sizeof(p->Geometry)) && !memcmp(&q->Dml,&p->Dml,sizeof(p->Dml)));
        CHECK(q->LayerByPipe[0]==1 && q->LayerByPipe[1]==0 && q->Mpc.Ready);
        // A stale or fabricated MC address does not change the description.
        dbad=d; dbad.Plane[1].AllocationMcAddress=~0ull;
        CHECK(Bc250OverlayDescribe(&dbad,&Bc250DcnCaps201,&r.Timing,w,bytes,q)==BC250_OVERLAY_OK && !q->Ready);
        CHECK(!memcmp(&q->Dml,&p->Dml,sizeof(p->Dml)));
        // The prepared (resident) path still sets both flags.
        CHECK(p->Ready==1 && p->Described==1);
#define DBAD(field,value,reason) do {dbad=d;dbad.field=(value);memset(q,0xa5,sizeof(*q));\
        status=Bc250OverlayDescribe(&dbad,&Bc250DcnCaps201,&r.Timing,w,bytes,q);CHECK(status==(reason));\
        CHECK(!memcmp(q,&zero,sizeof(zero)));} while(0)
        DBAD(Plane[1].ChromaOffset,4096,BC250_OVERLAY_LAYOUT);
        DBAD(Plane[1].AllocationBytes,0x100001,BC250_OVERLAY_LAYOUT);
        DBAD(Plane[1].ChromaPitchBytes,1024,BC250_OVERLAY_LAYOUT);
        DBAD(Plane[0].PitchBytes,7600,BC250_OVERLAY_LAYOUT);
        DBAD(Plane[0].PitchBytes,7680+256*300,BC250_OVERLAY_LAYOUT);
        DBAD(Plane[0].AllocationBytes,7680ull*1199,BC250_OVERLAY_LAYOUT);
        DBAD(Plane[0].SurfaceOffset,16,BC250_OVERLAY_LAYOUT);
        DBAD(Plane[0].ChromaPitchBytes,256,BC250_OVERLAY_LAYOUT);
        DBAD(Plane[0].Width,0,BC250_OVERLAY_LAYOUT);
        DBAD(Count,1,BC250_OVERLAY_TOPOLOGY);
        DBAD(Plane[1].Format,BC250_LINEAR_RGBA8,BC250_OVERLAY_TOPOLOGY);
        DBAD(Plane[1].Color,BC250_INPUT_RGB_FULL,BC250_OVERLAY_COLOR);
        memset(q,0xa5,sizeof(*q));
        CHECK(Bc250OverlayDescribe(NULL,&Bc250DcnCaps201,&r.Timing,w,bytes,q)==BC250_OVERLAY_ARGUMENT && !memcmp(q,&zero,sizeof(zero)));
        memset(q,0xa5,sizeof(*q));
        CHECK(Bc250OverlayDescribe(&d,&Bc250DcnCaps201,NULL,w,bytes,q)==BC250_OVERLAY_ARGUMENT && !memcmp(q,&zero,sizeof(zero)));
        CHECK(Bc250OverlayDescribe(&d,&Bc250DcnCaps201,&r.Timing,w,bytes,NULL)==BC250_OVERLAY_ARGUMENT);
        // Output over the description or over the timing is refused: the planner reads both in place.
        memcpy(q,&d,sizeof(d));
        CHECK(Bc250OverlayDescribe((const BC250_OVERLAY_DESCRIPTION*)q,&Bc250DcnCaps201,&r.Timing,w,bytes,q)==
            BC250_OVERLAY_ARGUMENT && !memcmp(q,&zero,sizeof(zero)));
        memcpy(q,&r.Timing,sizeof(r.Timing));
        CHECK(Bc250OverlayDescribe(&d,&Bc250DcnCaps201,(const BC250_DML201_INPUT*)q,w,bytes,q)==
            BC250_OVERLAY_ARGUMENT && !memcmp(q,&zero,sizeof(zero)));
#undef DBAD
        CHECK(Bc250OverlayDescribe(&d,NULL,&r.Timing,w,bytes,q)==BC250_OVERLAY_ARGUMENT && !memcmp(q,&zero,sizeof(zero)));
    }
    // The display IP row decides limits; no board literal does (Part Y).
    {
        BC250_DCN_CAPS row=Bc250DcnCaps201;
        CHECK(Bc250DcnCapsForPci(0x1002,0x13FE)==&Bc250DcnCaps201 && Bc250DcnCapsForPci(0x1002,0x13DB)==&Bc250DcnCaps201);
        CHECK(Bc250DcnCapsForPci(0x1002,0x15BF)==NULL && Bc250DcnCapsForPci(0x1022,0x13FE)==NULL);
        CHECK(Bc250DcnCaps201.Version==BC250_DCN_2_01 && Bc250DcnCaps201.Pipes==4 && Bc250DcnCaps201.Nv12==BC250_DCN_NV12_WITNESS);
        bad=r; bad.Caps=NULL; Refused(&bad,w,bytes,q,BC250_OVERLAY_ARGUMENT);
        row=Bc250DcnCaps201; row.Pipes=1; bad=r; bad.Caps=&row; Refused(&bad,w,bytes,q,BC250_OVERLAY_TOPOLOGY);
        row=Bc250DcnCaps201; row.MaxSlavePlanes=0; bad=r; bad.Caps=&row; Refused(&bad,w,bytes,q,BC250_OVERLAY_TOPOLOGY);
        row=Bc250DcnCaps201; row.Nv12=BC250_DCN_NV12_NONE; bad=r; bad.Caps=&row; Refused(&bad,w,bytes,q,BC250_OVERLAY_FORMAT);
        row=Bc250DcnCaps201; row.MaxSlaveYuvPlanes=0; bad=r; bad.Caps=&row; Refused(&bad,w,bytes,q,BC250_OVERLAY_FORMAT);
        row=Bc250DcnCaps201; row.MaxSurfaceWidth=1919; bad=r; bad.Caps=&row; Refused(&bad,w,bytes,q,BC250_OVERLAY_LAYOUT);
        // NV12 1280x720 scaled to 1920x1080: 1.5x up. A 1.4x upscale row refuses it.
        row=Bc250DcnCaps201; row.MaxUpscaleNv12=1400; bad=r; bad.Caps=&row; Refused(&bad,w,bytes,q,BC250_OVERLAY_GEOMETRY);
        row=Bc250DcnCaps201; row.MaxUpscaleArgb=1400; bad=r; bad.Caps=&row;
        CHECK(Bc250OverlayPrepare(&bad,w,bytes,q)==BC250_OVERLAY_OK);
        row=Bc250DcnCaps201; row.MinSurfaceDimension=800; bad=r; bad.Caps=&row; Refused(&bad,w,bytes,q,BC250_OVERLAY_GEOMETRY);
        // 1280 source columns shrunk to 1200 exceed a 1000-column downscale source width.
        row=Bc250DcnCaps201; row.MaxDownscaleSourceWidth=1000; bad=r; bad.Caps=&row;
        bad.Plane[1].Destination=(BC250_OVERLAY_RECT){0,60,1200,1140}; Refused(&bad,w,bytes,q,BC250_OVERLAY_GEOMETRY);
        row=Bc250DcnCaps201; row.PitchAlignBytes=512; bad=r; bad.Caps=&row;
        {
            BC250_OVERLAY_DESCRIPTION d2={0}; d2.Count=2; d2.Plane[0]=r.Plane[0]; d2.Plane[1]=r.Plane[1];
            d2.Plane[0].PitchBytes=7936; d2.Plane[0].AllocationMcAddress=d2.Plane[1].AllocationMcAddress=0;
            CHECK(Bc250OverlayDescribe(&d2,&Bc250DcnCaps201,&r.Timing,w,bytes,q)==BC250_OVERLAY_OK);
            CHECK(Bc250OverlayDescribe(&d2,&row,&r.Timing,w,bytes,q)==BC250_OVERLAY_LAYOUT);
        }
    }
    free(w);free(q);free(p);
    printf("overlay group: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
