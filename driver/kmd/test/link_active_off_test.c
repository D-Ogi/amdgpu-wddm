/* Offline command footprint. Fake register file only. Never connects to a device. */
#define _CRT_SECURE_NO_WARNINGS
#include "bc250_atom.h"
#include "bc250_link_regs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bc250_link_atom.h"
#include "bc250_hdmi_link.h"
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "link_control_masks.inc"
// Offline model only. AMD dpcs_2_0_3_offset.h, Linux 7d0a66e4; addresses from regcalc.
#define MODEL_RDPCSTX0_RDPCSTX_PHY_CNTL0 0x5e00u /* mmRDPCSTX0_RDPCSTX_PHY_CNTL0, DWORD */
#define MODEL_RDPCSTX0_RDPCSTX_PHY_CNTL3 0x5e03u /* mmRDPCSTX0_RDPCSTX_PHY_CNTL3, DWORD */
#define MODEL_DPCSTX0_DPCSTX_TX_CNTL 0x5de9u /* mmDPCSTX0_DPCSTX_TX_CNTL, DWORD */
#define MODEL_RDPCSTX0_RDPCSTX_PHY_CNTL5 0x5e05u /* mmRDPCSTX0_RDPCSTX_PHY_CNTL5, DWORD */
#define MODEL_RDPCSTX0_RDPCSTX_PHY_CNTL12 0x5e0cu /* mmRDPCSTX0_RDPCSTX_PHY_CNTL12, DWORD */
#define MODEL_DPCSTX0_DPCSTX_TX_CLOCK_CNTL 0x5de8u /* mmDPCSTX0_DPCSTX_TX_CLOCK_CNTL, DWORD */
#define MODEL_RDPCSTX0_RDPCSTX_CLOCK_CNTL 0x5df1u /* mmRDPCSTX0_RDPCSTX_CLOCK_CNTL, DWORD */

#include "dpcs_2_0_3_sh_mask.h"

typedef struct TRACE {
    uint32_t registers[262144];
    uint8_t touched[262144];
    uint64_t ticks;
    unsigned reads, writes, lastRead, oldPolicy, denied, oldClockPolicy, valueRefused, strictValues;
} TRACE;
static int Owned(void *p) { (void)p; return 1; }
static int Allowed(void *p,uint32_t reg,int write)
{
    TRACE *t=p;
    if(write && t->oldClockPolicy && Bc250LinkRegisterWriteNeedsBefore(reg)) {
        t->denied=reg;return 0;
    }
    if(t->oldPolicy && (reg==BC250_REG_DMU_DP0_DP_LINK_CNTL/4u ||
        reg==BC250_REG_DMU_DP0_DP_DPHY_TRAINING_PATTERN_SEL/4u ||
        reg==BC250_REG_DMU_DP0_DP_STEER_FIFO/4u ||
        (write && reg==BC250_REG_DMU_DP0_DP_VID_STREAM_CNTL/4u))) {t->denied=reg;return 0;}
    return reg<262144 && Bc250LinkRegisterAllowed(reg,write);
}
static int Read(void *p, uint32_t reg, uint32_t *v)
{
    TRACE *t=p;
    t->touched[reg]|=1; ++t->reads; t->lastRead=reg; *v=t->registers[reg];
    if(reg==MODEL_RDPCSTX0_RDPCSTX_PHY_CNTL12 &&
       (t->registers[MODEL_RDPCSTX0_RDPCSTX_PHY_CNTL5]&RDPCSTX0_RDPCSTX_PHY_CNTL5__RDPCS_PHY_DP_TX0_MPLL_EN_MASK))
        *v |= RDPCSTX0_RDPCSTX_PHY_CNTL12__RDPCS_PHY_DP_MPLLB_STATE_MASK;
    return 0;
}
static int Write(void *p, uint32_t reg, uint32_t v)
{
    TRACE *t=p;
    const BC250_HDMI_FIELD_POLICY* policy=Bc250HdmiLinkPolicy(reg);
    if (Bc250LinkRegisterWriteNeedsBefore(reg)) {
        uint32_t effective = 0, reason = 0;
        int allowed = t->strictValues ?
            Bc250LinkRegisterWriteValueAllowed(reg,t->registers[reg],v) :
            Bc250LinkRegisterPrepareWrite(reg,t->registers[reg],v,&effective,&reason);
        if (!allowed) {
            printf("REFUSED reg=%x before=%08x requested=%08x reason=%u\n",
                reg,t->registers[reg],v,reason);
            ++t->valueRefused; return 1;
        }
        if (!t->strictValues) v = effective;
    }
    t->touched[reg]|=2; ++t->writes;

    // Model only: ideal hardware acknowledgment follows a request. These are
    // named status fields, not a hardware probe or proof of the PHY sequence.
    if(reg==MODEL_RDPCSTX0_RDPCSTX_PHY_CNTL3) {
#define PAIR(n) do { \
        uint32_t req=RDPCSTX0_RDPCSTX_PHY_CNTL3__RDPCS_PHY_DP_TX##n##_REQ_MASK; \
        uint32_t ack=RDPCSTX0_RDPCSTX_PHY_CNTL3__RDPCS_PHY_DP_TX##n##_ACK_MASK; \
        v=(v&~ack)|((v&req)?ack:0); } while(0)
        PAIR(0);PAIR(1);PAIR(2);PAIR(3);
#undef PAIR
    }
    if(reg==MODEL_RDPCSTX0_RDPCSTX_PHY_CNTL0) {
        uint32_t req=RDPCSTX0_RDPCSTX_PHY_CNTL0__RDPCS_PHY_RTUNE_REQ_MASK;
        uint32_t ack=RDPCSTX0_RDPCSTX_PHY_CNTL0__RDPCS_PHY_RTUNE_ACK_MASK;
        v=(v&~ack)|((v&req)?ack:0);
    }
    if(reg==MODEL_DPCSTX0_DPCSTX_TX_CNTL) v &=
        ~(DPCSTX0_DPCSTX_TX_CNTL__DPCS_TX_PLL_UPDATE_REQ_MASK|DPCSTX0_DPCSTX_TX_CNTL__DPCS_TX_PLL_UPDATE_PENDING_MASK);
    if(reg==MODEL_DPCSTX0_DPCSTX_TX_CLOCK_CNTL) {
        uint32_t en=DPCSTX0_DPCSTX_TX_CLOCK_CNTL__DPCS_SYMCLK_EN_MASK;
        uint32_t on=DPCSTX0_DPCSTX_TX_CLOCK_CNTL__DPCS_SYMCLK_CLOCK_ON_MASK;
        v=(v&~on)|((v&en)?on:0);
    }
    // ATOM's byte-1 attribute at offset 51393 shifts immediate 4 to 0x400:
    // RDPCS_SYMCLK_DIV2_CLOCK_ON, not bit 2. Decode the operand alignment too.
    if(reg==MODEL_RDPCSTX0_RDPCSTX_CLOCK_CNTL) {
        uint32_t en=RDPCSTX0_RDPCSTX_CLOCK_CNTL__RDPCS_SYMCLK_DIV2_EN_MASK;
        uint32_t on=RDPCSTX0_RDPCSTX_CLOCK_CNTL__RDPCS_SYMCLK_DIV2_CLOCK_ON_MASK;
        uint32_t sen=RDPCSTX0_RDPCSTX_CLOCK_CNTL__RDPCS_SRAMCLK_EN_MASK;
        uint32_t son=RDPCSTX0_RDPCSTX_CLOCK_CNTL__RDPCS_SRAMCLK_CLOCK_ON_MASK;
        v=(v&~(on|son))|((v&en)?on:0)|((v&sen)?son:0);
    }
    if(policy) {
        // Model command self-clear and stable independently supplied status.
        v=(v&~(policy->Command|policy->Status))|(t->registers[reg]&policy->Status);
        if(reg==BC250_REG_DMU_DIG0_DIG_FE_CNTL/4u)v|=DIG0_DIG_FE_CNTL__DIG_SYMCLK_FE_ON_MASK;
    }
    t->registers[reg]=v;
    return 0;
}
static uint64_t Now(void *p) { return ++((TRACE*)p)->ticks; }
static int Delay(void *p,uint32_t us) { ((TRACE*)p)->ticks+=us; return 0; }

static unsigned checks,failures;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("FAIL line %u: %s\n",__LINE__,#x);}}while(0)
int main(int argc,char **argv)
{
    FILE *f;size_t n;unsigned mode,old,active;
    uint8_t *bytes=malloc(BC250_ATOM_MAX_IMAGE+1u);
    TRACE *t=calloc(1,sizeof(*t));BC250_ATOM_IMAGE image;
    BC250_LINK_ATOM l={0};BC250_ATOM_STATUS status;
    const unsigned modes[]={BC250_LINK_DP,BC250_LINK_DVI,BC250_LINK_HDMI};
    if((argc!=2 && argc!=3) || !bytes || !t)return 2;
    f=fopen(argv[1],"rb");if(!f)return 2;
    n=fread(bytes,1,BC250_ATOM_MAX_IMAGE+1u,f);if(ferror(f)){fclose(f);return 2;}fclose(f);
    if(Bc250AtomImageOpen(bytes,n,0x1002,0x13fe,&image))return 2;
    l.Image=&image;l.Plan=calloc(1,sizeof(*l.Plan));l.Workspace=calloc(1,Bc250AtomContextSize());
    l.WorkspaceSize=Bc250AtomContextSize();
    if(!l.Plan || !l.Workspace)return 2;
    l.Io.Context=t;l.Io.Owned=Owned;l.Io.Allowed=Allowed;l.Io.Read=Read;
    l.Io.Write=Write;l.Io.NowUs=Now;l.Io.DelayUs=Delay;
    CHECK(Bc250LinkAtomValidate(&l)==BC250_ATOM_OK);
    CHECK(Bc250LinkRegisterWriteNeedsBefore(LINK_OTG_CONTROL));
    CHECK(!Bc250LinkRegisterWriteNeedsBefore(MODEL_RDPCSTX0_RDPCSTX_PHY_CNTL0));
    CHECK(Bc250LinkRegisterWriteValueAllowed(LINK_OTG_CONTROL,0x40011300,0x40011300));
    CHECK(!Bc250LinkRegisterWriteValueAllowed(LINK_OTG_CONTROL,0,1));
    CHECK(!Bc250LinkRegisterWriteValueAllowed(LINK_OTG_CONTROL,0,0x100));
    CHECK(!Bc250LinkRegisterWriteValueAllowed(LINK_OTG_CONTROL,0x10000,0));
    CHECK(!Bc250LinkRegisterWriteValueAllowed(LINK_OTG_CONTROL,0x1000,0));
    CHECK(Bc250LinkRegisterWriteValueAllowed(LINK_OTG_COUNT_CONTROL,0x8000001f,0x80000000));
    CHECK(!Bc250LinkRegisterWriteValueAllowed(LINK_OTG_COUNT_CONTROL,0x20,0));
    CHECK(!Bc250LinkRegisterWriteValueAllowed(LINK_OTG_COUNT_CONTROL,0,1));
    CHECK(Bc250LinkRegisterWriteValueAllowed(LINK_VTG_CONTROL,0x80001234,0x1234));
    CHECK(!Bc250LinkRegisterWriteValueAllowed(LINK_VTG_CONTROL,0,0x80000000));
    CHECK(!Bc250LinkRegisterWriteValueAllowed(LINK_VTG_CONTROL,0x1234,0));
    CHECK(!Bc250LinkRegisterWriteValueAllowed(UINT32_MAX,0,0));
    {
        uint32_t effective, reason;
#define PREP(reg,before,request,wanted,why) do { \
        effective=UINT32_MAX;reason=UINT32_MAX; \
        CHECK(Bc250LinkRegisterPrepareWrite(reg,before,request,&effective,&reason)); \
        CHECK(effective==(wanted));CHECK(reason==(why)); } while(0)
#define DENY(reg,before,request,why) do { \
        effective=UINT32_MAX;reason=UINT32_MAX; \
        CHECK(!Bc250LinkRegisterPrepareWrite(reg,before,request,&effective,&reason)); \
        CHECK(effective==0);CHECK(reason==(why)); } while(0)
        PREP(LINK_OTG_CONTROL,0x80000310,0x80000300,0x80000310,1);
        PREP(LINK_OTG_CONTROL,0x80000311,0x80000300,0x80000310,1);
        PREP(LINK_OTG_CONTROL,0x80010310,0x80000300,0x80010310,3);
        PREP(LINK_OTG_CONTROL,0x80000310,0x80010310,0x80000310,2);
        PREP(LINK_OTG_COUNT_CONTROL,0x800000ff,0x80000000,0x800000e0,1);
        PREP(LINK_VTG_CONTROL,0x80001234,0x1234,0x1234,0);
        DENY(LINK_OTG_CONTROL,0x30,0x10,BC250_LINK_WRITE_REFUSED_UNKNOWN);
        DENY(LINK_OTG_CONTROL,0,0x10,BC250_LINK_WRITE_REFUSED_UNKNOWN);
        DENY(LINK_OTG_CONTROL,0x1000,0,BC250_LINK_WRITE_REFUSED_UNKNOWN);
        DENY(LINK_OTG_CONTROL,0,1,BC250_LINK_WRITE_REFUSED_CONTROL);
        DENY(LINK_OTG_CONTROL,0,0x100,BC250_LINK_WRITE_REFUSED_CONTROL);
        DENY(LINK_OTG_COUNT_CONTROL,0,1,BC250_LINK_WRITE_REFUSED_CONTROL);
        DENY(LINK_OTG_COUNT_CONTROL,0xe0,0x20,BC250_LINK_WRITE_REFUSED_UNKNOWN);
        DENY(LINK_VTG_CONTROL,0,0x80000000,BC250_LINK_WRITE_REFUSED_CONTROL);
        DENY(UINT32_MAX,0,0,BC250_LINK_WRITE_REFUSED_ADDRESS);
        CHECK(!Bc250LinkRegisterPrepareWrite(LINK_OTG_CONTROL,0,0,NULL,&reason));
        CHECK(reason==BC250_LINK_WRITE_INVALID_OUTPUT);
        effective=UINT32_MAX;
        CHECK(!Bc250LinkRegisterPrepareWrite(LINK_OTG_CONTROL,0,0,&effective,NULL));
        CHECK(effective==0);
#undef PREP
#undef DENY
    }
    for(mode=0;mode<3;++mode) {
        unsigned cycle;
        const unsigned clocks[]={148500,174500,74250,148500,74250,174500};
        memset(t,0,sizeof(*t));t->oldClockPolicy=argc==3 && !strcmp(argv[2],"old-clock-policy");
        t->strictValues=argc==3 && !strcmp(argv[2],"strict-values");
        t->registers[LINK_OTG_CONTROL]=0x80000310u;
        t->registers[LINK_OTG_COUNT_CONTROL]=0xff;
        t->registers[LINK_VTG_CONTROL]=0x1234;
        for(cycle=0;cycle<sizeof(clocks)/sizeof(clocks[0]);++cycle) {
            BC250_HDMI_LINK hdmi={0};
            status=Bc250LinkAtomClock(&l,modes[mode],clocks[cycle]);
            printf("CLOCK mode=%u cycle=%u khz=%u status=%u reads=%u writes=%u table=%u offset=%u\n",
                modes[mode],cycle,clocks[cycle],status,l.Last.Reads,l.Last.Writes,l.Last.Table,l.Last.Offset);
            CHECK(status==BC250_ATOM_OK);
            CHECK(!t->valueRefused);
            CHECK(t->registers[LINK_OTG_CONTROL]==(0x80000310u));
            CHECK(t->registers[LINK_VTG_CONTROL]==0x1234);
            if(mode && cycle && !status) CHECK(t->registers[LINK_OTG_COUNT_CONTROL]==0xe0);
            if(status)break;
            // Production establishes FE0 -> BE0 route before Stream, and
            // programs HDMI before transmitter enable. Keep that exact order.
            t->registers[BC250_REG_DMU_DIG0_DIG_BE_CNTL/4u]=0x100u|(modes[mode]<<16);
            t->registers[BC250_REG_DMU_DIG0_DIG_FE_CNTL/4u]=DIG0_DIG_FE_CNTL__DIG_SYMCLK_FE_ON_MASK;
            status=Bc250LinkAtomStream(&l,modes[mode],clocks[cycle],4,mode==0?10:0);
            CHECK(status==BC250_ATOM_OK);if(status)break;
            if(modes[mode]==BC250_LINK_HDMI) {
                int programmed=Bc250HdmiStreamProgram(&hdmi,&l.Io,clocks[cycle],1920,1080,
                    clocks[cycle]==174500?2080:2200,clocks[cycle]==174500?1119:1125);
                printf("CHAIN HDMI cycle=%u result=%d stage=%u register=%x operation=%u\n",
                    cycle,programmed,hdmi.StreamStage,hdmi.StreamFailure.Register,hdmi.StreamFailure.Operation);
                CHECK(programmed==0);if(programmed)break;
            }
            status=Bc250LinkAtomTransmitter(&l,BC250_LINK_TX_ON,modes[mode],
                mode==0?270000:clocks[cycle],4,0,0);
            printf("CHAIN TX_ON mode=%u cycle=%u status=%u reg=%x table=%u offset=%u\n",
                modes[mode],cycle,status,l.Last.Register,l.Last.Table,l.Last.Offset);
            CHECK(status==BC250_ATOM_OK);if(status)break;
            if(modes[mode]==BC250_LINK_HDMI)CHECK(Bc250HdmiLinkRestore(&hdmi)==0);
            status=Bc250LinkAtomTransmitter(&l,BC250_LINK_TX_OFF,modes[mode],
                mode==0?270000:clocks[cycle],4,0,0);
            CHECK(status==BC250_ATOM_OK);if(status)break;
        }
    }
    for(mode=0;mode<3;++mode)for(old=0;old<2;++old)for(active=0;active<2;++active) {
        memset(t,0,sizeof(*t));t->oldPolicy=old;
        t->registers[BC250_REG_DMU_DIG0_DIG_BE_EN_CNTL/4u]=active?
            (DIG0_DIG_BE_EN_CNTL__DIG_ENABLE_MASK|DIG0_DIG_BE_EN_CNTL__DIG_SYMCLK_BE_ON_MASK):0;
        t->registers[BC250_REG_DMU_DIG0_DIG_BE_CNTL/4u]=0x00000100u|(modes[mode]<<16);
        t->registers[BC250_REG_DMU_DIG0_DIG_FE_CNTL/4u]=0x01000000u;
        status=Bc250LinkAtomTransmitter(&l,BC250_LINK_TX_OFF,modes[mode],
            mode==0?270000:174500,4,0,0);
        printf("MODEL mode=%u active=%u old_policy=%u status=%u reads=%u writes=%u reg=%x table=%u offset=%u\n",
            modes[mode],active,old,status,l.Last.Reads,l.Last.Writes,l.Last.Register,l.Last.Table,l.Last.Offset);
        if(active && old) {
            CHECK(status==BC250_ATOM_IO);CHECK(l.Last.Writes==(mode==0?0u:6u));
            CHECK(t->denied==(mode==0?BC250_REG_DMU_DP0_DP_VID_STREAM_CNTL/4u:
                BC250_REG_DMU_DP0_DP_LINK_CNTL/4u));
        } else {
            CHECK(status==BC250_ATOM_OK);
            CHECK(l.Last.Writes==(active?(mode==0?11u:8u):0u));
            CHECK(!(t->registers[BC250_REG_DMU_DIG0_DIG_BE_EN_CNTL/4u]&DIG0_DIG_BE_EN_CNTL__DIG_ENABLE_MASK));
        }
    }
    printf("Active TX_OFF: %u checks, %u failures; ideal ACK model only, no hardware validation\n",checks,failures);
    free(l.Workspace);free(l.Plan);free(t);free(bytes);return failures?1:0;
}
