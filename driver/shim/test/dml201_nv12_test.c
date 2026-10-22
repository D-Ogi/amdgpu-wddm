/* Real DML and scaler code. Source mapping controls are CPU-only. */
#include "../bc250_dml201.c"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    printf("FAIL line %u: %s\n",__LINE__,#x); } } while (0)

static BC250_DML201_INPUT timing(void)
{
    BC250_DML201_INPUT t={0};
    t.HActive=1920; t.HTotal=2080; t.HFrontPorch=48; t.HSyncWidth=32;
    t.VActive=1200; t.VTotal=1235; t.VFrontPorch=3; t.VSyncWidth=6;
    t.PixelClockKhz=154000; t.Output=BC250_DML201_DP; t.DpLanes=4;
    t.DchubRefClockKhz=48000; t.DentistVcoKhz=3000000; t.CursorWidth=256;
    return t;
}

static void prepare(unsigned h, unsigned format, unsigned filter,
                    BC250_SCALER_PREPARED *s, BC250_DML201_PLANE_INPUT *p)
{
    BC250_SCALER_REQUEST r={0};
    r.Geometry.Horizontal=(BC250_SCALER_AXIS_INPUT){1920,1920,0,1920,0,1920,0,1920,0,0};
    r.Geometry.Vertical=(BC250_SCALER_AXIS_INPUT){h,1200,0,(int)h,0,1200,0,1200,0,0};
    r.Geometry.ChromaDivisor=format==BC250_DML201_NV12?2:1; r.FilterKind=filter;
    CHECK(Bc250ScalerPrepare(&r,s)==BC250_SCALER_GEOMETRY_OK);
    *p=(BC250_DML201_PLANE_INPUT){s,1920,h,format==BC250_DML201_NV12?2048:1920,
        1920,h,format==BC250_DML201_NV12?8:32,format,format==BC250_DML201_NV12?1024:0};
}

static void refused(const BC250_DML201_INPUT *t, const BC250_DML201_PLANE_INPUT *p,
                    void *w, size_t size)
{
    BC250_DML201_GROUP_PLAN g, zero={0};
    memset(&g,0xa5,sizeof(g));
    CHECK(Bc250Dml201PlanGroup(t,p,2,w,size,&g)==BC250_DML201_INVALID);
    CHECK(!memcmp(&g,&zero,sizeof(g)));
}

int main(void)
{
    BC250_DML201_INPUT t=timing();
    BC250_SCALER_PREPARED s[2], badScaler;
    BC250_DML201_PLANE_INPUT p[2], bad[2], reversed[2];
    BC250_DML201_GROUP_PLAN g, repeat;
    display_e2e_pipe_params_st mapped={0};
    size_t size=Bc250Dml201WorkspaceSize();
    void *w=malloc(size);
    CHECK(w!=NULL); if(!w) return 1;
    prepare(1200,BC250_DML201_RGB32,BC250_SCALER_FILTER_AMD,&s[0],&p[0]);
    prepare(1200,BC250_DML201_NV12,BC250_SCALER_FILTER_AMD,&s[1],&p[1]);
    CHECK(groupPipeInput(&t,&p[1],&mapped));
    CHECK(mapped.pipe.src.source_format==dm_420_8);
    CHECK(mapped.pipe.src.surface_width_y==1920 && mapped.pipe.src.surface_height_y==1200);
    CHECK(mapped.pipe.src.surface_width_c==960 && mapped.pipe.src.surface_height_c==600);
    CHECK(mapped.pipe.src.viewport_width_c==960 && mapped.pipe.src.viewport_height_c==600);
    CHECK(mapped.pipe.src.data_pitch==2048 && mapped.pipe.src.data_pitch_c==1024);
    CHECK(mapped.pipe.scale_ratio_depth.hscl_ratio==1.0);
    CHECK(mapped.pipe.scale_ratio_depth.vscl_ratio==1.0);
    CHECK(mapped.pipe.scale_ratio_depth.hscl_ratio_c==0.5);
    CHECK(mapped.pipe.scale_ratio_depth.vscl_ratio_c==0.5);
    CHECK(mapped.pipe.scale_ratio_depth.scl_enable); /* Chroma scales even at 1:1 luma. */
    CHECK(mapped.pipe.scale_taps.htaps==1 && mapped.pipe.scale_taps.vtaps==1);
    CHECK(mapped.pipe.scale_taps.htaps_c>=2 && mapped.pipe.scale_taps.vtaps_c>=2);
    CHECK(mapped.pipe.src.num_cursors==0 && mapped.pipe.src.cur0_src_width==0);
    CHECK(mapped.pipe.src.sw_mode==dm_sw_linear && !mapped.pipe.src.dcc);
    CHECK(!mapped.pipe.src.gpuvm && !mapped.pipe.src.hostvm);
    for(unsigned filter=BC250_SCALER_FILTER_AMD;filter<=BC250_SCALER_FILTER_LINEAR;++filter) {
        for(unsigned h=600;h<=2400;h*=2) {
            prepare(h,BC250_DML201_NV12,filter,&s[1],&p[1]);
            int status=Bc250Dml201PlanGroup(&t,p,2,w,size,&g);
            printf("NV12 height=%u filter=%u status=%d assertions=%u first=%u\n",
                h,filter,status,g.Assertions,g.FirstAssertionLine);
            CHECK(status==BC250_DML201_OK);
            if(status!=BC250_DML201_OK) continue;
            CHECK(g.Valid && g.Count==2 && !g.Assertions);
            CHECK(g.Plane[0].Valid && g.Plane[1].Valid);
            CHECK(!memcmp(g.Plane[0].Watermarks,g.Plane[1].Watermarks,sizeof(g.Plane[0].Watermarks)));
            CHECK(g.Plane[1].Dlg.refcyc_per_line_delivery_c>0);
            CHECK(g.Plane[1].Ttu.refcyc_per_req_delivery_c>0);
            CHECK(g.Plane[0].Ttu.refcyc_per_req_delivery_cur0>0);
            CHECK(g.Plane[1].Ttu.refcyc_per_req_delivery_cur0==0);
            CHECK(Bc250Dml201PlanGroup(&t,p,2,w,size,&repeat)==BC250_DML201_OK);
            CHECK(!memcmp(&g,&repeat,sizeof(g)));
            reversed[0]=p[1]; reversed[1]=p[0];
            CHECK(Bc250Dml201PlanGroup(&t,reversed,2,w,size,&repeat)==BC250_DML201_OK);
            CHECK(repeat.Plane[0].Dlg.refcyc_per_line_delivery_c==g.Plane[1].Dlg.refcyc_per_line_delivery_c);
            CHECK(repeat.Plane[1].Ttu.refcyc_per_req_delivery_cur0>0);
            printf("NV12 urgent_ns=%u luma_delivery=%u chroma_delivery=%u\n",
                g.Plane[1].Watermarks[0].UrgentNs,g.Plane[1].Dlg.refcyc_per_line_delivery_l,
                g.Plane[1].Dlg.refcyc_per_line_delivery_c);
        }
    }
    prepare(1200,BC250_DML201_NV12,BC250_SCALER_FILTER_AMD,&s[1],&p[1]);
#define BAD(field,value) do { memcpy(bad,p,sizeof(p)); bad[1].field=(value); \
    refused(&t,bad,w,size); } while(0)
    BAD(Format,2); BAD(StorageBits,32); BAD(PitchPixels,1920);
    BAD(ChromaPitchSamples,0); BAD(ChromaPitchSamples,959);
    BAD(ChromaPitchSamples,1025); BAD(ChromaPitchSamples,16512);
    BAD(SurfaceWidth,1919); BAD(SurfaceHeight,1199);
    BAD(SourceWidth,1); BAD(SourceHeight,1);
#define CORRUPT(field,value) do { badScaler=s[1]; badScaler.field=(value); BAD(Scaler,&badScaler); } while(0)
    CORRUPT(Buffer.ChromaPartitions,0);
    CORRUPT(ChromaH.Taps,0); CORRUPT(ChromaV.Taps,9); CORRUPT(ChromaH.Taps,1);
    CORRUPT(Geometry.ChromaH.ViewportBegin,960);
    CORRUPT(Geometry.ChromaV.ViewportSize,601);
    CORRUPT(Geometry.ChromaH.Ratio19,1u<<19);
    CORRUPT(Geometry.ChromaV.RecoutBegin,1);
    CORRUPT(Geometry.ChromaV.RecoutSize,1199);
    CHECK(Bc250Dml201PlanGroup(&t,&p[1],1,w,size,&g)==BC250_DML201_OK);
    CHECK(g.Valid && g.Count==1 && !g.Assertions && g.Plane[1].Valid==0);
    /* Cropped video: odd source extents and output clips, even source origins.
     * Chroma coordinates are component samples; its recout stays output pixels. */
    {
        BC250_SCALER_REQUEST r={0};
        r.Geometry.Horizontal=(BC250_SCALER_AXIS_INPUT){1920,1920,64,1855,-33,1859,101,1701,0,0};
        r.Geometry.Vertical=(BC250_SCALER_AXIS_INPUT){1200,1200,20,1179,9,1179,71,1101,0,0};
        r.Geometry.ChromaDivisor=2; r.FilterKind=BC250_SCALER_FILTER_AMD;
        CHECK(Bc250ScalerPrepare(&r,&s[1])==BC250_SCALER_GEOMETRY_OK);
        p[1].SourceWidth=1791; p[1].SourceHeight=1159;
        p[1].ChromaPitchSamples=1152; /* Independent UV row padding. */
        memset(&mapped,0,sizeof(mapped));
        CHECK(groupPipeInput(&t,&p[1],&mapped));
        CHECK(mapped.pipe.src.viewport_x_c==s[1].Geometry.ChromaH.ViewportBegin);
        CHECK(mapped.pipe.src.viewport_y_c==s[1].Geometry.ChromaV.ViewportBegin);
        CHECK(mapped.pipe.src.viewport_width_c==s[1].Geometry.ChromaH.ViewportSize);
        CHECK(mapped.pipe.src.viewport_height_c==s[1].Geometry.ChromaV.ViewportSize);
        CHECK(mapped.pipe.src.viewport_x_c>32 && mapped.pipe.src.viewport_y_c>10);
        CHECK(mapped.pipe.src.viewport_width_max==1791 && mapped.pipe.src.viewport_height_max==1159);
        CHECK(mapped.pipe.src.data_pitch_c==1152);
        CHECK(mapped.pipe.dest.recout_width==1600 && mapped.pipe.dest.recout_height==1030);
        CHECK(Bc250Dml201PlanGroup(&t,p,2,w,size,&g)==BC250_DML201_OK);
        CHECK(g.Valid && !g.Assertions);
    }
    /* Exact horizontal 8:1 luma has one-LSB adjustment; chroma stays 4:1.
     * A wide vertical 8:1 request can exceed line-buffer capacity instead. */
    prepare(1200,BC250_DML201_NV12,BC250_SCALER_FILTER_AMD,&s[1],&p[1]);
    {
        BC250_SCALER_REQUEST r={0};
        r.Geometry.Horizontal=(BC250_SCALER_AXIS_INPUT){1920,1920,0,1920,0,240,0,240,0,0};
        r.Geometry.Vertical=(BC250_SCALER_AXIS_INPUT){1200,1200,0,1200,0,1200,0,1200,0,0};
        r.Geometry.ChromaDivisor=2;
        CHECK(Bc250ScalerPrepare(&r,&s[1])==BC250_SCALER_GEOMETRY_OK);
    }
    CHECK(s[1].Geometry.LumaH.Ratio19==(8u<<19)-1);
    CHECK(s[1].Geometry.ChromaH.Ratio19==(4u<<19));
    memset(&mapped,0,sizeof(mapped));
    CHECK(groupPipeInput(&t,&p[1],&mapped));
    CHECK(mapped.pipe.scale_ratio_depth.hscl_ratio_c==4.0);
    /* An NV12 plan cannot be relabelled as RGB without rebuilding geometry. */
    p[1].Format=BC250_DML201_RGB32; p[1].StorageBits=32;
    p[1].ChromaPitchSamples=0;
    refused(&t,p,w,size);
    {
        BC250_SCALER_REQUEST r={0};
        BC250_SCALER_BUFFER_PLAN buffer;
        BC250_SCALER_PREPARED zero={0};
        BC250_SCALER_BUFFER_INPUT b={1920,960,1920,(8u<<19)-1,4u<<19,8,4,1,0};
        r.Geometry.Horizontal=(BC250_SCALER_AXIS_INPUT){1920,1920,0,1920,0,1920,0,1920,0,0};
        r.Geometry.Vertical=(BC250_SCALER_AXIS_INPUT){9600,1200,0,9600,0,1200,0,1200,0,0};
        r.Geometry.ChromaDivisor=2;
        CHECK(!Bc250ScalerBufferPlan(&b,&buffer));
        CHECK(Bc250ScalerPrepare(&r,&s[1])==BC250_SCALER_GEOMETRY_UNSUPPORTED);
        CHECK(!memcmp(&s[1],&zero,sizeof(zero)));
    }
    free(w);
    printf("dml201_nv12: %u checks, %u failures; no hardware admission\n",checks,failures);
    return failures?1:0;
}
