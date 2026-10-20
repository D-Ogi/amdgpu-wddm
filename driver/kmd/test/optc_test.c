// Timing vectors and access-failure injection against unchanged AMD DC functions.
#include <stdio.h>
#include <string.h>
#include "../regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "../../shim/bc250_optc.h"

static unsigned checks,failures;
#define CHECK(x) do { ++checks;if(!(x)) {++failures;printf("FAIL %u: %s\n",(unsigned)__LINE__,#x);} } while(0)
#define REG(n) BC250_REG_DMU_##n
// Private fake MMIO, no addresses or pointers come from the host system.
typedef struct MODEL { unsigned long reg[65536];unsigned access,failAt,after,writes,mutateAt; } MODEL;
static MODEL model;
static long access(MODEL* m)
{
    ++m->access;if(m->failAt && m->access>m->failAt)++m->after;
    return m->access==m->failAt?BC250_DISP_STATUS_DEVICE:0;
}
static long Read(void* p,unsigned long r,unsigned long* v)
{
    MODEL* m=p;long s=access(m);CHECK(r/4<65536);
    if(s>=0)*v=m->reg[r/4]^(m->access==m->mutateAt?1ul:0ul);return s;
}
static long Write(void* p,unsigned long r,unsigned long v)
{
    MODEL* m=p;long s=access(m);CHECK(r/4<65536);
    if(s>=0) {m->reg[r/4]=v;++m->writes;}return s;
}
static void Stall(void* p,unsigned long us) {(void)p;(void)us;}
static unsigned long field(unsigned long reg,unsigned long mask)
{return Bc250DispGet(model.reg[reg/4],mask);}
#define F(r,f) field(REG(OTG0_##r),OTG0_##r##__##f##_MASK)
static const BC250_TARGET_TIMING hd={148500,1920,280,88,44,1080,45,4,5,0x1e};
static const BC250_TARGET_TIMING svga={40000,800,256,40,128,600,28,1,4,0x1e};
static const BC250_OPTC_SYNC sync={32,10,20,8};
static long op(BC250_DISP_IO* io,unsigned n)
{
    if(n==0)return Bc250OptcDisable(io);
    if(n==1)return Bc250OptcProgram(io,&hd,&sync,0);
    if(n==2)return Bc250OptcProgram(io,&svga,&sync,1);
    return Bc250OptcEnable(io);
}
static void readTimingTests(BC250_DISP_IO* io)
{
    CHECK(Bc250DispReadAllowed(REG(OTG0_PIXEL_RATE_CNTL)));
    CHECK(Bc250DispReadAllowed(REG(OTG0_OTG_V_TOTAL_CONTROL)));
    CHECK(Bc250DispReadAllowed(REG(OTG0_OTG_H_TIMING_CNTL)));
    CHECK(Bc250DispReadAllowed(REG(OTG0_OTG_STEREO_CONTROL)));
    BC250_TARGET_TIMING t,out,zero={0};unsigned mode,pol,i,reads,writes;
    const unsigned long reject[]={REG(OTG0_OTG_INTERLACE_CONTROL),REG(OTG0_OTG_V_TOTAL_CONTROL),
        REG(OTG0_OTG_H_TIMING_CNTL),REG(OTG0_PIXEL_RATE_CNTL),REG(OTG0_PIXEL_RATE_CNTL),
        REG(OTG0_PIXEL_RATE_CNTL),REG(OTG0_OTG_STEREO_CONTROL),REG(OTG0_OTG_H_SYNC_A),
        REG(OTG0_OTG_V_SYNC_A),REG(OTG0_OTG_H_SYNC_A_CNTL)};
    const unsigned long masks[]={OTG0_OTG_INTERLACE_CONTROL__OTG_INTERLACE_ENABLE_MASK,
        OTG0_OTG_V_TOTAL_CONTROL__OTG_V_TOTAL_MIN_SEL_MASK,OTG0_OTG_H_TIMING_CNTL__OTG_H_TIMING_DIV_BY2_MASK,
        OTG0_PIXEL_RATE_CNTL__OTG0_DROP_PIXEL_MASK,OTG0_PIXEL_RATE_CNTL__OTG0_ADD_PIXEL_MASK,
        OTG0_PIXEL_RATE_CNTL__OTG0_DISPOUT_HALF_RATE_EN_MASK,OTG0_OTG_STEREO_CONTROL__OTG_STEREO_EN_MASK,
        1,1,OTG0_OTG_H_SYNC_A_CNTL__OTG_COMP_SYNC_A_EN_MASK};
    for(mode=0;mode<2;++mode)for(pol=0;pol<4;++pol) {
        memset(&model,0,sizeof(model));t=mode?svga:hd;t.Flags=(unsigned char)(0x18u|(pol<<1));
        CHECK(Bc250OptcProgram(io,&t,&sync,(int)mode)==0);CHECK(Bc250OptcEnable(io)==0);
        model.access=0;writes=model.writes;memset(&out,0xcc,sizeof(out));
        CHECK(Bc250OptcReadTiming(io,t.PixelClockKhz,&out)==0);
        CHECK(memcmp(&t,&out,sizeof(t))==0 && model.writes==writes);reads=model.access;
        {
            BC250_DISP_IO readOnly=*io;readOnly.Write=NULL;readOnly.Stall=NULL;
            CHECK(Bc250OptcReadTiming(&readOnly,t.PixelClockKhz,&out)==0);
            CHECK(memcmp(&t,&out,sizeof(t))==0 && model.writes==writes);
        }
        for(i=1;i<=reads;++i) {
            model.access=0;model.failAt=i;model.after=0;memset(&out,0xcc,sizeof(out));
            CHECK(Bc250OptcReadTiming(io,t.PixelClockKhz,&out)==BC250_DISP_STATUS_DEVICE);
            CHECK(!memcmp(&out,&zero,sizeof(out)) && model.access==i && !model.after && model.writes==writes);
        }
        model.failAt=0;
        for(i=1;i<=reads;++i) {
            model.access=0;model.mutateAt=i;
            CHECK(Bc250OptcReadTiming(io,t.PixelClockKhz,&out)==BC250_DISP_STATUS_BUSY);
            CHECK(!memcmp(&out,&zero,sizeof(out)) && model.writes==writes);
        }
        model.mutateAt=0;
        for(i=0;i<sizeof(reject)/sizeof(reject[0]);++i) {
            model.reg[reject[i]/4]|=masks[i];
            CHECK(Bc250OptcReadTiming(io,t.PixelClockKhz,&out)<0);
            CHECK(!memcmp(&out,&zero,sizeof(out)) && model.writes==writes);
            model.reg[reject[i]/4]&=~masks[i];
        }
        model.reg[REG(OTG0_OTG_H_BLANK_START_END)/4]=0;
        CHECK(Bc250OptcReadTiming(io,t.PixelClockKhz,&out)==BC250_DISP_STATUS_INVALID);
        CHECK(!memcmp(&out,&zero,sizeof(out)) && model.writes==writes);
    }
    memset(&out,0xcc,sizeof(out));CHECK(Bc250OptcReadTiming(io,0,&out)==BC250_DISP_STATUS_INVALID);
    CHECK(!memcmp(&out,&zero,sizeof(out)));
}
int main(void)
{
    BC250_DISP_IO io={0};unsigned n,i,total;BC250_TARGET_TIMING bad;BC250_OPTC_SYNC bs;
    io.Context=&model;io.Read=Read;io.Write=Write;io.Stall=Stall;io.BudgetUs=100000;
    readTimingTests(&io);memset(&model,0,sizeof(model));
    CHECK(Bc250OptcProgram(&io,&hd,&sync,0)==0);
    CHECK(F(OTG_H_TOTAL,OTG_H_TOTAL)==2199 && F(OTG_V_TOTAL,OTG_V_TOTAL)==1124);
    CHECK(F(OTG_H_BLANK_START_END,OTG_H_BLANK_START)==2112 && F(OTG_H_BLANK_START_END,OTG_H_BLANK_END)==192);
    CHECK(F(OTG_V_BLANK_START_END,OTG_V_BLANK_START)==1121 && F(OTG_V_BLANK_START_END,OTG_V_BLANK_END)==41);
    CHECK(F(OTG_H_SYNC_A,OTG_H_SYNC_A_END)==44 && F(OTG_V_SYNC_A,OTG_V_SYNC_A_END)==5);
    CHECK(F(OTG_CONTROL,OTG_START_POINT_CNTL)==0);
    CHECK(F(OTG_V_TOTAL_MAX,OTG_V_TOTAL_MAX)==1124 && F(OTG_V_TOTAL_MIN,OTG_V_TOTAL_MIN)==1124);
    CHECK(field(REG(VTG0_CONTROL),VTG0_CONTROL__VTG0_VCOUNT_INIT_MASK)==1121);
    CHECK(F(OTG_VSTARTUP_PARAM,VSTARTUP_START)==10 && F(OTG_VREADY_PARAM,VREADY_OFFSET)==32);
    CHECK(Bc250OptcEnable(&io)==0 && F(OTG_CONTROL,OTG_MASTER_EN)==1);
    total=model.writes;
    CHECK(Bc250OptcProgram(&io,&svga,&sync,1)==BC250_DISP_STATUS_BUSY && model.writes==total);
    CHECK(Bc250OptcDisable(&io)==0 && F(OTG_CONTROL,OTG_MASTER_EN)==0);
    CHECK(Bc250OptcProgram(&io,&svga,&sync,1)==0);
    CHECK(F(OTG_H_TOTAL,OTG_H_TOTAL)==1055 && F(OTG_V_TOTAL,OTG_V_TOTAL)==627);
    CHECK(F(OTG_CONTROL,OTG_START_POINT_CNTL)==1);
    for(n=0;n<4;++n) {
        memset(&model,0,sizeof(model));CHECK(op(&io,n)==0);total=model.access;
        for(i=1;i<=total;++i) {
            memset(&model,0,sizeof(model));model.failAt=i;
            CHECK(op(&io,n)==BC250_DISP_STATUS_DEVICE);
            CHECK(model.access==i && !model.after);
        }
    }
    memset(&model,0,sizeof(model));
    model.reg[REG(OTG0_OTG_CLOCK_CONTROL)/4]=OTG0_OTG_CLOCK_CONTROL__OTG_BUSY_MASK;
    io.BudgetUs=9;io.StalledUs=0;
    CHECK(Bc250OptcDisable(&io)==BC250_DISP_STATUS_BUDGET && io.StalledUs==9);
    memset(&model,0,sizeof(model));
    bad=hd;bad.Flags|=0x80;CHECK(!Bc250OptcTimingValid(&bad,&sync));
    bad=hd;bad.HSyncWidth=7;CHECK(!Bc250OptcTimingValid(&bad,&sync));
    bad=hd;bad.VSyncOffset=0;CHECK(!Bc250OptcTimingValid(&bad,&sync));
    bad=hd;bad.HActive=0xffffffff;CHECK(!Bc250OptcTimingValid(&bad,&sync));
    bs=sync;bs.VStartup=0;CHECK(!Bc250OptcTimingValid(&hd,&bs));
    bs=sync;bs.VStartup=hd.VBlank;CHECK(!Bc250OptcTimingValid(&hd,&bs));
    // BD-177: the same gate, asked which term refused, and the name that goes into the log line.
    CHECK(Bc250OptcTimingReason(&hd,&sync)==BC250_OPTC_REASON_OK);
    CHECK(Bc250OptcTimingValid(&hd,&sync));
    CHECK(Bc250OptcTimingReason(NULL,&sync)==BC250_OPTC_REASON_EXTENT);
    CHECK(Bc250OptcTimingReason(&hd,NULL)==BC250_OPTC_REASON_EXTENT);
    bad=hd;bad.HActive=0xffffffff;CHECK(Bc250OptcTimingReason(&bad,&sync)==BC250_OPTC_REASON_EXTENT);
    bad=hd;bad.Flags|=0x80;CHECK(Bc250OptcTimingReason(&bad,&sync)==BC250_OPTC_REASON_FLAGS);
    bad=hd;bad.HSyncWidth=7;CHECK(Bc250OptcTimingReason(&bad,&sync)==BC250_OPTC_REASON_SYNC);
    bad=hd;bad.VSyncOffset=0;CHECK(Bc250OptcTimingReason(&bad,&sync)==BC250_OPTC_REASON_SYNC);
    // The reduced-blanking case of BD-177: the prefetch does not fit in the mode's vertical blanking.
    bs=sync;bs.VStartup=hd.VBlank;CHECK(Bc250OptcTimingReason(&hd,&bs)==BC250_OPTC_REASON_VSTARTUP_BLANK);
    bs=sync;bs.VStartup=0;CHECK(Bc250OptcTimingReason(&hd,&bs)==BC250_OPTC_REASON_VSTARTUP_BLANK);
    bs=sync;bs.VReady=0xffffffff;CHECK(Bc250OptcTimingReason(&hd,&bs)==BC250_OPTC_REASON_VREADY_FIELD);
    bs=sync;bs.VUpdateOffset=0xffffffff;
    CHECK(Bc250OptcTimingReason(&hd,&bs)==BC250_OPTC_REASON_VUPDATE_OFFSET_FIELD);
    bs=sync;bs.VUpdateWidth=0xffffffff;
    CHECK(Bc250OptcTimingReason(&hd,&bs)==BC250_OPTC_REASON_VUPDATE_WIDTH_FIELD);
    CHECK(!strcmp(Bc250OptcReasonName(BC250_OPTC_REASON_OK),"optc-ok"));
    CHECK(!strcmp(Bc250OptcReasonName(BC250_OPTC_REASON_VSTARTUP_BLANK),"optc-vstartup-blank"));
    CHECK(!strcmp(Bc250OptcReasonName(BC250_OPTC_REASON_VUPDATE_WIDTH_FIELD),"optc-vupdate-width-field"));
    CHECK(!strcmp(Bc250OptcReasonName(-1),"optc-invalid"));
    CHECK(!strcmp(Bc250OptcReasonName(BC250_OPTC_REASON_VUPDATE_WIDTH_FIELD+1),"optc-invalid"));
    for(n=0;n<=(unsigned)BC250_OPTC_REASON_VUPDATE_WIDTH_FIELD;++n) {
        const char* name=Bc250OptcReasonName((int)n);
        // 32 is what a %s costs a driver log line (tools/quality/guardlog_width.py), so no term may reach it.
        CHECK(name && *name && strcmp(name,"optc-invalid") && strlen(name)<32);
    }
    CHECK(Bc250OptcProgram(&io,&bad,&sync,0)==BC250_DISP_STATUS_INVALID && !model.access);
    CHECK(Bc250OptcProgram(&io,&hd,&sync,2)==BC250_DISP_STATUS_INVALID && !model.access);
    printf("optc_test: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
