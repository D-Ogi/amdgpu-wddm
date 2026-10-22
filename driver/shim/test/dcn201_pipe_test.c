#include "../bc250_dcn201_pipe.h"
#include "dcn_2_0_1_sh_mask.h"
#include "../dml201/reference/pipe/clk_mask.h"
#include "../pipe_registers.inc"
#include "../../kmd/regs.generated.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks,failures;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("FAIL %u: %s\n",__LINE__,#x);}} while(0)
#define R(r) PIPE_REG_##r
#define M(r,f) PIPE_MASK_##r##_##f
#define F(r,f,v) Bc250DispField(M(r,f),v)
typedef struct MOCK {
    unsigned long reg[0x70000/4];unsigned ops,fail_at,failed,after_failure,zero,readonly;
    unsigned writes,stalls,drop,add,no_done,power_gated,fail_after;
    unsigned writes_on[0x70000/4];
} MOCK;
static long readMmio(void* ctx,unsigned long r,unsigned long* v)
{
    MOCK* m=ctx;++m->ops;
    if(m->ops==m->fail_at && !m->fail_after){m->failed=1;return BC250_DISP_STATUS_DEVICE;}
    if(!r || r>=sizeof(m->reg)){++m->zero;return BC250_DISP_STATUS_ACCESS_DENIED;}
    *v=m->reg[r/4];
    if(r==R(DCHUBP_CNTL) && m->power_gated)*v=0;
    if(m->ops==m->fail_at){m->failed=1;return BC250_DISP_STATUS_DEVICE;}
    return 0;
}
static long writeMmio(void* ctx,unsigned long r,unsigned long v)
{
    MOCK* m=ctx;++m->ops;
    if(m->ops==m->fail_at && !m->fail_after){m->failed=1;return BC250_DISP_STATUS_DEVICE;}
    if(!r || r>=sizeof(m->reg)){++m->zero;return BC250_DISP_STATUS_ACCESS_DENIED;}
    if(r==R(REFCLK_CNTL) || r==R(DCHUBBUB_GLOBAL_TIMER_CNTL) || r==R(CLK4_CLK_PLL_REQ) ||
       r==R(CLK4_CLK2_CURRENT_CNT) || r==R(DIG_FIFO_STATUS) || r==R(DPG_STATUS))++m->readonly;
    if(m->failed && r!=R(DISPCLK_FREQ_CHANGE_CNTL))++m->after_failure;
    ++m->writes;++m->writes_on[r/4];m->reg[r/4]=v;
    if(r==R(DENTIST_DISPCLK_CNTL)) {
        if(!m->no_done)m->reg[r/4]|=M(DENTIST_DISPCLK_CNTL,DENTIST_DISPCLK_CHG_DONE)|M(DENTIST_DISPCLK_CNTL,DENTIST_DPPCLK_CHG_DONE);
        else m->reg[r/4]&=~(M(DENTIST_DISPCLK_CNTL,DENTIST_DISPCLK_CHG_DONE)|M(DENTIST_DISPCLK_CNTL,DENTIST_DPPCLK_CHG_DONE));
    }
    if(r==R(OTG_PIXEL_RATE_CNTL)) {
        if(v&M(OTG_PIXEL_RATE_CNTL,OTG_ADD_PIXEL))++m->add;
        if(v&M(OTG_PIXEL_RATE_CNTL,OTG_DROP_PIXEL))++m->drop;
    }
    if(m->ops==m->fail_at){m->failed=1;return BC250_DISP_STATUS_DEVICE;}
    return 0;
}
static void stallMmio(void* ctx,unsigned long us) {((MOCK*)ctx)->stalls+=(unsigned)us;}
// Exercise production accessors too: the integration register allowlist is
// checked in this same process, rather than mocked as universally permitted.
static void init(MOCK* m,BC250_DISP_IO* io)
{
    memset(m,0,sizeof(*m));memset(io,0,sizeof(*io));
    io->Context=m;io->Read=readMmio;io->Write=writeMmio;io->Stall=stallMmio;io->BudgetUs=200000;
    m->reg[R(DCHUBBUB_GLOBAL_TIMER_CNTL)/4]=F(DCHUBBUB_GLOBAL_TIMER_CNTL,DCHUBBUB_GLOBAL_TIMER_ENABLE,1);
    m->reg[R(CLK4_CLK_PLL_REQ)/4]=F(CLK4_CLK_PLL_REQ,FbMult_int,30);
    m->reg[R(CLK4_CLK2_CURRENT_CNT)/4]=F(CLK4_CLK2_CURRENT_CNT,CURRENT_COUNT,6000);
    m->reg[R(DENTIST_DISPCLK_CNTL)/4]=F(DENTIST_DISPCLK_CNTL,DENTIST_DISPCLK_WDIVIDER,40)|
        F(DENTIST_DISPCLK_CNTL,DENTIST_DPPCLK_WDIVIDER,40)|M(DENTIST_DISPCLK_CNTL,DENTIST_DISPCLK_CHG_DONE)|M(DENTIST_DISPCLK_CNTL,DENTIST_DPPCLK_CHG_DONE);
    m->reg[R(DCHUBP_CNTL)/4]=M(DCHUBP_CNTL,HUBP_NO_OUTSTANDING_REQ);
    m->reg[BC250_REG_DMU_HUBP1_DCHUBP_CNTL/4]=M(DCHUBP_CNTL,HUBP_NO_OUTSTANDING_REQ);
    m->reg[R(DIG_FIFO_STATUS)/4]=F(DIG_FIFO_STATUS,DIG_FIFO_CAL_AVERAGE_LEVEL,32);
}
static BC250_DML201_PLAN plan(void)
{
    BC250_DML201_INPUT i;BC250_DML201_PLAN p;size_t n=Bc250Dml201WorkspaceSize();void* ws=malloc(n);
    memset(&i,0,sizeof(i));memset(&p,0,sizeof(p));CHECK(ws!=NULL);if(!ws)return p;
    i.HActive=1920;i.HTotal=2200;i.HFrontPorch=88;i.HSyncWidth=44;
    i.VActive=1080;i.VTotal=1125;i.VFrontPorch=4;i.VSyncWidth=5;
    i.PixelClockKhz=148500;i.PitchPixels=1920;i.DchubRefClockKhz=48000;i.DentistVcoKhz=3000000;
    CHECK(Bc250Dml201Plan(&i,ws,n,&p)==0);free(ws);return p;
}
static void ready(MOCK* m,BC250_DISP_IO* io,BC250_DCN201_CLOCKS* c)
{
    init(m,io);CHECK(Bc250Dcn201ReadClocks(io,48000,c)==0);CHECK(Bc250Dcn201PlaneBlank(io,1)==0);
    m->ops=0;m->writes=0;io->Reads=0;io->Writes=0;
}

static BC250_DML201_GROUP_PLAN groupPlan(unsigned nv12)
{
    BC250_DML201_INPUT i={0};BC250_SCALER_REQUEST r={0};
    BC250_SCALER_PREPARED s[2];BC250_DML201_PLANE_INPUT planes[2];
    BC250_DML201_GROUP_PLAN g={0};size_t size=Bc250Dml201WorkspaceSize();void* ws=malloc(size);
    CHECK(ws!=NULL);if(!ws)return g;
    i.HActive=1920;i.HTotal=2200;i.HFrontPorch=88;i.HSyncWidth=44;
    i.VActive=1080;i.VTotal=1125;i.VFrontPorch=4;i.VSyncWidth=5;
    i.PixelClockKhz=148500;i.DchubRefClockKhz=48000;i.DentistVcoKhz=3000000;
    r.Geometry.Horizontal=(BC250_SCALER_AXIS_INPUT){1920,1920,0,1920,0,1920,0,1920,0,0};
    r.Geometry.ChromaDivisor=1;
    for(unsigned n=0;n<2;++n) {
        unsigned h=1080*(n+1);
        r.Geometry.ChromaDivisor=nv12 && n==1?2:1;
        r.Geometry.Vertical=(BC250_SCALER_AXIS_INPUT){h,1080,0,(int)h,0,1080,0,1080,0,0};
        CHECK(Bc250ScalerPrepare(&r,&s[n])==BC250_SCALER_GEOMETRY_OK);
        planes[n]=(BC250_DML201_PLANE_INPUT){&s[n],1920,h,1920,1920,h,32,BC250_DML201_RGB32,0};
        if(nv12 && n==1) {
            planes[n].Format=BC250_DML201_NV12;planes[n].StorageBits=8;
            planes[n].PitchPixels=2048;planes[n].ChromaPitchSamples=1024;
        }
    }
    CHECK(Bc250Dml201PlanGroup(&i,planes,2,ws,size,&g)==BC250_DML201_OK);
    free(ws);return g;
}

static void groupReady(MOCK* m,BC250_DISP_IO* io,BC250_DCN201_CLOCKS* c)
{
    ready(m,io,c);CHECK(Bc250Dcn201PlaneBlankAt(io,1,1)==0);
    m->ops=m->writes=0;io->Reads=io->Writes=0;
    memset(m->writes_on,0,sizeof(m->writes_on));
}

static void groupTest(MOCK* m,BC250_DISP_IO* io)
{
    BC250_DCN201_CLOCKS c;BC250_DML201_GROUP_PLAN g=groupPlan(0),bad;
    unsigned pipes[2]={0,1},reverse[2]={1,0},duplicate[2]={1,1},outside[2]={0,2};
    unsigned total,first,second;
    groupReady(m,io,&c);
    CHECK(Bc250Dcn201GroupProgramBlanked(io,&g,pipes,&c)==0);total=m->ops;
    CHECK(!m->zero && !m->readonly && !m->drop && !m->add && !m->stalls);
    CHECK(!m->writes_on[R(DENTIST_DISPCLK_CNTL)/4]);
    CHECK(m->writes_on[R(DCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A)/4]==1);
    first=(unsigned)Bc250DispGet(m->reg[R(PER_LINE_DELIVERY)/4],M(PER_LINE_DELIVERY,REFCYC_PER_LINE_DELIVERY_L));
    second=(unsigned)Bc250DispGet(m->reg[BC250_REG_DMU_HUBPREQ1_PER_LINE_DELIVERY/4],M(PER_LINE_DELIVERY,REFCYC_PER_LINE_DELIVERY_L));
    CHECK(first==g.Plane[0].Dlg.refcyc_per_line_delivery_l);
    CHECK(second==g.Plane[1].Dlg.refcyc_per_line_delivery_l);CHECK(first!=second);
    CHECK(m->writes_on[BC250_REG_DMU_HUBP1_DCHUBP_REQ_SIZE_CONFIG/4]>0);
    CHECK(m->reg[R(DCHUBP_CNTL)/4]&M(DCHUBP_CNTL,HUBP_BLANK_EN));
    CHECK(m->reg[BC250_REG_DMU_HUBP1_DCHUBP_CNTL/4]&M(DCHUBP_CNTL,HUBP_BLANK_EN));
    printf("group program: %u MMIO operations, %u writes, line delivery %u/%u\n",total,m->writes,first,second);
    /* Mixed RGB/NV12 requestors: chroma delivery must reach the selected HUBP.
     * This is still blanked setup, without addresses, CSC, latch or scanout. */
    {
        BC250_DML201_GROUP_PLAN video=groupPlan(1);
        groupReady(m,io,&c);
        CHECK(Bc250Dcn201GroupProgramBlanked(io,&video,pipes,&c)==0);
        CHECK(video.Plane[1].Dlg.refcyc_per_line_delivery_c>0);
        CHECK(Bc250DispGet(m->reg[BC250_REG_DMU_HUBPREQ1_PER_LINE_DELIVERY/4],
            M(PER_LINE_DELIVERY,REFCYC_PER_LINE_DELIVERY_C))==video.Plane[1].Dlg.refcyc_per_line_delivery_c);
        CHECK(m->reg[BC250_REG_DMU_HUBP1_DCHUBP_CNTL/4]&M(DCHUBP_CNTL,HUBP_BLANK_EN));
        groupReady(m,io,&c);
        CHECK(Bc250Dcn201GroupProgramBlanked(io,&video,reverse,&c)==0);
        CHECK(Bc250DispGet(m->reg[R(PER_LINE_DELIVERY)/4],
            M(PER_LINE_DELIVERY,REFCYC_PER_LINE_DELIVERY_C))==video.Plane[1].Dlg.refcyc_per_line_delivery_c);
    }
    for(unsigned after=0;after<2;++after) for(unsigned n=1;n<=total;++n) {
        groupReady(m,io,&c);m->fail_at=n;m->fail_after=after;
        CHECK(Bc250Dcn201GroupProgramBlanked(io,&g,pipes,&c)<0);
        CHECK(m->failed && !m->after_failure && !m->zero && !m->readonly);
        CHECK(m->reg[R(DCHUBP_CNTL)/4]&M(DCHUBP_CNTL,HUBP_BLANK_EN));
        CHECK(m->reg[BC250_REG_DMU_HUBP1_DCHUBP_CNTL/4]&M(DCHUBP_CNTL,HUBP_BLANK_EN));
    }
    groupReady(m,io,&c);CHECK(Bc250Dcn201GroupProgramBlanked(io,&g,reverse,&c)==0);
    CHECK(Bc250DispGet(m->reg[R(PER_LINE_DELIVERY)/4],M(PER_LINE_DELIVERY,REFCYC_PER_LINE_DELIVERY_L))==second);
    CHECK(Bc250DispGet(m->reg[BC250_REG_DMU_HUBPREQ1_PER_LINE_DELIVERY/4],M(PER_LINE_DELIVERY,REFCYC_PER_LINE_DELIVERY_L))==first);
    groupReady(m,io,&c);bad=g;bad.Count=1;
    CHECK(Bc250Dcn201GroupProgramBlanked(io,&bad,reverse,&c)==0);
    CHECK(!m->writes_on[R(PER_LINE_DELIVERY)/4]);
    CHECK(m->writes_on[BC250_REG_DMU_HUBPREQ1_PER_LINE_DELIVERY/4]==1);
#define BAD_GROUP(field,value) do { groupReady(m,io,&c);bad=g;bad.field=(value); \
    CHECK(Bc250Dcn201GroupProgramBlanked(io,&bad,pipes,&c)==BC250_DISP_STATUS_INVALID);CHECK(m->ops==0); } while(0)
    BAD_GROUP(Count,0);BAD_GROUP(Count,3);BAD_GROUP(Valid,0);
    BAD_GROUP(Plane[1].Rq.rq_regs_l.chunk_size,0xffffffffu);
    BAD_GROUP(Plane[1].Ttu.refcyc_per_req_delivery_pre_cur1,1);
    BAD_GROUP(Plane[1].Watermarks[0].UrgentNs,g.Plane[0].Watermarks[0].UrgentNs+1);
    BAD_GROUP(Plane[1].Destination.htotal,2201);BAD_GROUP(Plane[1].Destination.otg_inst,1);
    BAD_GROUP(Plane[1].Destination.interlaced,1);BAD_GROUP(Plane[1].DcfClockKhz,1);
    groupReady(m,io,&c);
    CHECK(Bc250Dcn201GroupProgramBlanked(io,&g,duplicate,&c)==BC250_DISP_STATUS_INVALID);
    CHECK(Bc250Dcn201GroupProgramBlanked(io,&g,outside,&c)==BC250_DISP_STATUS_INVALID);
    CHECK(Bc250Dcn201GroupProgramBlanked(io,&g,NULL,&c)==BC250_DISP_STATUS_INVALID);CHECK(m->ops==0);
    groupReady(m,io,&c);bad=g;bad.Plane[1].DppClockKhz=c.DppClockKhz+1;
    CHECK(Bc250Dcn201GroupProgramBlanked(io,&bad,pipes,&c)==BC250_DISP_STATUS_NOT_SUPPORTED);CHECK(m->writes==0);
    groupReady(m,io,&c);c.DentistVcoKhz++;
    CHECK(Bc250Dcn201GroupProgramBlanked(io,&g,pipes,&c)==BC250_DISP_STATUS_MISMATCH);CHECK(m->writes==0);
    for(unsigned n=0;n<2;++n) for(unsigned missing=0;missing<3;++missing) {
        const unsigned long bits[]={M(DCHUBP_CNTL,HUBP_BLANK_EN),M(DCHUBP_CNTL,HUBP_TTU_DISABLE),
            M(DCHUBP_CNTL,HUBP_NO_OUTSTANDING_REQ)};
        groupReady(m,io,&c);
        m->reg[(n?BC250_REG_DMU_HUBP1_DCHUBP_CNTL:R(DCHUBP_CNTL))/4]&=~bits[missing];
        CHECK(Bc250Dcn201GroupProgramBlanked(io,&g,pipes,&c)==BC250_DISP_STATUS_BUSY);CHECK(m->writes==0);
        bad=g;bad.Count=1;
        CHECK(Bc250Dcn201GroupProgramBlanked(io,&bad,pipes,&c)==BC250_DISP_STATUS_BUSY);CHECK(m->writes==0);
    }
    init(m,io);CHECK(Bc250Dcn201PlaneBlankAt(io,2,1)==BC250_DISP_STATUS_INVALID);CHECK(m->ops==0);
    m->reg[BC250_REG_DMU_HUBP1_DCHUBP_CNTL/4]=0;io->BudgetUs=7;
    CHECK(Bc250Dcn201PlaneBlankAt(io,1,1)==BC250_DISP_STATUS_BUDGET);CHECK(io->StalledUs==7);
}
int main(void)
{
    MOCK* m=calloc(1,sizeof(*m));BC250_DISP_IO io;BC250_DCN201_CLOCKS c,after;
    BC250_DML201_PLAN p=plan(),bad;unsigned total,i;long status;
    CHECK(m!=NULL);if(!m)return 1;
    init(m,&io);CHECK(Bc250Dcn201ReadClocks(&io,48000,&c)==0);
    CHECK(c.DentistVcoKhz==3000000 && c.DchubRefClockKhz==48000 && c.DpRefClockKhz==600000);
    CHECK(c.DispClockKhz==300000 && c.DppClockKhz==300000);CHECK(m->writes==0);
    CHECK(Bc250Dcn201PipeApply(&io,&p,&c)==BC250_DISP_STATUS_BUSY);CHECK(m->writes==0);
    CHECK(Bc250Dcn201PlaneBlank(&io,1)==0);CHECK(Bc250Dcn201PipeApply(&io,&p,&c)==0);
    CHECK(Bc250Dcn201ReadClocks(&io,48000,&after)==0);
    CHECK(after.DispClockKhz>=p.DispClockKhz && after.DppClockKhz>=p.DppClockKhz);
    CHECK(Bc250DispGet(m->reg[R(DST_DIMENSIONS)/4],M(DST_DIMENSIONS,REFCYC_PER_HTOTAL))==182044);
    CHECK(Bc250DispGet(m->reg[R(BLANK_OFFSET_0)/4],M(BLANK_OFFSET_0,DLG_V_BLANK_END))==41);
    CHECK(Bc250DispGet(m->reg[R(DCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A)/4],M(DCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A,DCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A))==p.Watermarks[0].UrgentNs*48/1000);
    CHECK(m->reg[R(DCHUBBUB_ARB_PTE_META_URGENCY_WATERMARK_A)/4]==p.Watermarks[0].MemoryTripNs*48/1000);
    CHECK(m->reg[R(DCHUBBUB_ARB_DRAM_STATE_CNTL)/4]&M(DCHUBBUB_ARB_DRAM_STATE_CNTL,DCHUBBUB_ARB_ALLOW_SELF_REFRESH_FORCE_ENABLE));
    CHECK(m->zero==0 && m->readonly==0);CHECK(Bc250Dcn201PlaneBlank(&io,0)==0);
    ready(m,&io,&c);CHECK(Bc250Dcn201PipeApply(&io,&p,&c)==0);total=m->ops;
    printf("ordinary apply: %u MMIO operations, %u writes\n",total,m->writes);
    for(i=1;i<=total;++i) {
        ready(m,&io,&c);m->fail_at=i;status=Bc250Dcn201PipeApply(&io,&p,&c);
        CHECK(status<0);CHECK(m->failed==1);CHECK(m->after_failure==0);CHECK(!m->zero && !m->readonly);
    }
    ready(m,&io,&c);bad=p;bad.Rq.rq_regs_l.chunk_size=0xffffffffu;
    CHECK(Bc250Dcn201PipeApply(&io,&bad,&c)==BC250_DISP_STATUS_INVALID);CHECK(m->ops==0);
    bad=p;bad.Ttu.refcyc_per_req_delivery_pre_cur1=1;
    CHECK(Bc250Dcn201PipeApply(&io,&bad,&c)==BC250_DISP_STATUS_INVALID);CHECK(m->ops==0);
    bad=p;bad.Watermarks[0].UrgentNs=0xffffffffu;
    CHECK(Bc250Dcn201PipeApply(&io,&bad,&c)==BC250_DISP_STATUS_INVALID);CHECK(m->ops==0);
    bad=p;bad.VStartup++;CHECK(Bc250Dcn201PipeApply(&io,&bad,&c)==BC250_DISP_STATUS_INVALID);CHECK(m->ops==0);
    ready(m,&io,&c);m->reg[R(CLK4_CLK_PLL_REQ)/4]=0;
    CHECK(Bc250Dcn201PipeApply(&io,&p,&c)<0);CHECK(m->writes==0);
    init(m,&io);m->reg[R(REFCLK_CNTL)/4]=M(REFCLK_CNTL,REFCLK_CLOCK_EN);
    CHECK(Bc250Dcn201ReadClocks(&io,48000,&c)==BC250_DISP_STATUS_NOT_SUPPORTED);CHECK(!c.DentistVcoKhz);
    init(m,&io);m->reg[R(DCHUBBUB_GLOBAL_TIMER_CNTL)/4]=0;
    CHECK(Bc250Dcn201ReadClocks(&io,48000,&c)==BC250_DISP_STATUS_NOT_SUPPORTED);
    init(m,&io);m->reg[R(CLK4_CLK_PLL_REQ)/4]=0xffffffffu;
    CHECK(Bc250Dcn201ReadClocks(&io,48000,&c)==BC250_DISP_STATUS_DEVICE);
    init(m,&io);m->power_gated=1;CHECK(Bc250Dcn201PlaneBlank(&io,1)==BC250_DISP_STATUS_MISMATCH);
    init(m,&io);m->reg[R(DCHUBP_CNTL)/4]=0;CHECK(Bc250Dcn201PlaneBlank(&io,1)==BC250_DISP_STATUS_TIMEOUT);CHECK(io.StalledUs==200);
    init(m,&io);m->reg[R(DCHUBP_CNTL)/4]=0;io.BudgetUs=7;
    CHECK(Bc250Dcn201PlaneBlank(&io,1)==BC250_DISP_STATUS_BUDGET);CHECK(io.StalledUs==7);
    ready(m,&io,&c);m->no_done=1;io.BudgetUs=100;
    CHECK(Bc250Dcn201PipeApply(&io,&p,&c)==BC250_DISP_STATUS_BUDGET);CHECK(io.StalledUs==100);
    // Crossing the special DID127 boundary must use AMD's FIFO drop sequence.
    ready(m,&io,&c);m->reg[R(DENTIST_DISPCLK_CNTL)/4]&=~M(DENTIST_DISPCLK_CNTL,DENTIST_DISPCLK_WDIVIDER);
    m->reg[R(DENTIST_DISPCLK_CNTL)/4]|=F(DENTIST_DISPCLK_CNTL,DENTIST_DISPCLK_WDIVIDER,127);
    CHECK(Bc250Dcn201ReadClocks(&io,48000,&c)==0);m->ops=0;
    CHECK(Bc250Dcn201PipeApply(&io,&p,&c)==0);CHECK(m->drop==4);total=m->ops;
    for(i=1;i<=total;++i) {
        ready(m,&io,&c);m->reg[R(DENTIST_DISPCLK_CNTL)/4]&=~M(DENTIST_DISPCLK_CNTL,DENTIST_DISPCLK_WDIVIDER);
        m->reg[R(DENTIST_DISPCLK_CNTL)/4]|=F(DENTIST_DISPCLK_CNTL,DENTIST_DISPCLK_WDIVIDER,127);
        CHECK(Bc250Dcn201ReadClocks(&io,48000,&c)==0);m->ops=0;m->fail_at=i;
        CHECK(Bc250Dcn201PipeApply(&io,&p,&c)<0);CHECK(!m->after_failure && !m->zero && !m->readonly);
        // Cleanup itself may fail. All other injected failures restore override.
        if(m->reg[R(DISPCLK_FREQ_CHANGE_CNTL)/4]&M(DISPCLK_FREQ_CHANGE_CNTL,DCCG_FIFO_ERRDET_OVR_EN))
            CHECK(m->ops==m->fail_at);
    }
    // A synthetic request isolates the reverse dentist transition, without
    // claiming this clock alone is a valid complete display mode.
    ready(m,&io,&c);bad=p;bad.DispClockKhz=20000;bad.DppClockKhz=20000;
    CHECK(Bc250Dcn201PipeApply(&io,&bad,&c)==0);CHECK(m->add==4);CHECK(m->drop==0);
    CHECK(Bc250DispGet(m->reg[R(DENTIST_DISPCLK_CNTL)/4],M(DENTIST_DISPCLK_CNTL,DENTIST_DISPCLK_WDIVIDER))==127);
    CHECK(m->reg[R(DISPCLK_FREQ_CHANGE_CNTL)/4]==0);
    ready(m,&io,&c);CHECK(Bc250Dcn201OutputProgram(&io,&p)==0);total=m->ops;
    CHECK(Bc250DispGet(m->reg[R(MUX)/4],M(MUX,MPC_OUT_FLOW_CONTROL_COUNT1))==280);
    CHECK(Bc250DispGet(m->reg[R(MUX)/4],M(MUX,MPC_OUT_FLOW_CONTROL_COUNT0))==128);
    CHECK(!(m->reg[R(MUX)/4]&M(MUX,MPC_OUT_RATE_CONTROL_DISABLE)));
    CHECK(m->reg[R(OPP_PIPE_CONTROL)/4]&M(OPP_PIPE_CONTROL,OPP_PIPE_CLOCK_EN));
    CHECK(Bc250DispGet(m->reg[R(FMT_422_CONTROL)/4],M(FMT_422_CONTROL,FMT_LEFT_EDGE_EXTRA_PIXEL_COUNT))==0);
    CHECK(Bc250DispGet(m->reg[R(DPG_DIMENSIONS)/4],M(DPG_DIMENSIONS,DPG_ACTIVE_WIDTH))==1920);
    CHECK(Bc250DispGet(m->reg[R(DPG_DIMENSIONS)/4],M(DPG_DIMENSIONS,DPG_ACTIVE_HEIGHT))==1080);
    CHECK(m->reg[R(DPG_CONTROL)/4]&M(DPG_CONTROL,DPG_EN));
    CHECK(Bc250Dcn201OutputBlank(&io,1920,1080,0)==0);CHECK(m->reg[R(DPG_CONTROL)/4]==0);
    for(i=1;i<=total;++i) {
        ready(m,&io,&c);m->fail_at=i;CHECK(Bc250Dcn201OutputProgram(&io,&p)<0);
        CHECK(m->failed && !m->after_failure && !m->zero && !m->readonly);
    }
    ready(m,&io,&c);bad=p;bad.Destination.htotal=10000;
    CHECK(Bc250Dcn201OutputProgram(&io,&bad)==BC250_DISP_STATUS_INVALID);CHECK(m->ops==0);
    init(m,&io);CHECK(Bc250Dcn201OutputProgram(&io,&p)==BC250_DISP_STATUS_BUSY);CHECK(m->writes==0);
    init(m,&io);m->reg[R(DPG_STATUS)/4]=M(DPG_STATUS,DPG_DOUBLE_BUFFER_PENDING);io.BudgetUs=20;
    CHECK(Bc250Dcn201OutputBlank(&io,1920,1080,1)==BC250_DISP_STATUS_BUDGET);CHECK(io.StalledUs==20);
    init(m,&io);CHECK(Bc250Dcn201OutputBlank(&io,1920,1080,1)==0);total=m->ops;
    for(i=1;i<=total;++i) {
        init(m,&io);m->fail_at=i;CHECK(Bc250Dcn201OutputBlank(&io,1920,1080,1)<0);
        CHECK(m->failed && !m->after_failure && !m->zero && !m->readonly);
    }
    for(i=0;i<4;++i) {
        if(i==1)continue;
        init(m,&io);CHECK(Bc250Dcn201Route(&io,i,1)==0);
        CHECK(Bc250DispGet(m->reg[R(DIG_BE_CNTL)/4],M(DIG_BE_CNTL,DIG_MODE))==i);
        CHECK(Bc250DispGet(m->reg[R(DIG_BE_CNTL)/4],M(DIG_BE_CNTL,DIG_FE_SOURCE_SELECT))==1);
        CHECK(Bc250DispGet(m->reg[R(DIG_FE_CNTL)/4],M(DIG_FE_CNTL,DIG_SOURCE_SELECT))==0);
        CHECK(Bc250Dcn201Route(&io,i,0)==0);
        CHECK(Bc250DispGet(m->reg[R(DIG_BE_CNTL)/4],M(DIG_BE_CNTL,DIG_FE_SOURCE_SELECT))==0);
    }
    init(m,&io);CHECK(Bc250Dcn201Route(&io,3,1)==0);total=m->ops;
    for(i=1;i<=total;++i) {
        init(m,&io);m->fail_at=i;CHECK(Bc250Dcn201Route(&io,3,1)<0);
        CHECK(m->failed && !m->after_failure && !m->zero && !m->readonly);
    }
    init(m,&io);m->reg[R(DIG_BE_CNTL)/4]=F(DIG_BE_CNTL,DIG_FE_SOURCE_SELECT,2);
    CHECK(Bc250Dcn201Route(&io,3,1)==BC250_DISP_STATUS_BUSY);CHECK(m->writes==0);
    init(m,&io);CHECK(Bc250Dcn201Route(&io,1,1)==BC250_DISP_STATUS_INVALID);CHECK(m->ops==0);
    // Validate before conversion to the AMD bool argument. In particular,
    // 256 must not silently become either false (byte) or true (_Bool).
    {
        static const int invalid[]={-2147483647-1,-65536,-256,-1,2,127,128,255,256,257,65536,2147483647};
        unsigned n;
        for(n=0;n<sizeof(invalid)/sizeof(invalid[0]);++n) {
            init(m,&io);CHECK(Bc250Dcn201Route(&io,3,invalid[n])==BC250_DISP_STATUS_INVALID);
            CHECK(m->ops==0);
            init(m,&io);CHECK(Bc250Dcn201PlaneBlankAt(&io,0,invalid[n])==BC250_DISP_STATUS_INVALID);
            CHECK(m->ops==0);
            init(m,&io);CHECK(Bc250Dcn201PlaneBlankAt(&io,1,invalid[n])==BC250_DISP_STATUS_INVALID);
            CHECK(m->ops==0);
        }
    }
    groupTest(m,&io);
    free(m);printf("dcn201 pipe: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
