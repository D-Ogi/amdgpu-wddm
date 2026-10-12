// Protocol boundary tests with controlled AUX/DDC outcomes. Real EDID and DP-cap decoding.
#include <stdio.h>
#include <string.h>
#include "../dpaux.h"
#include "../../shim/bc250_sink.h"

static unsigned checks,failures;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL %u: %s\n",(unsigned)__LINE__,#x);}}while(0)
typedef struct MODEL {
    unsigned calls,cancelAt,dp,auxOpenFail,auxCloseFail,ddcCloseFail,badEdid,unknownAdapter,badCaps;
    unsigned ddcOpen,auxClose,ddcClose,cleanupReads,backendReads;
} MODEL;
static MODEL m;
static BC250_SINK result;
static int Current(void* context) {MODEL* model=context;return !model->cancelAt || model->calls<model->cancelAt;}
static long Read(void* ctx,unsigned long reg,unsigned long* value)
{MODEL* model=ctx;(void)reg;*value=0;++model->backendReads;return 0;}
static long Write(void* ctx,unsigned long reg,unsigned long value) {(void)ctx;(void)reg;(void)value;return 0;}
static void Stall(void* ctx,unsigned long us) {(void)ctx;(void)us;}
static long step(BC250_DISP_IO* io)
{
    unsigned long value;long s=io->Read(io->Context,0,&value);++m.calls;return s;
}
static long closeStep(BC250_DISP_IO* io)
{
    long s=step(io);if(s>=0)++m.cleanupReads;return s;
}
static void edid(unsigned char* b,unsigned long* n)
{
    unsigned i,sum=0;unsigned char* d;
    memset(b,0,128);b[0]=b[7]=0;for(i=1;i<7;++i)b[i]=255;
    b[18]=1;b[19]=4;b[20]=0x80;d=b+54;
    // EDID DTD: 1920x1080 148.5 MHz, 2200x1125, positive digital separate sync.
    d[0]=0x02;d[1]=0x3a;d[2]=0x80;d[3]=0x18;d[4]=0x71;
    d[5]=0x38;d[6]=0x2d;d[7]=0x40;d[8]=88;d[9]=44;d[10]=0x45;d[17]=0x1e;
    for(i=0;i<127;++i)sum+=b[i];b[127]=(unsigned char)(0u-sum);
    if(m.badEdid)b[127]^=1;*n=128;
}
long Bc250AuxOpenForDetection(BC250_AUX_SESSION* s,BC250_DISP_IO* io)
{s->Io=io;if(step(io)<0)return BC250_DISP_STATUS_BUSY;return m.auxOpenFail?BC250_DISP_STATUS_DEVICE:0;}
long Bc250AuxDpcdRead(BC250_AUX_SESSION* s,unsigned long addr,unsigned char* p,unsigned long n)
{
    (void)addr;CHECK(n==16);
    if(step(s->Io)<0)return BC250_DISP_STATUS_BUSY;
    if(!m.dp)return BC250_DISP_STATUS_DEVICE;
    memset(p,0,n);p[0]=0x12;p[1]=m.badCaps?0:10;p[2]=0x84;return 0;
}
long Bc250AuxReadEdid(BC250_AUX_SESSION* s,unsigned char* p,unsigned long cap,unsigned long* n)
{CHECK(cap>=128);if(step(s->Io)<0)return BC250_DISP_STATUS_BUSY;edid(p,n);return 0;}
long Bc250AuxClose(BC250_AUX_SESSION* s)
{++m.auxClose;CHECK(closeStep(s->Io)==0);return m.auxCloseFail?BC250_DISP_STATUS_DEVICE:0;}
long bc250_ddc_open(BC250_DDC_SESSION* s,BC250_DISP_IO* io)
{++m.ddcOpen;s->io=io;return step(io);}
long bc250_ddc_read_adapter(BC250_DDC_SESSION* s,BC250_DUAL_MODE_ADAPTER* a)
{
    if(step(s->io)<0)return BC250_DISP_STATUS_BUSY;
    memset(a,0,sizeof(*a));
    if(!m.unknownAdapter){a->type=BC250_ADAPTER_TYPE2_HDMI;a->max_tmds_khz=300000;}
    return 0;
}
long bc250_ddc_read_edid(BC250_DDC_SESSION* s,unsigned char* p,unsigned long cap,unsigned long* n)
{CHECK(cap>=128);if(step(s->io)<0)return BC250_DISP_STATUS_BUSY;edid(p,n);return 0;}
long bc250_ddc_close(BC250_DDC_SESSION* s)
{++m.ddcClose;CHECK(closeStep(s->io)==0);return m.ddcCloseFail?BC250_DISP_STATUS_DEVICE:0;}
static long discover(void)
{
    BC250_DISP_IO io={0};BC250_SINK_GUARD guard={&m,Current};
    io.Context=&m;io.Read=Read;io.Write=Write;io.Stall=Stall;io.BudgetUs=400000;
    return Bc250SinkDiscover(&io,&guard,300000,4096,2160,&result);
}
int main(void)
{
    unsigned i,dp,calls;
    memset(&m,0,sizeof(m));m.dp=1;
    CHECK(discover()==0 && result.Kind==BC250_SINK_DP && result.Phase==BC250_SINK_PHASE_READY);
    CHECK(result.TargetCount==1 && result.Targets[0].PixelClockKhz==148500);
    CHECK(m.auxClose==1 && m.ddcOpen==0);
    memset(&m,0,sizeof(m));
    CHECK(discover()==0 && result.Kind==BC250_SINK_DVI && result.Adapter.type==BC250_ADAPTER_TYPE2_HDMI);
    CHECK(m.auxClose==1 && m.ddcClose==1 && result.TargetCount==1);
    // Receiver DPCD success prevents protocol fallback on bad caps or EDID.
    memset(&m,0,sizeof(m));m.dp=m.badCaps=1;
    CHECK(discover()<0 && result.Kind==BC250_SINK_DP && m.ddcOpen==0);
    memset(&m,0,sizeof(m));m.dp=m.badEdid=1;
    CHECK(discover()<0 && m.ddcOpen==0 && result.TargetCount==0);
    memset(&m,0,sizeof(m));m.unknownAdapter=1;
    CHECK(discover()<0 && result.Kind==BC250_SINK_NONE && result.TargetCount==0);
    // Cleanup failure is fatal even if a second protocol would answer.
    memset(&m,0,sizeof(m));m.auxCloseFail=1;
    CHECK(discover()<0 && m.ddcOpen==0 && result.CleanupStatus<0);
    memset(&m,0,sizeof(m));m.auxOpenFail=1;
    CHECK(discover()==0 && m.auxClose==1 && m.ddcOpen==1);
    memset(&m,0,sizeof(m));m.ddcCloseFail=1;
    CHECK(discover()<0 && result.CleanupStatus<0 && result.TargetCount==0);
    // Unplug at any operation cancels the result but does not suppress owner cleanup.
    for(dp=0;dp<2;++dp) {
        memset(&m,0,sizeof(m));m.dp=dp;CHECK(discover()==0);calls=m.calls;
        for(i=1;i<=calls;++i) {
            memset(&m,0,sizeof(m));m.dp=dp;m.cancelAt=i;
            CHECK(discover()==BC250_DISP_STATUS_BUSY);
            CHECK(m.cleanupReads>=1 && result.TargetCount==0);
        }
    }
    printf("sink_test: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
