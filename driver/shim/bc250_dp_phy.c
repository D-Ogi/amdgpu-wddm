// PROVENANCE: AMD DC (MIT), dcn10_link_encoder.c via extract_connector.py.
#include "bc250_dp_phy.h"
#include "bc250_atom_types.h"
#include <stdarg.h>
#if defined(BC250_SHIM_KERNEL)
typedef unsigned char bool;
#define true 1
#define false 0
#else
#include <stdbool.h>
#endif
#include "../kmd/regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"

enum { LANE_COUNT_ONE=1 };
struct dc_link_settings { unsigned lane_count; };
struct link_encoder { BC250_DISP_IO *io;long status; };
struct dcn10_link_encoder { struct link_encoder base; };
#define TO_DCN10_LINK_ENC(p) ((struct dcn10_link_encoder *)(p))
#define REG(r) REG_##r
#define REG_DP_DPHY_CNTL BC250_REG_DMU_DP0_DP_DPHY_CNTL
#define REG_DP_DPHY_PRBS_CNTL BC250_REG_DMU_DP0_DP_DPHY_PRBS_CNTL
#define REG_DP_LINK_CNTL BC250_REG_DMU_DP0_DP_LINK_CNTL
#define REG_DP_DPHY_TRAINING_PATTERN_SEL BC250_REG_DMU_DP0_DP_DPHY_TRAINING_PATTERN_SEL
#define REG_DP_LINK_FRAMING_CNTL BC250_REG_DMU_DP0_DP_LINK_FRAMING_CNTL
#define REG_DP_DPHY_SCRAM_CNTL BC250_REG_DMU_DP0_DP_DPHY_SCRAM_CNTL
#define REG_DP_CONFIG BC250_REG_DMU_DP0_DP_CONFIG
// Optional DCE panel register is absent from DCN201. AMD's setup_panel_mode
// explicitly checks REG(...)==0 before accessing it. No fabricated offset.
#define REG_DP_DPHY_INTERNAL_CTRL 0u
#define MASK(r,f) DP0_##r##__##f##_MASK
static uint32_t readReg(struct dcn10_link_encoder *e,uint32_t reg)
{
    unsigned long v=0;
    if(e->base.status>=0)e->base.status=Bc250DispRead(e->base.io,reg,&v);
    return (uint32_t)v;
}
static void writeReg(struct dcn10_link_encoder *e,uint32_t reg,uint32_t value)
{ if(e->base.status>=0)e->base.status=Bc250DispWrite(e->base.io,reg,value); }
static void fields(struct dcn10_link_encoder *e,uint32_t reg,int fresh,unsigned count,...)
{
    uint32_t value=fresh?0:readReg(e,reg);unsigned i;va_list ap;
    va_start(ap,count);
    for(i=0;i<count;++i) {
        uint32_t m=va_arg(ap,uint32_t),v=va_arg(ap,uint32_t);
        value=(value&~m)|(uint32_t)Bc250DispField(m,v);
    }
    va_end(ap);writeReg(e,reg,value);
}
#define PAIR(r,f,v) (uint32_t)MASK(r,f),(uint32_t)(v)
#define REG_READ(r) readReg(enc10,REG(r))
#define REG_WRITE(r,v) writeReg(enc10,REG(r),(v))
#define REG_UPDATE(r,f,v) fields(enc10,REG(r),0,1,PAIR(r,f,v))
#define REG_UPDATE_3(r,f,v,g,w,h,x) fields(enc10,REG(r),0,3,PAIR(r,f,v),PAIR(r,g,w),PAIR(r,h,x))
#define REG_SET(r,init,f,v) fields(enc10,REG(r),1,1,PAIR(r,f,v))
#include "dp_phy_pattern.inc"

long Bc250DpPhyConfigure(BC250_DISP_IO *io,unsigned lanes,int enhanced)
{
    struct dcn10_link_encoder e={{io,0}};struct dc_link_settings settings={lanes};
    if(!io || !io->Read || !io->Write || (lanes!=1 && lanes!=2 && lanes!=4) || (enhanced!=0 && enhanced!=1)) return BC250_DISP_STATUS_INVALID;
    enc1_configure_encoder(&e,&settings);
    if(e.base.status<0)return e.base.status;
    return Bc250DispUpdate(io,REG(DP_LINK_FRAMING_CNTL),MASK(DP_LINK_FRAMING_CNTL,DP_VID_ENHANCED_FRAME_MODE),
        Bc250DispField(MASK(DP_LINK_FRAMING_CNTL,DP_VID_ENHANCED_FRAME_MODE),(unsigned)enhanced));
}
long Bc250DpPhyPattern(BC250_DISP_IO *io,unsigned pattern,int enhanced)
{
    struct dcn10_link_encoder e={{io,0}};
    if(!io || !io->Read || !io->Write || pattern>4 || (enhanced!=0 && enhanced!=1))return BC250_DISP_STATUS_INVALID;
    // TPS1 to TPS4 select index 0 to 3, as dcn10_link_encoder_dp_set_phy_pattern does (BD-139: TPS3/TPS4 for HBR2/HBR3).
    if(pattern) dcn10_link_encoder_set_dp_phy_pattern_training_pattern(&e.base,pattern-1);
    else {
        set_dp_phy_pattern_passthrough_mode(&e,DP_PANEL_MODE_DEFAULT);
        // AMD's passthrough sets enhanced mode unconditionally. A DP 1.0 sink
        // may not advertise it: source framing must match the DPCD lane count.
        if(e.base.status>=0 && !enhanced) e.base.status=Bc250DispUpdate(io,REG(DP_LINK_FRAMING_CNTL),
            MASK(DP_LINK_FRAMING_CNTL,DP_VID_ENHANCED_FRAME_MODE),0);
    }
    return e.base.status;
}
long Bc250DpPhyDisableTraining(BC250_DISP_IO *io)
{
    struct dcn10_link_encoder e={{io,0}};
    if(!io || !io->Read || !io->Write)return BC250_DISP_STATUS_INVALID;
    link_encoder_disable(&e);return e.base.status;
}
