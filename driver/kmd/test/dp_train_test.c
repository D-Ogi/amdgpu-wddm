// Protocol model only. No GPU, firmware or lab access.
#include "../../shim/bc250_dp_train.h"
#include "../../shim/dpcd_constants.inc"
#include <stdio.h>
#include <string.h>
static unsigned checks,failures;
#define CHECK(x) do { ++checks;if(!(x)) { ++failures;printf("line %u: %s\n",__LINE__,#x); } } while(0)
typedef struct MOCK {
    uint64_t clock;
    unsigned calls,failAt,cancelAt,delayExtra,active,enabled,disables;
    unsigned pattern,rate,lanes,swing,emphasis,reads,writes,drives,starts,stops;
    unsigned failCr,failEq,lostCr,oscillate,adjust,failDisable,requireAdjust;
    unsigned maxSwing,maxEmphasis,rxPattern;
    uint8_t laneset[4];
} MOCK;
static int step(MOCK *m)
{
    ++m->calls;m->clock+=3;
    if(m->calls==m->cancelAt) m->active=0;
    return m->calls==m->failAt?-1:0;
}
static int Current(void *p) { return (int)((MOCK*)p)->active; }
static uint64_t Now(void *p) { return ((MOCK*)p)->clock; }
static int Delay(void *p,unsigned us)
{ MOCK *m=p;m->clock+=us+m->delayExtra;return step(m); }
static int Write(void *p,uint32_t address,const uint8_t *b,unsigned n)
{
    MOCK *m=p;++m->writes;if(step(m)) return -1;
    if(address==DP_TRAINING_PATTERN_SET) {
        CHECK(n==1 || n==m->lanes+1);
        m->rxPattern=b[0]&3;
        if(!m->rxPattern) ++m->stops;else ++m->starts;
        if(n>1) memcpy(m->laneset,b+1,n-1);
    } else if(address==DP_TRAINING_LANE0_SET) {
        CHECK(n==m->lanes);memcpy(m->laneset,b,n);
    } else if(address==DP_LINK_BW_SET) {
        CHECK(n==2 && b[0]==m->rate && ((unsigned)b[1]&31u)==m->lanes);
    } else if(address==DP_DOWNSPREAD_CTRL) CHECK(n==2 && b[0]==0 && b[1]==DP_SET_ANSI_8B10B);
    else CHECK(address==DP_SET_POWER && n==1 && b[0]==DP_SET_POWER_D0);
    return 0;
}
static int Read(void *p,uint32_t address,uint8_t *b,unsigned n)
{
    MOCK *m=p;unsigned bits,i;int fail;
    ++m->reads;fail=step(m);
    CHECK(address==DP_LANE0_1_STATUS && n==6 && m->enabled);
    CHECK(m->rxPattern==m->pattern);
    memset(b,0,n);if(fail) return -1;
    bits=m->pattern==1?1u:7u;
    if(m->failCr==2 || (m->failCr && m->rate==10) || m->oscillate) bits=0;
    if(m->pattern==2 && m->lostCr) bits=0;
    if(m->pattern==2 && m->failEq) bits=1;
    if(m->requireAdjust && m->reads==1) bits=0;
    for(i=0;i<m->lanes;++i) b[i/2]|=(uint8_t)(bits<<((i%2)*4));
    b[2]=m->failEq?0:1;
    if(m->adjust) { b[4]=0x51;b[5]=0x9b; } // max VS3, PE2 -> common VS3/PE0
    if(m->oscillate) b[4]=b[5]=(uint8_t)((m->reads&1)?0x11:0);
    return 0;
}
static int Enable(void *p,unsigned rate,unsigned lanes,int enhanced)
{
    MOCK *m=p;m->enabled=1;m->rate=rate;m->lanes=lanes;
    CHECK(rate==6 || rate==10);CHECK(lanes==1 || lanes==2 || lanes==4);CHECK(enhanced==1);
    return step(m);
}
static int Pattern(void *p,unsigned pattern)
{ MOCK *m=p;m->pattern=pattern;CHECK(pattern<=2);return step(m); }
static int Drive(void *p,unsigned vs,unsigned pe)
{
    MOCK *m=p;++m->drives;m->swing=vs;m->emphasis=pe;
    if(vs>m->maxSwing)m->maxSwing=vs;if(pe>m->maxEmphasis)m->maxEmphasis=pe;
    CHECK(vs+pe<=3);return step(m);
}
static int Disable(void *p)
{ MOCK *m=p;++m->disables;m->enabled=0;return m->failDisable?-1:step(m); }
static void init(MOCK *m,uint8_t caps[16])
{ memset(m,0,sizeof(*m));memset(caps,0,16);m->active=1;caps[0]=0x12;caps[1]=10;caps[2]=0x84; }
static enum BC250_DP_TRAIN_STATUS train(MOCK *m,uint8_t caps[16],uint32_t pixel,BC250_DP_TRAIN_RESULT *r)
{
    BC250_DP_TRAIN_IO io={m,Current,Read,Write,Enable,Pattern,Drive,Disable,Now,Delay};
    return Bc250DpTrain(&io,caps,pixel,r);
}
int main(void)
{
    MOCK m;uint8_t caps[16];BC250_DP_TRAIN_RESULT r;unsigned i,ops;
    init(&m,caps);CHECK(Bc250DpMaxPixelClock(caps)==360000);
    CHECK(train(&m,caps,154000,&r)==BC250_DP_TRAIN_OK);
    CHECK(r.CrSamples==1 && r.EqSamples==1 && r.Phase==3 && r.Attempts==1);
    CHECK(m.enabled && m.pattern==0 && m.stops==1 && !m.disables);ops=m.calls;
    for(i=1;i<=ops;++i) {
        init(&m,caps);m.failAt=i;
        CHECK(train(&m,caps,154000,&r)==BC250_DP_TRAIN_IO_ERROR);
        CHECK(!m.enabled && m.disables==1 && !r.CleanupFailed);
    }
    for(i=1;i<=ops;++i) {
        init(&m,caps);m.cancelAt=i;
        CHECK(train(&m,caps,154000,&r)==BC250_DP_TRAIN_CANCELLED);
        CHECK(!m.enabled && m.disables==1);
    }
    init(&m,caps);m.active=0;
    CHECK(train(&m,caps,154000,&r)==BC250_DP_TRAIN_CANCELLED);CHECK(!m.calls);
    init(&m,caps);m.delayExtra=2000000;
    CHECK(train(&m,caps,154000,&r)==BC250_DP_TRAIN_TIMEOUT);CHECK(!m.enabled);
    init(&m,caps);m.failCr=1;
    CHECK(train(&m,caps,154000,&r)==BC250_DP_TRAIN_OK);
    CHECK(r.Attempts==2 && r.Rate==6 && m.disables==1 && m.enabled);
    init(&m,caps);m.failCr=2;
    CHECK(train(&m,caps,250000,&r)==BC250_DP_TRAIN_CR_FAILED);
    CHECK(r.Attempts==1 && r.CrSamples==5 && !m.enabled);
    init(&m,caps);m.failEq=1;
    CHECK(train(&m,caps,250000,&r)==BC250_DP_TRAIN_EQ_FAILED);
    CHECK(r.EqSamples==6 && !m.enabled);
    init(&m,caps);m.lostCr=1;
    CHECK(train(&m,caps,250000,&r)==BC250_DP_TRAIN_EQ_FAILED);CHECK(r.EqSamples==1);
    init(&m,caps);m.oscillate=1;
    CHECK(train(&m,caps,250000,&r)==BC250_DP_TRAIN_CR_FAILED);
    CHECK(r.CrSamples==100 && r.ElapsedUs<2000000 && !m.enabled);
    init(&m,caps);m.adjust=1;m.requireAdjust=1;
    CHECK(train(&m,caps,154000,&r)==BC250_DP_TRAIN_OK);
    CHECK(m.maxSwing==3 && m.maxEmphasis==0 && m.laneset[0]==7 && m.laneset[3]==7);
    init(&m,caps);m.failCr=2;m.failDisable=1;
    CHECK(train(&m,caps,154000,&r)==BC250_DP_TRAIN_IO_ERROR);CHECK(r.CleanupFailed && r.Attempts==1);
    for(i=1;i<=4;i*=2) {
        init(&m,caps);caps[2]=(uint8_t)(0x80|i);
        CHECK(train(&m,caps,50000,&r)==BC250_DP_TRAIN_OK);CHECK(r.Lanes==i);
    }
    init(&m,caps);caps[0]=0;
    CHECK(train(&m,caps,154000,&r)==BC250_DP_TRAIN_UNSUPPORTED);CHECK(!m.calls);
    init(&m,caps);caps[1]=0xff;
    CHECK(train(&m,caps,154000,&r)==BC250_DP_TRAIN_UNSUPPORTED);CHECK(!m.calls);
    init(&m,caps);caps[2]=3;
    CHECK(train(&m,caps,154000,&r)==BC250_DP_TRAIN_UNSUPPORTED);CHECK(!m.calls);
    init(&m,caps);caps[14]=127;
    CHECK(train(&m,caps,154000,&r)==BC250_DP_TRAIN_UNSUPPORTED);CHECK(!m.calls);
    init(&m,caps);
    CHECK(train(&m,caps,360001,&r)==BC250_DP_TRAIN_UNSUPPORTED);CHECK(!m.calls);
    CHECK(Bc250DpTrain(NULL,caps,1,&r)==BC250_DP_TRAIN_ARGUMENT);
    printf("DP training: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
