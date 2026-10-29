// Symbol-generator contract and access failure tests, no physical MMIO.
#include <stdio.h>
#include <string.h>
#include "../regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "../../shim/bc250_dp_phy.h"

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; printf("FAIL %u: %s\n",(unsigned)__LINE__,#x); } } while(0)
#define REG(n) BC250_REG_DMU_DP0_##n
#define MASK(r,f) DP0_##r##__##f##_MASK
static const unsigned long offsets[]={REG(DP_DPHY_CNTL),REG(DP_DPHY_PRBS_CNTL),REG(DP_LINK_CNTL),
    REG(DP_DPHY_TRAINING_PATTERN_SEL),REG(DP_LINK_FRAMING_CNTL),REG(DP_DPHY_SCRAM_CNTL),REG(DP_CONFIG)};
enum { NREGS=sizeof(offsets)/sizeof(offsets[0]) };
typedef struct MODEL { unsigned long value[NREGS];unsigned accesses,failAt,postFailure; } MODEL;
static unsigned slot(unsigned long reg)
{
    unsigned i;
    for(i=0;i<NREGS;++i) if(offsets[i]==reg)return i;
    CHECK(0); return NREGS;
}
static long access(MODEL* m)
{
    ++m->accesses;
    if(m->failAt && m->accesses>m->failAt)++m->postFailure;
    return m->accesses==m->failAt ? BC250_DISP_STATUS_DEVICE : 0;
}
static long Read(void* ctx,unsigned long reg,unsigned long* value)
{
    MODEL* m=ctx;unsigned i=slot(reg);long status=access(m);
    if(i==NREGS)return BC250_DISP_STATUS_ACCESS_DENIED;
    if(status<0)return status;
    *value=m->value[i];return 0;
}
static long Write(void* ctx,unsigned long reg,unsigned long value)
{
    MODEL* m=ctx;unsigned i=slot(reg);long status=access(m);
    if(i==NREGS)return BC250_DISP_STATUS_ACCESS_DENIED;
    if(status<0)return status;
    m->value[i]=value;return 0;
}
static void reset(MODEL* m)
{
    unsigned i;memset(m,0,sizeof(*m));
    for(i=0;i<NREGS;++i)m->value[i]=0xa5a5a5a5ul;
}
static unsigned long field(MODEL* m,unsigned long r,unsigned long mask)
{return Bc250DispGet(m->value[slot(r)],mask);}
static long operation(BC250_DISP_IO* io,unsigned op)
{
    switch(op) {
    case 0:return Bc250DpPhyConfigure(io,4,1);
    case 1:return Bc250DpPhyPattern(io,1,1);
    case 2:return Bc250DpPhyPattern(io,2,1);
    case 3:return Bc250DpPhyPattern(io,0,1);
    case 4:return Bc250DpPhyPattern(io,0,0);
    case 5:return Bc250DpPhyPattern(io,3,1);
    case 6:return Bc250DpPhyPattern(io,4,1);
    default:return Bc250DpPhyDisableTraining(io);
    }
}
int main(void)
{
    MODEL m;BC250_DISP_IO io={0};unsigned lanes,pattern,enhanced,op,fail,accesses;
    io.Context=&m;io.Read=Read;io.Write=Write;
    for(lanes=1;lanes<=4;lanes*=2)for(enhanced=0;enhanced<=1;++enhanced) {
        reset(&m);
        CHECK(Bc250DpPhyConfigure(&io,lanes,(int)enhanced)==0);
        CHECK(field(&m,REG(DP_CONFIG),MASK(DP_CONFIG,DP_UDI_LANES))==lanes-1);
        CHECK(field(&m,REG(DP_DPHY_SCRAM_CNTL),MASK(DP_DPHY_SCRAM_CNTL,DPHY_SCRAMBLER_ADVANCE))==1);
        CHECK(field(&m,REG(DP_LINK_FRAMING_CNTL),MASK(DP_LINK_FRAMING_CNTL,DP_VID_ENHANCED_FRAME_MODE))==enhanced);
        for(pattern=1;pattern<=4;++pattern) {
            CHECK(Bc250DpPhyPattern(&io,pattern,(int)enhanced)==0);
            CHECK(field(&m,REG(DP_DPHY_TRAINING_PATTERN_SEL),MASK(DP_DPHY_TRAINING_PATTERN_SEL,DPHY_TRAINING_PATTERN_SEL))==pattern-1);
            CHECK(field(&m,REG(DP_LINK_CNTL),MASK(DP_LINK_CNTL,DP_LINK_TRAINING_COMPLETE))==0);
            CHECK(field(&m,REG(DP_DPHY_CNTL),MASK(DP_DPHY_CNTL,DPHY_BYPASS))==0);
            CHECK(field(&m,REG(DP_DPHY_PRBS_CNTL),MASK(DP_DPHY_PRBS_CNTL,DPHY_PRBS_EN))==0);
        }
        CHECK(Bc250DpPhyPattern(&io,0,(int)enhanced)==0);
        CHECK(field(&m,REG(DP_LINK_CNTL),MASK(DP_LINK_CNTL,DP_LINK_TRAINING_COMPLETE))==1);
        CHECK(field(&m,REG(DP_LINK_FRAMING_CNTL),MASK(DP_LINK_FRAMING_CNTL,DP_VID_ENHANCED_FRAME_MODE))==enhanced);
        CHECK(field(&m,REG(DP_LINK_FRAMING_CNTL),MASK(DP_LINK_FRAMING_CNTL,DP_IDLE_BS_INTERVAL))==0x2000);
        CHECK(field(&m,REG(DP_LINK_FRAMING_CNTL),MASK(DP_LINK_FRAMING_CNTL,DP_VBID_DISABLE))==0);
        CHECK(field(&m,REG(DP_DPHY_SCRAM_CNTL),MASK(DP_DPHY_SCRAM_CNTL,DPHY_SCRAMBLER_BS_COUNT))==0x1ff);
        CHECK(Bc250DpPhyDisableTraining(&io)==0);
        CHECK(field(&m,REG(DP_LINK_CNTL),MASK(DP_LINK_CNTL,DP_LINK_TRAINING_COMPLETE))==0);
    }
    // Every access failure aborts all later writes: never advertise trained after a failed control access.
    for(op=0;op<8;++op) {
        reset(&m);CHECK(operation(&io,op)==0);accesses=m.accesses;
        for(fail=1;fail<=accesses;++fail) {
            reset(&m);m.failAt=fail;
            CHECK(operation(&io,op)==BC250_DISP_STATUS_DEVICE);
            CHECK(m.accesses==fail && m.postFailure==0);
        }
    }
    reset(&m);
    CHECK(Bc250DpPhyConfigure(&io,3,1)==BC250_DISP_STATUS_INVALID);
    CHECK(Bc250DpPhyPattern(&io,5,1)==BC250_DISP_STATUS_INVALID);
    CHECK(Bc250DpPhyConfigure(&io,4,2)==BC250_DISP_STATUS_INVALID);
    CHECK(m.accesses==0 && io.Refusals==0);
    io.Write=NULL;
    CHECK(Bc250DpPhyConfigure(&io,4,1)==BC250_DISP_STATUS_INVALID);
    printf("dp_phy_test: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
