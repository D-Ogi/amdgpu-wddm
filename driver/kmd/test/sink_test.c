// Protocol boundary tests with controlled AUX/DDC outcomes. Real EDID and DP-cap decoding.
#include <stdio.h>
#include <string.h>
#include "../dpaux.h"
#include "../../shim/bc250_sink.h"
#include "../../shim/bc250_dp_train.h"

static unsigned checks,failures;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL %u: %s\n",(unsigned)__LINE__,#x);}}while(0)
typedef struct MODEL {
    unsigned calls,cancelAt,dp,auxOpenFail,auxCloseFail,ddcCloseFail,badEdid,unknownAdapter,badCaps;
    unsigned rate,fast;     // DPCD MAX_LINK_RATE (0 for HBR); a second DTD of 2560x1440 at 165 Hz
    unsigned uhd;           // GitHub #45 review: a third DTD, 3840x2160 at 60 Hz (533.25 MHz)
    unsigned rev,present,portFail,portReads; // DPCD_REV (0 for 1.2), DPCD 0x005, a failed read of 0x080, reads of it
    unsigned char port[16]; // DPCD 0x080-0x08F
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
    if(m.fast) {
        // BD-139: 2560x1440 645.1 MHz, 2640x1481 (165 Hz), +H -V, the reduced blanking of 1440p 165 Hz monitors.
        d=b+72;d[0]=0xfe;d[1]=0xfb;d[2]=0x00;d[3]=0x50;d[4]=0xa0;
        d[5]=0xa0;d[6]=0x29;d[7]=0x50;d[8]=8;d[9]=32;d[10]=0x38;d[17]=0x1a;
    }
    if(m.uhd) {
        // 3840x2160 533.25 MHz, 4000x2222 (60 Hz), +H -V: the Philips 27E1N8900 preferred timing of GitHub #45.
        d=b+90;d[0]=0x4d;d[1]=0xd0;d[2]=0x00;d[3]=0xa0;d[4]=0xf0;
        d[5]=0x70;d[6]=0x3e;d[7]=0x80;d[8]=48;d[9]=32;d[10]=0x35;d[17]=0x1a;
    }
    for(i=0;i<127;++i)sum+=b[i];b[127]=(unsigned char)(0u-sum);
    if(m.badEdid)b[127]^=1;*n=128;
}
long Bc250AuxOpenForDetection(BC250_AUX_SESSION* s,BC250_DISP_IO* io)
{s->Io=io;if(step(io)<0)return BC250_DISP_STATUS_BUSY;return m.auxOpenFail?BC250_DISP_STATUS_DEVICE:0;}
long Bc250AuxDpcdRead(BC250_AUX_SESSION* s,unsigned long addr,unsigned char* p,unsigned long n)
{
    CHECK(n==16 && (addr==0 || addr==BC250_DP_DOWNSTREAM_PORT_DPCD));
    if(step(s->Io)<0)return BC250_DISP_STATUS_BUSY;
    if(!m.dp)return BC250_DISP_STATUS_DEVICE;
    if(addr) {
        // Only a branch device on DPCD 1.1 or later is asked for its ports.
        CHECK((m.present&1) && (!m.rev || m.rev>=0x11));++m.portReads;
        if(m.portFail)return BC250_DISP_STATUS_DEVICE;
        memcpy(p,m.port,n);return 0;
    }
    memset(p,0,n);p[0]=(unsigned char)(m.rev?m.rev:0x12);p[1]=(unsigned char)(m.badCaps?0:m.rate?m.rate:10);p[2]=0x84;
    p[5]=(unsigned char)m.present;p[7]=(unsigned char)(m.present&1);return 0;
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
static long discoverWithin(unsigned long dp,unsigned long tmds)
{
    BC250_DISP_IO io={0};BC250_SINK_GUARD guard={&m,Current};
    io.Context=&m;io.Read=Read;io.Write=Write;io.Stall=Stall;io.BudgetUs=400000;
    return Bc250SinkDiscover(&io,&guard,dp,tmds,4096,2160,&result);
}
static long discover(void) { return discoverWithin(BC250_DP_MAX_PIXEL_KHZ,300000); }
/* GitHub #45 review: an active DP converter ends the DP link, so the DP link rate is no longer the limit. Every case
 * is an HBR2 x4 sink (720 MHz on its link) with three timings: 1080p60 148.5, 4K60 533.25 and 1440p165 645.1 MHz. */
static void branch(unsigned present,unsigned type,unsigned max)
{
    memset(&m,0,sizeof(m));m.dp=m.fast=m.uhd=1;m.rate=0x14;m.present=present;m.port[0]=(unsigned char)type;
    m.port[1]=(unsigned char)max;
}
static void downstream(void)
{
    /* A native DP sink: no branch device, no read of 0x080, all three timings. */
    branch(0,0,0);
    CHECK(discover()==0 && result.DownstreamType==BC250_DP_DOWNSTREAM_NONE && !result.DownstreamKhz);
    CHECK(result.MaxPixelClockKhz==720000 && result.TargetCount==3 && !m.portReads && !result.DownstreamRead);
    /* A branch device with a DP downstream port (a hub): the DP link stays the limit. */
    branch(0x11,0,0);
    CHECK(discover()==0 && result.DownstreamType==BC250_DP_DOWNSTREAM_DP && !result.DownstreamKhz);
    CHECK(result.MaxPixelClockKhz==720000 && result.TargetCount==3 && m.portReads==1 && result.DownstreamRead);
    /* HDMI converters that state 340 and 600 MHz (byte 1 in 2.5 MHz units, DPCD 0x005 TMDS with detailed caps). */
    branch(0x15,3,136);
    CHECK(discover()==0 && result.DownstreamType==BC250_DP_DOWNSTREAM_HDMI && result.DownstreamKhz==340000);
    CHECK(result.MaxPixelClockKhz==340000 && result.TargetCount==1 && result.Targets[0].PixelClockKhz==148500);
    branch(0x15,3,240);
    CHECK(discover()==0 && result.DownstreamKhz==600000 && result.MaxPixelClockKhz==600000 && result.TargetCount==2);
    CHECK(result.Targets[1].PixelClockKhz==533250 && result.Targets[1].HActive==3840);
    /* A DP++ port is an HDMI port. */
    branch(0x15,5,240);
    CHECK(discover()==0 && result.DownstreamType==BC250_DP_DOWNSTREAM_HDMI && result.MaxPixelClockKhz==600000);
    /* An HDMI converter without detailed caps, and one with a 0 byte: 300 MHz. */
    branch(0x05,3,240);
    CHECK(discover()==0 && result.DownstreamType==BC250_DP_DOWNSTREAM_HDMI && result.DownstreamKhz==300000);
    CHECK(result.MaxPixelClockKhz==300000 && result.TargetCount==1);
    branch(0x15,3,0);
    CHECK(discover()==0 && result.DownstreamKhz==300000 && result.TargetCount==1);
    /* A DVI converter states its TMDS clock the same way: 165 MHz. */
    branch(0x15,2,66);
    CHECK(discover()==0 && result.DownstreamType==BC250_DP_DOWNSTREAM_DVI && result.DownstreamKhz==165000);
    CHECK(result.TargetCount==1 && result.Targets[0].PixelClockKhz==148500);
    /* VGA: its own limit, byte 1 in 8 MHz units (25 = 200 MHz), else 300 MHz. */
    branch(0x13,1,25);
    CHECK(discover()==0 && result.DownstreamType==BC250_DP_DOWNSTREAM_VGA && result.DownstreamKhz==200000);
    CHECK(result.MaxPixelClockKhz==200000 && result.TargetCount==1);
    branch(0x13,1,100);
    CHECK(discover()==0 && result.DownstreamKhz==800000 && result.MaxPixelClockKhz==720000 && result.TargetCount==3);
    branch(0x03,1,25);
    CHECK(discover()==0 && result.DownstreamType==BC250_DP_DOWNSTREAM_VGA && result.DownstreamKhz==300000);
    /* An unknown type (a port without EDID, a wireless port): 300 MHz. */
    branch(0x15,4,240);
    CHECK(discover()==0 && result.DownstreamType==BC250_DP_DOWNSTREAM_OTHER && result.DownstreamKhz==300000);
    branch(0x17,6,240);
    CHECK(discover()==0 && result.DownstreamType==BC250_DP_DOWNSTREAM_OTHER && result.DownstreamKhz==300000);
    /* DPCD 0x005 alone, as Linux decodes it: DPCD 1.0 has no 0x080 to read, and a failed read leaves this class. */
    branch(0x15,0,0);m.rev=0x10;
    CHECK(discover()==0 && !m.portReads && result.DownstreamType==BC250_DP_DOWNSTREAM_DVI);
    CHECK(result.DownstreamKhz==300000 && result.TargetCount==1);
    branch(0x15,0,0);m.portFail=1;
    CHECK(discover()==0 && m.portReads==1 && !result.DownstreamRead && result.DownstreamType==BC250_DP_DOWNSTREAM_DVI);
    CHECK(result.DownstreamKhz==300000 && result.TargetCount==1);
    branch(0x11,0,0);m.portFail=1;
    CHECK(discover()==0 && result.DownstreamType==BC250_DP_DOWNSTREAM_DP && result.TargetCount==3);
    /* The converter limit is under the link: an HBR x4 sink behind a 600 MHz HDMI converter stays at 360 MHz. */
    branch(0x15,3,240);m.rate=0;
    CHECK(discover()==0 && result.DownstreamKhz==600000 && result.MaxPixelClockKhz==360000 && result.TargetCount==1);
    /* The shared function: the same answers without a discovery (connector.c uses it for the firmware's sink). */
    {
        unsigned char caps[16]={0x12,0x14,0x84};unsigned type=99;uint32_t converter=1;
        CHECK(Bc250DpSinkMaxPixelClock(caps,NULL,&type,&converter)==720000 && type==0 && !converter);
        caps[5]=0x15;m.port[0]=3;m.port[1]=136;
        CHECK(Bc250DpSinkMaxPixelClock(caps,m.port,&type,&converter)==340000);
        CHECK(type==BC250_DP_DOWNSTREAM_HDMI && converter==340000);
        CHECK(Bc250DpSinkMaxPixelClock(caps,NULL,&type,&converter)==300000 && type==BC250_DP_DOWNSTREAM_DVI);
        caps[1]=0;CHECK(Bc250DpSinkMaxPixelClock(caps,m.port,&type,&converter)==0);
        CHECK(Bc250DpDownstreamLimit(NULL,NULL,&type)==0 && type==BC250_DP_DOWNSTREAM_NONE);
    }
}
int main(void)
{
    unsigned i,dp,calls;
    memset(&m,0,sizeof(m));m.dp=1;
    CHECK(discover()==0 && result.Kind==BC250_SINK_DP && result.Phase==BC250_SINK_PHASE_READY);
    CHECK(result.TargetCount==1 && result.Targets[0].PixelClockKhz==148500);
    CHECK(m.auxClose==1 && m.ddcOpen==0);
    // BD-139: each protocol has its own ceiling. An HBR x4 sink stays at what HBR carries, 360 MHz; an HBR2 x4 sink
    // gets 720 MHz and the 165 Hz timing; the same EDID behind a TMDS adapter stays at 300 MHz.
    memset(&m,0,sizeof(m));m.dp=m.fast=1;
    CHECK(discover()==0 && result.MaxPixelClockKhz==360000 && result.TargetCount==1);
    memset(&m,0,sizeof(m));m.dp=m.fast=1;m.rate=0x14;
    CHECK(discover()==0 && result.MaxPixelClockKhz==720000 && result.TargetCount==2);
    CHECK(result.Targets[1].PixelClockKhz==645100 && result.Targets[1].HActive==2560 && result.Targets[1].VBlank==41);
    memset(&m,0,sizeof(m));m.dp=m.fast=1;m.rate=0x14;
    CHECK(discoverWithin(600000,300000)==0 && result.MaxPixelClockKhz==600000 && result.TargetCount==1);
    memset(&m,0,sizeof(m));m.dp=m.fast=1;m.rate=0x14;
    CHECK(discoverWithin(BC250_DP_MAX_PIXEL_KHZ,1)==0 && result.TargetCount==2);
    memset(&m,0,sizeof(m));m.fast=1;
    CHECK(discover()==0 && result.Kind==BC250_SINK_DVI && result.MaxPixelClockKhz==300000 && result.TargetCount==1);
    memset(&m,0,sizeof(m));m.fast=1;
    CHECK(discoverWithin(BC250_DP_MAX_PIXEL_KHZ,165000)==0 && result.MaxPixelClockKhz==165000 && result.TargetCount==1);
    downstream();
    memset(&m,0,sizeof(m));m.dp=1;
    CHECK(discoverWithin(0,300000)==BC250_DISP_STATUS_INVALID && discoverWithin(300000,0)==BC250_DISP_STATUS_INVALID);
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
