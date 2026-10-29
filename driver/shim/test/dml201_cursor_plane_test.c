/* BD-223 with the hardware pointer: ConnectorLinkPlaneDml (connector_link.c) gives DML the cursor width of the
 * committed plan (256 while the hardware pointer is on, 0 without it). The scaled-plane check (dcn.c DcnPlaneCheck)
 * admits a plane when DML says valid, its DISPCLK and DPPCLK fit in the clocks that run, and its VSTARTUP and VUPDATE
 * offset are those of the timing generator. This test runs the real DML for the scaled shapes of the lab kits, on the
 * lab's timing, with and without the cursor, and requires the same answer for every input of those verdicts. The TTU
 * of the cursor request is the control that DML took the cursor into account. CPU-only: no hardware admission. */
#include "../bc250_dml201.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    printf("FAIL line %u: %s\n",__LINE__,#x); } } while (0)

/* The timing of unit A's monitor: 1920x1200 at 154000 kHz (the connector's "pipe ready" line of the lab). */
static BC250_DML201_INPUT timing(unsigned output, unsigned refKhz)
{
    BC250_DML201_INPUT i={0};
    i.HActive=1920; i.HTotal=2080; i.HFrontPorch=48; i.HSyncWidth=32;
    i.VActive=1200; i.VTotal=1235; i.VFrontPorch=3; i.VSyncWidth=6;
    i.PixelClockKhz=154000; i.Output=output; i.DpLanes=output==BC250_DML201_DP?4:0;
    i.AudioSampleRateKhz=192; i.DchubRefClockKhz=refKhz; i.DentistVcoKhz=3000000;
    return i;
}

/* As DcnPlaneCheck fills BC250_LINK_PLANE: the pitch in pixels, the viewport the pipe reads, the recout it fills and
 * the taps of dcn_scale.c (4 for a scaled direction, 1 for a direction at 1:1). */
static void plane(BC250_DML201_INPUT *i, unsigned sw, unsigned sh, unsigned dw, unsigned dh)
{
    i->PitchPixels=(sw+63)&~63u;
    i->ViewportWidth=sw; i->ViewportHeight=sh; i->RecoutWidth=dw; i->RecoutHeight=dh;
    i->HTaps=sw==dw?1:4; i->VTaps=sh==dh?1:4;
}

int main(void)
{
    static const unsigned shapes[][4]={
        {1280,720,1920,1080},{1280,720,1920,1200},{1600,900,1920,1080},{960,540,1920,1080},
        {1680,1050,1920,1200},{2560,1440,1920,1080},{2560,1600,1920,1200},{3840,2160,1920,1080}};
    static const unsigned outputs[]={BC250_DML201_HDMI,BC250_DML201_DP}, refs[]={50000,48000};
    size_t size=Bc250Dml201WorkspaceSize();
    void *w=malloc(size);
    unsigned o,r,k,valid=0,whole=0;
    CHECK(w!=NULL); if(!w) return 1;
    for(o=0;o<2;++o) for(r=0;r<2;++r) for(k=0;k<sizeof(shapes)/sizeof(shapes[0]);++k) {
        BC250_DML201_INPUT a=timing(outputs[o],refs[r]), b;
        BC250_DML201_PLAN pa, pb;
        int ra, rb;
        plane(&a,shapes[k][0],shapes[k][1],shapes[k][2],shapes[k][3]);
        b=a; b.CursorWidth=256;
        ra=Bc250Dml201Plan(&a,w,size,&pa);
        rb=Bc250Dml201Plan(&b,w,size,&pb);
        CHECK(ra==rb && pa.Valid==pb.Valid);
        if(ra!=BC250_DML201_OK || !pa.Valid || !pb.Valid) {
            printf("cursor-plane %ux%u->%ux%u out=%u ref=%u: result %d/%d valid %u/%u\n",shapes[k][0],shapes[k][1],
                shapes[k][2],shapes[k][3],outputs[o],refs[r],ra,rb,pa.Valid,pb.Valid);
            continue;
        }
        ++valid; if(k==1) ++whole;
        /* The inputs of Bc250PlaneDmlVerdict and Bc250PlaneSyncVerdict, and the requestor's VREADY. */
        CHECK(pa.DispClockKhz==pb.DispClockKhz && pa.DppClockKhz==pb.DppClockKhz);
        CHECK(pa.VStartup==pb.VStartup && pa.VUpdateOffset==pb.VUpdateOffset && pa.VUpdateWidth==pb.VUpdateWidth);
        CHECK(pa.VReady==pb.VReady);
        /* The control: DML planned the cursor request only when it had the cursor. */
        CHECK(pa.Ttu.refcyc_per_req_delivery_cur0==0 && pb.Ttu.refcyc_per_req_delivery_cur0>0);
        printf("cursor-plane %ux%u->%ux%u out=%u ref=%u: disp %u dpp %u vstartup %u vupdate %u/%u cur0 ttu %u/%u\n",
            shapes[k][0],shapes[k][1],shapes[k][2],shapes[k][3],outputs[o],refs[r],pb.DispClockKhz,pb.DppClockKhz,
            pb.VStartup,pb.VUpdateOffset,pb.VUpdateWidth,pb.Ttu.refcyc_per_req_delivery_cur0,
            pb.Ttu.refcyc_per_req_delivery_pre_cur0);
    }
    /* The positive control: the S2 kits' 1280x720 chain on the whole output is a valid plan in every combination. */
    CHECK(whole==4);
    free(w);
    printf("dml201_cursor_plane: %u checks, %u failures, %u valid plans; no hardware admission\n",checks,failures,valid);
    return failures?1:0;
}
