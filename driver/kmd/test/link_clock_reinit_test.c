/* Offline command footprint. Fake register file only. Never connects to a device. */
#define _CRT_SECURE_NO_WARNINGS
#include "bc250_atom.h"
#include "bc250_link_atom.h"
#include "bc250_link_regs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regs.generated.h"
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
    unsigned reads, writes, lastRead;
} TRACE;
static BC250_LINK_ATOM* active_link;
static int old_scope;
static int Owned(void *p) { (void)p; return 1; }
static int Allowed(void *p, uint32_t reg, int write)
{ (void)p;
    if (!old_scope && Bc250LinkAtomScopedRegister(active_link,reg,write)) return 1;
    if(!Bc250LinkRegisterAllowed(reg,write)) {printf("DENIED dword=%08x write=%d\n",reg,write);return 0;}return reg<262144; }
static int Read(void *p, uint32_t reg, uint32_t *v)
{
    TRACE *t=p;
    t->touched[reg]|=1; ++t->reads; t->lastRead=reg; *v=t->registers[reg];
    /* Explicit ideal status assumptions for path enumeration, NOT hardware proof. */
    if(reg==0x5e00) *v |= 0x10000000u;
    if(reg==0x16061) *v = 0x80000000u;
    if(reg==0x5df1) *v &= ~0x4000u; /* branch demands SRAMCLK_CLOCK_ON clear */

    if(reg==MODEL_RDPCSTX0_RDPCSTX_PHY_CNTL12 &&
       (t->registers[MODEL_RDPCSTX0_RDPCSTX_PHY_CNTL5]&RDPCSTX0_RDPCSTX_PHY_CNTL5__RDPCS_PHY_DP_TX0_MPLL_EN_MASK))
        *v |= RDPCSTX0_RDPCSTX_PHY_CNTL12__RDPCS_PHY_DP_MPLLB_STATE_MASK;
    return 0;
}
static int Write(void *p, uint32_t reg, uint32_t v)
{
    TRACE *t=p;
    t->touched[reg]|=2; ++t->writes;
    printf("write dword=%08x before=%08x after=%08x\n",reg,t->registers[reg],v);
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
    t->registers[reg]=v;
    return 0;
}
static void Seed(TRACE *t,uint32_t fill,uint32_t phy0)
{
    unsigned i;
    for(i=0;i<262144;++i)t->registers[i]=fill;
    t->registers[BC250_REG_DMU_DIG0_DIG_BE_CNTL/4]=0x00020100;
    t->registers[BC250_REG_DMU_DIG0_DIG_BE_EN_CNTL/4]=0;
    t->registers[BC250_REG_DMU_DIG0_DIG_FE_CNTL/4]=0x01000000;
    t->registers[MODEL_RDPCSTX0_RDPCSTX_PHY_CNTL0]=phy0;
    t->registers[BC250_REG_DMU_OTG0_OTG_CONTROL/4]=0x300; /* caller has disabled OTG */
}
static uint64_t Now(void *p) { return ++((TRACE*)p)->ticks; }
static int Delay(void *p,uint32_t us) { ((TRACE*)p)->ticks+=us; return 0; }
int main(int argc,char **argv)
{
    FILE *file;size_t length;unsigned mode,khz,root;
    uint8_t *bytes=malloc(BC250_ATOM_MAX_IMAGE+1u);
    TRACE *trace=calloc(1,sizeof(*trace));
    BC250_ATOM_PLAN *plan=calloc(1,sizeof(*plan));
    void *workspace=calloc(1,Bc250AtomContextSize());
    BC250_ATOM_IMAGE image;BC250_ATOM_STATUS status;
    BC250_LINK_ATOM link={0};
    if((argc!=6 && argc!=7) || !bytes || !trace || !plan || !workspace)return 2;
    old_scope=argc==7;active_link=&link;
    mode=(unsigned)strtoul(argv[2],NULL,0);root=(unsigned)strtoul(argv[4],NULL,0);khz=(unsigned)strtoul(argv[5],NULL,0);
    file=fopen(argv[1],"rb");if(!file)return 3;
    length=fread(bytes,1,BC250_ATOM_MAX_IMAGE+1u,file);if(ferror(file))return 4;fclose(file);
    status=Bc250AtomImageOpen(bytes,length,0x1002,0x13fe,&image);if(status)return 5;
    link.Image=&image;link.Plan=plan;link.Workspace=workspace;link.WorkspaceSize=Bc250AtomContextSize();
    link.Io.Context=trace;link.Io.Owned=Owned;link.Io.Allowed=Allowed;link.Io.Read=Read;link.Io.Write=Write;link.Io.NowUs=Now;link.Io.DelayUs=Delay;
    status=Bc250LinkAtomValidate(&link);if(status)return 6;
    Seed(trace,(uint32_t)strtoul(argv[3],NULL,0),0x100);
    printf("MOCK ONLY mode%u root%u clock%u seed%s\n",mode,root,khz,argv[3]);
    if(root==112){ unsigned cycle; for(cycle=0;cycle<4;++cycle){const unsigned frequencies[]={174500,148500,74250,174500};unsigned f=frequencies[cycle];status=Bc250LinkAtomClock(&link,mode,f);printf("REPEAT %u khz%u status%u table%u offset%u\n",cycle,f,status,link.Last.Table,link.Last.Offset);if(status)break;} }
    else if(root==76){
        /* BD-195: TX ENABLE powers the PHY up through table 77 from both states the
         * lab showed after a re-plug: HDMI mode left behind (0x100) and PHY in reset (0x1). */
        const uint32_t phy0[]={0x100,0x1};unsigned s;
        for(s=0;s<2;++s){
            unsigned writes=trace->writes;
            Seed(trace,(uint32_t)strtoul(argv[3],NULL,0),phy0[s]);
            status=Bc250LinkAtomTransmitter(&link,BC250_LINK_TX_ON,mode,khz,4,0,0);
            printf("TX_ON phy0=%x status%u table%u offset%u instructions%u writes%u\n",phy0[s],status,
                link.Last.Table,link.Last.Offset,link.Last.Instructions,trace->writes-writes);
            if(status)break;
            /* The power-up branch is long: 46 writes before PLL_UPDATE alone. */
            if(trace->writes-writes<100){printf("FAIL CHECK power-up branch not taken\n");status=BC250_ATOM_IO;break;}
        }
    }
    else if(root==0){
        /* GitHub #45: TX DISABLE of a DP link that the firmware trained, at its own symbol clock, on two and four
         * lanes. DISABLE does not take the power-up branch and stays inside the global register list. */
        const unsigned lanes[]={2,4};unsigned s;
        for(s=0;s<2;++s){
            unsigned writes=trace->writes;
            /* The back end the firmware left enabled; with it off the table returns at once. */
            Seed(trace,(uint32_t)strtoul(argv[3],NULL,0),0x100);
            trace->registers[BC250_REG_DMU_DIG0_DIG_BE_EN_CNTL/4]=1;
            status=Bc250LinkAtomTransmitter(&link,BC250_LINK_TX_OFF,mode,khz,lanes[s],0,0);
            printf("TX_OFF lanes%u status%u table%u offset%u instructions%u writes%u\n",lanes[s],status,
                link.Last.Table,link.Last.Offset,link.Last.Instructions,trace->writes-writes);
            if(status)break;
            if(trace->writes==writes){printf("FAIL CHECK disable branch not taken\n");status=BC250_ATOM_IO;break;}
        }
    }
    else status=root==12?Bc250LinkAtomClock(&link,mode,khz):root==4?Bc250LinkAtomStream(&link,mode,khz,4,mode==0?10:0):Bc250LinkAtomTransmitter(&link,BC250_LINK_TX_ON,mode,mode==0?270000:khz,4,0,0);
    printf("status=%u instructions=%u reads=%u writes=%u table=%u offset=%u\n",status,link.Last.Instructions,link.Last.Reads,link.Last.Writes,link.Last.Table,link.Last.Offset);
    printf("last read dword=%08x value=%08x\n",trace->lastRead,trace->registers[trace->lastRead]);
    if(status || link.ClockScope)printf("FAIL CHECK complete clock tree and scope cleanup\n");
    free(workspace);free(plan);free(trace);free(bytes);return status || link.ClockScope?1:0;
}