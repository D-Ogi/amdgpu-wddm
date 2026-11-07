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
    r.Count=2; r.McBase=0x10000000000ull; r.VramPhysical=0x300000000ull; r.VramLength=0x10000000ull;
    r.Timing=(BC250_DML201_INPUT){1920,2080,48,32,1200,1235,3,6,154000,0,BC250_DML201_DP,4,256,0,48000,3000000,0,0,0,0,0,0};
    r.Plane[0]=(BC250_OVERLAY_INPUT){1,BC250_LINEAR_BGRA8,BC250_INPUT_RGB_FULL,BC250_SCALER_FILTER_AMD,0,
        1920,1200,7680,0,r.McBase+0x1000000,0x1000000,0,0,
        {0,0,1920,1200},{0,0,1920,1200},{0,0,1920,1200}};
    r.Plane[1]=(BC250_OVERLAY_INPUT){0,BC250_CNVC_NV12,BC250_INPUT_709_LIMITED,BC250_SCALER_FILTER_AMD,0,
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
    bad=r; bad.Plane[1].Filter=BC250_SCALER_FILTER_LINEAR;
    CHECK(Bc250OverlayPrepare(&bad,w,bytes,q)==BC250_OVERLAY_OK);
    CHECK(q->Geometry[1].Scaler.LumaH.Taps==2);
    // Cropping uses component geometry and its original source extents for DML.
    bad=r; bad.Plane[1].Source=(BC250_OVERLAY_RECT){64,20,1279,719};
    bad.Plane[1].Destination=(BC250_OVERLAY_RECT){-31,11,1801,1101};
    bad.Plane[1].Clip=(BC250_OVERLAY_RECT){100,80,1700,1000};
    CHECK(Bc250OverlayPrepare(&bad,w,bytes,q)==BC250_OVERLAY_OK);
    CHECK(q->Geometry[1].Scaler.Geometry.LumaH.RecoutBegin==100 && q->Geometry[1].Scaler.Geometry.LumaH.RecoutSize==1600);
    // Input and output alias, but the plan contains no retained temporary pointers.
    memcpy(q,&r,sizeof(r));
    CHECK(Bc250OverlayPrepare((BC250_OVERLAY_REQUEST*)q,w,bytes,q)==BC250_OVERLAY_OK);
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
    free(w);free(q);free(p);
    printf("overlay group: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
