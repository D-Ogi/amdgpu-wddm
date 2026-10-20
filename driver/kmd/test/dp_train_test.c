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
    unsigned failCrRate,eqPatterns,badScramble; // CR fails at this rate; EQ patterns the sink saw; scrambling errors
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
        unsigned set=b[0]&15u;
        CHECK(n==1 || n==m->lanes+1);
        CHECK(set==DP_TRAINING_PATTERN_DISABLE || set==DP_TRAINING_PATTERN_1 || set==DP_TRAINING_PATTERN_2 ||
            set==DP_TRAINING_PATTERN_3 || set==DP_TRAINING_PATTERN_4);
        m->rxPattern=set==DP_TRAINING_PATTERN_4?4u:set;
        // DP 1.4: TPS1 to TPS3 are sent with scrambling disabled, TPS4 scrambled.
        if(m->rxPattern && ((b[0]&DP_LINK_SCRAMBLING_DISABLE)!=0)!=(m->rxPattern!=4u)) ++m->badScramble;
        if(m->rxPattern>=2) m->eqPatterns|=1u<<m->rxPattern;
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
    if(m->failCr==2 || (m->failCr && m->rate==10) || (m->failCrRate && m->rate==m->failCrRate) || m->oscillate) bits=0;
    if(m->pattern>=2 && m->lostCr) bits=0;
    if(m->pattern>=2 && m->failEq) bits=1;
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
    CHECK(rate==6 || rate==10 || rate==20 || rate==30);CHECK(lanes==1 || lanes==2 || lanes==4);CHECK(enhanced==1);
    return step(m);
}
static int Pattern(void *p,unsigned pattern)
{ MOCK *m=p;m->pattern=pattern;CHECK(pattern<=4);return step(m); }
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
    CHECK(r.FirstRate==10 && r.EqPattern==2 && m.eqPatterns==(1u<<2) && !m.badScramble);
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
    // BD-139, the rate ladder. HBR2 x4 with TPS3 and enhanced framing (DPCD 1.2): the least a 2560x1440 165 Hz DP
    // sink must claim. These are model caps; no DPCD dump of the HP X32 of BD-139 exists yet.
    init(&m,caps);caps[1]=0x14;caps[2]=0xc4;
    CHECK(Bc250DpMaxPixelClock(caps)==720000);
    CHECK(Bc250DpFirstRate(caps,154000)==10 && Bc250DpFirstRate(caps,304250)==10 && Bc250DpFirstRate(caps,360000)==10);
    CHECK(Bc250DpFirstRate(caps,360001)==20 && Bc250DpFirstRate(caps,645100)==20 && Bc250DpFirstRate(caps,720000)==20);
    CHECK(Bc250DpFirstRate(caps,720001)==0 && Bc250DpFirstRate(caps,0)==0);
    CHECK(train(&m,caps,645100,&r)==BC250_DP_TRAIN_OK);
    CHECK(r.Rate==20 && r.FirstRate==20 && r.EqPattern==3 && r.Attempts==1 && m.rate==20 && r.Lanes==4);
    CHECK(r.Tried[0].Rate==20 && r.Tried[0].EqPattern==3 && r.Tried[0].Phase==2 && r.Tried[0].AmdResult==0);
    CHECK(m.eqPatterns==(1u<<3) && !m.badScramble && m.enabled && m.pattern==0 && !m.disables);ops=m.calls;
    for(i=1;i<=ops;++i) {
        init(&m,caps);caps[1]=0x14;caps[2]=0xc4;m.failAt=i;
        CHECK(train(&m,caps,645100,&r)==BC250_DP_TRAIN_IO_ERROR);
        CHECK(!m.enabled && m.disables==1 && r.Attempts==1 && r.Rate==20);
    }
    // A mode HBR carries trains on an HBR2 sink exactly as before: HBR first, TPS2, RBR when HBR fails.
    init(&m,caps);caps[1]=0x14;caps[2]=0xc4;
    CHECK(train(&m,caps,304250,&r)==BC250_DP_TRAIN_OK && r.Rate==10 && r.EqPattern==2 && m.eqPatterns==(1u<<2));
    init(&m,caps);caps[1]=0x14;caps[2]=0xc4;m.failCr=1;
    CHECK(train(&m,caps,154000,&r)==BC250_DP_TRAIN_OK && r.Rate==6 && r.Attempts==2);
    CHECK(r.Tried[0].Rate==10 && r.Tried[0].Phase==1 && r.Tried[0].AmdResult!=0 && r.Tried[1].Rate==6);
    // A failed HBR2 training of a mode HBR cannot carry ends there with the PHY off. No slower rate carries the
    // mode; the connector keeps this sink below HBR2 instead (connector_link.c).
    init(&m,caps);caps[1]=0x14;caps[2]=0xc4;m.failCrRate=20;
    CHECK(train(&m,caps,645100,&r)==BC250_DP_TRAIN_CR_FAILED);
    CHECK(r.Attempts==1 && r.FirstRate==20 && r.Tried[0].Rate==20 && r.Tried[0].Phase==1 && !m.enabled && m.disables==1);
    init(&m,caps);caps[1]=0x14;caps[2]=0xc4;m.failEq=1;
    CHECK(train(&m,caps,645100,&r)==BC250_DP_TRAIN_EQ_FAILED);
    CHECK(r.Attempts==1 && r.Tried[0].Phase==2 && r.Tried[0].EqPattern==3 && !m.enabled);
    // No TPS3 bit: TPS2 (Linux decide_eq_training_pattern).
    init(&m,caps);caps[1]=0x14;caps[2]=0x84;
    CHECK(train(&m,caps,645100,&r)==BC250_DP_TRAIN_OK && r.EqPattern==2 && m.eqPatterns==(1u<<2));
    // TPS4 (MAX_DOWNSPREAD bit 7) wins over TPS3 and goes out scrambled.
    init(&m,caps);caps[0]=0x14;caps[1]=0x14;caps[2]=0xc4;caps[3]=0x80;
    CHECK(train(&m,caps,645100,&r)==BC250_DP_TRAIN_OK && r.EqPattern==4 && m.eqPatterns==(1u<<4) && !m.badScramble);
    // HBR3: what HBR2 x4 cannot carry trains at HBR3; the 165 Hz mode still starts at HBR2.
    init(&m,caps);caps[0]=0x14;caps[1]=0x1e;caps[2]=0xc4;caps[3]=0x80;
    CHECK(Bc250DpMaxPixelClock(caps)==1080000 && Bc250DpFirstRate(caps,720001)==30 && Bc250DpFirstRate(caps,1080001)==0);
    CHECK(train(&m,caps,1080000,&r)==BC250_DP_TRAIN_OK && r.Rate==30 && m.rate==30 && r.EqPattern==4);
    init(&m,caps);caps[0]=0x14;caps[1]=0x1e;caps[2]=0xc4;caps[3]=0x80;
    CHECK(train(&m,caps,645100,&r)==BC250_DP_TRAIN_OK && r.Rate==20 && r.Attempts==1);
    init(&m,caps);caps[0]=0x14;caps[1]=0x1e;caps[2]=0xc4;caps[3]=0x80;
    CHECK(train(&m,caps,1080001,&r)==BC250_DP_TRAIN_UNSUPPORTED && !m.calls);
    // Two lanes of HBR2 carry 360 MHz: the 165 Hz mode is refused before any I/O, 75 Hz trains at HBR2.
    init(&m,caps);caps[1]=0x14;caps[2]=0xc2;
    CHECK(Bc250DpMaxPixelClock(caps)==360000 && Bc250DpFirstRate(caps,645100)==0 && Bc250DpFirstRate(caps,304250)==20);
    CHECK(train(&m,caps,645100,&r)==BC250_DP_TRAIN_UNSUPPORTED && !m.calls);
    init(&m,caps);caps[1]=0x14;caps[2]=0xc2;
    CHECK(train(&m,caps,304250,&r)==BC250_DP_TRAIN_OK && r.Rate==20 && r.Lanes==2);
    // RBR-only sink: RBR, as before.
    init(&m,caps);caps[1]=6;
    CHECK(Bc250DpFirstRate(caps,154000)==6 && train(&m,caps,154000,&r)==BC250_DP_TRAIN_OK && r.Rate==6);
    CHECK(Bc250DpTrain(NULL,caps,1,&r)==BC250_DP_TRAIN_ARGUMENT);
    printf("DP training: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
