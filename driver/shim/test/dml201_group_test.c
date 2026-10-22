/* Public adapter plus real scaler preparation. CPU-only: no hardware admission. */
#include "../bc250_dml201.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    printf("FAIL line %u: %s\n",__LINE__,#x); } } while (0)

static BC250_DML201_INPUT timing(void)
{
    BC250_DML201_INPUT i={0};
    i.HActive=1920; i.HTotal=2080; i.HFrontPorch=48; i.HSyncWidth=32;
    i.VActive=1200; i.VTotal=1235; i.VFrontPorch=3; i.VSyncWidth=6;
    i.PixelClockKhz=154000; i.PitchPixels=1920;
    i.Output=BC250_DML201_DP; i.DpLanes=4;
    i.DchubRefClockKhz=48000; i.DentistVcoKhz=3000000;
    return i;
}

static BC250_SCALER_REQUEST request(unsigned height, unsigned filter)
{
    BC250_SCALER_REQUEST r={0};
    r.Geometry.Horizontal=(BC250_SCALER_AXIS_INPUT){1920,1920,0,1920,0,1920,0,1920,0,0};
    r.Geometry.Vertical=(BC250_SCALER_AXIS_INPUT){height,1200,0,(int)height,0,1200,0,1200,0,0};
    r.Geometry.ChromaDivisor=1; r.FilterKind=filter;
    return r;
}

static void prepare(const BC250_SCALER_REQUEST *r, BC250_SCALER_PREPARED *s,
                    BC250_DML201_PLANE_INPUT *p)
{
    CHECK(Bc250ScalerPrepare(r,s)==BC250_SCALER_GEOMETRY_OK);
    *p=(BC250_DML201_PLANE_INPUT){s,r->Geometry.Horizontal.SurfaceSize,
        r->Geometry.Vertical.SurfaceSize,(r->Geometry.Horizontal.SurfaceSize+31)&~31u,
        (unsigned)(r->Geometry.Horizontal.SourceEnd-r->Geometry.Horizontal.SourceBegin),
        (unsigned)(r->Geometry.Vertical.SourceEnd-r->Geometry.Vertical.SourceBegin),32};
}

static void shared(const BC250_DML201_GROUP_PLAN *g)
{
    const BC250_DML201_PLAN *a=&g->Plane[0], *b=&g->Plane[1];
    CHECK(g->Valid && g->Count==2 && !g->Assertions);
    CHECK(a->Valid && b->Valid && !a->Assertions && !b->Assertions);
    CHECK(a->VoltageLevel==b->VoltageLevel);
    CHECK(a->DispClockKhz==b->DispClockKhz);
    CHECK(a->DcfClockKhz==b->DcfClockKhz);
    CHECK(a->SocClockKhz==b->SocClockKhz);
    CHECK(a->DcfDeepSleepKhz==b->DcfDeepSleepKhz);
    CHECK(a->FabricClockKhz==b->FabricClockKhz);
    CHECK(a->DramClockKhz==b->DramClockKhz);
    CHECK(!memcmp(a->Watermarks,b->Watermarks,sizeof(a->Watermarks)));
}

static void refused(const BC250_DML201_INPUT *t, const BC250_DML201_PLANE_INPUT *p,
                    unsigned count, void *w, size_t size)
{
    BC250_DML201_GROUP_PLAN g, zero={0};
    memset(&g,0xa5,sizeof(g));
    CHECK(Bc250Dml201PlanGroup(t,p,count,w,size,&g)==BC250_DML201_INVALID);
    CHECK(!memcmp(&g,&zero,sizeof(g)));
}

int main(void)
{
    BC250_DML201_INPUT t=timing(), other;
    BC250_SCALER_PREPARED s[2], corrupt;
    BC250_DML201_PLANE_INPUT p[2], bad[2];
    BC250_SCALER_REQUEST r=request(1200,BC250_SCALER_FILTER_AMD);
    BC250_DML201_GROUP_PLAN g, native, repeat;
    BC250_DML201_PLAN single;
    size_t size=Bc250Dml201WorkspaceSize();
    void *w=malloc(size);
    CHECK(w!=NULL); if(!w) return 1;
    prepare(&r,&s[0],&p[0]); prepare(&r,&s[1],&p[1]);
    CHECK(Bc250Dml201Plan(&t,w,size,&single)==BC250_DML201_OK);
    CHECK(Bc250Dml201PlanGroup(&t,p,1,w,size,&g)==BC250_DML201_OK);
    CHECK(g.Count==1 && g.Valid && !g.Assertions);
    CHECK(!memcmp(&single,&g.Plane[0],sizeof(single)));
    CHECK(Bc250Dml201PlanGroup(&t,p,2,w,size,&native)==BC250_DML201_OK);
    shared(&native);
    CHECK(native.Plane[0].Watermarks[0].UrgentNs>single.Watermarks[0].UrgentNs);
    CHECK(native.Plane[0].Dlg.refcyc_per_line_delivery_l==598);
    CHECK(native.Plane[1].Dlg.refcyc_per_line_delivery_l==598);
    CHECK(Bc250Dml201PlanGroup(&t,p,2,w,size,&repeat)==BC250_DML201_OK);
    CHECK(!memcmp(&native,&repeat,sizeof(native)));
    other=t; other.PitchPixels=0; /* No artificial full-output surface required. */
    CHECK(Bc250Dml201PlanGroup(&other,p,2,w,size,&repeat)==BC250_DML201_OK);
    CHECK(!memcmp(&native,&repeat,sizeof(native)));
    other=t; other.CursorWidth=256;
    CHECK(Bc250Dml201PlanGroup(&other,p,2,w,size,&repeat)==BC250_DML201_OK);
    shared(&repeat);
    CHECK(repeat.Plane[0].Ttu.refcyc_per_req_delivery_cur0>0);
    CHECK(repeat.Plane[1].Ttu.refcyc_per_req_delivery_cur0>0);
    for(unsigned filter=BC250_SCALER_FILTER_AMD;filter<=BC250_SCALER_FILTER_LINEAR;++filter) {
        for(unsigned h=1080;h<=2400;h+=1320) {
            r=request(h,filter); prepare(&r,&s[1],&p[1]);
            CHECK(s[1].LumaV.Taps>=2);
            CHECK(Bc250Dml201PlanGroup(&t,p,2,w,size,&g)==BC250_DML201_OK);
            shared(&g);
            CHECK(g.Plane[0].Dlg.refcyc_per_line_delivery_l==598);
            if(h==2400) CHECK(g.Plane[1].Dlg.refcyc_per_line_delivery_l==308);
            CHECK(g.Plane[1].Destination.recout_height==1200);
            printf("group height=%u filter=%u taps=%u urgent_ns=%u line_delivery=%u/%u\n",
                h,filter,s[1].LumaV.Taps,g.Plane[0].Watermarks[0].UrgentNs,
                g.Plane[0].Dlg.refcyc_per_line_delivery_l,g.Plane[1].Dlg.refcyc_per_line_delivery_l);
        }
    }
    /* Negative destination, nonzero source origin, clip and a smaller surface. */
    r=request(1200,BC250_SCALER_FILTER_AMD);
    r.Geometry.Horizontal=(BC250_SCALER_AXIS_INPUT){1280,1920,64,1216,-100,1100,100,900,0,0};
    r.Geometry.Vertical=(BC250_SCALER_AXIS_INPUT){720,1200,20,700,20,820,50,700,0,0};
    prepare(&r,&s[1],&p[1]);
    CHECK(s[1].Geometry.LumaH.ViewportBegin>64);
    CHECK(s[1].Geometry.LumaV.ViewportBegin>20);
    CHECK(Bc250Dml201PlanGroup(&t,p,2,w,size,&g)==BC250_DML201_OK);
    shared(&g);
    CHECK(g.Plane[1].Destination.recout_width==800);
    CHECK(g.Plane[1].Destination.recout_height==650);
    CHECK(g.Plane[0].Destination.recout_width==1920);
    CHECK(g.Plane[1].Destination.otg_inst==g.Plane[0].Destination.otg_inst);
    other=t; other.PixelClockKhz=1200000;
    memset(&repeat,0xa5,sizeof(repeat));
    CHECK(Bc250Dml201PlanGroup(&other,p,2,w,size,&repeat)==BC250_DML201_UNSUPPORTED);
    { BC250_DML201_GROUP_PLAN zero={0}; CHECK(!memcmp(&repeat,&zero,sizeof(zero))); }

    refused(NULL,p,2,w,size); refused(&t,NULL,2,w,size);
    refused(&t,p,0,w,size); refused(&t,p,3,w,size);
    refused(&t,p,2,NULL,size); refused(&t,p,2,w,size-1);
    refused(&t,p,2,(char*)w+1,size);
    other=t; other.VTotal=1201; refused(&other,p,2,w,size);
#define BAD(field,value) do { memcpy(bad,p,sizeof(p)); bad[1].field=(value); \
    refused(&t,bad,2,w,size); } while(0)
    BAD(Scaler,NULL); BAD(StorageBits,64); BAD(PitchPixels,1279);
    BAD(PitchPixels,1281); BAD(PitchPixels,16416);
    BAD(SurfaceWidth,0); BAD(SurfaceHeight,16384);
    BAD(SourceWidth,1281); BAD(SourceHeight,0);
#define CORRUPT(field,value) do { corrupt=s[1]; corrupt.field=(value); \
    BAD(Scaler,&corrupt); } while(0)
    CORRUPT(Geometry.LumaH.ViewportBegin,1280);
    CORRUPT(Geometry.LumaV.ViewportSize,721);
    CORRUPT(Geometry.LumaH.RecoutBegin,1920);
    CORRUPT(Geometry.LumaV.RecoutSize,1201);
    CORRUPT(Geometry.LumaH.Ratio19,0);
    CORRUPT(Geometry.LumaV.Ratio19,8u<<19);
    CORRUPT(LumaH.Taps,0); CORRUPT(LumaV.Taps,9);
    CORRUPT(LumaV.Taps,1); CORRUPT(Buffer.LumaPartitions,0);
    CORRUPT(Buffer.Config,4); CORRUPT(Geometry.ChromaV.Ratio19,1);
    /* A real subsampled scaler plan must also refuse this RGB-only boundary. */
    r=request(1200,BC250_SCALER_FILTER_AMD); r.Geometry.ChromaDivisor=2;
    prepare(&r,&s[1],&p[1]); refused(&t,p,2,w,size);
    CHECK(Bc250Dml201PlanGroup(&t,p,2,w,size,NULL)==BC250_DML201_INVALID);
    r=request(1200,BC250_SCALER_FILTER_AMD); prepare(&r,&s[1],&p[1]);
    CHECK(Bc250Dml201PlanGroup(&t,p,2,w,size,&repeat)==BC250_DML201_OK);
    CHECK(!memcmp(&native,&repeat,sizeof(native))); /* Refusals leave no stale state. */
    free(w);
    printf("dml201_group: %u checks, %u failures; no hardware admission\n",checks,failures);
    return failures?1:0;
}
