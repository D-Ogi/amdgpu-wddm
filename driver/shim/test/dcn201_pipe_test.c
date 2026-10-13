#include "../bc250_dcn201_pipe.h"
#include "dcn_2_0_1_sh_mask.h"
#include "../dml201/reference/pipe/clk_mask.h"
#include "../pipe_registers.inc"
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
    unsigned writes,stalls,drop,add,no_done,power_gated;
} MOCK;
static long readMmio(void* ctx,unsigned long r,unsigned long* v)
{
    MOCK* m=ctx;++m->ops;
    if(m->ops==m->fail_at){m->failed=1;return BC250_DISP_STATUS_DEVICE;}
    if(!r || r>=sizeof(m->reg)){++m->zero;return BC250_DISP_STATUS_ACCESS_DENIED;}
    *v=m->reg[r/4];
    if(r==R(DCHUBP_CNTL) && m->power_gated)*v=0;
    return 0;
}
static long writeMmio(void* ctx,unsigned long r,unsigned long v)
{
    MOCK* m=ctx;++m->ops;
    if(m->ops==m->fail_at){m->failed=1;return BC250_DISP_STATUS_DEVICE;}
    if(!r || r>=sizeof(m->reg)){++m->zero;return BC250_DISP_STATUS_ACCESS_DENIED;}
    if(r==R(REFCLK_CNTL) || r==R(DCHUBBUB_GLOBAL_TIMER_CNTL) || r==R(CLK4_CLK_PLL_REQ) ||
       r==R(CLK4_CLK2_CURRENT_CNT) || r==R(DIG_FIFO_STATUS) || r==R(DPG_STATUS))++m->readonly;
    if(m->failed && r!=R(DISPCLK_FREQ_CHANGE_CNTL))++m->after_failure;
    ++m->writes;m->reg[r/4]=v;
    if(r==R(DENTIST_DISPCLK_CNTL)) {
        if(!m->no_done)m->reg[r/4]|=M(DENTIST_DISPCLK_CNTL,DENTIST_DISPCLK_CHG_DONE)|M(DENTIST_DISPCLK_CNTL,DENTIST_DPPCLK_CHG_DONE);
        else m->reg[r/4]&=~(M(DENTIST_DISPCLK_CNTL,DENTIST_DISPCLK_CHG_DONE)|M(DENTIST_DISPCLK_CNTL,DENTIST_DPPCLK_CHG_DONE));
    }
    if(r==R(OTG_PIXEL_RATE_CNTL)) {
        if(v&M(OTG_PIXEL_RATE_CNTL,OTG_ADD_PIXEL))++m->add;
        if(v&M(OTG_PIXEL_RATE_CNTL,OTG_DROP_PIXEL))++m->drop;
    }
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
    free(m);printf("dcn201 pipe: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
