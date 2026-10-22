// PROVENANCE: AMD DC (MIT), Linux 7d0a66e4; exact DCN201 hooks in pipe_*.inc.
#include "bc250_dcn201_pipe.h"
#include "bc250_atom_types.h"
#include "dcn_2_0_1_sh_mask.h"
#include "dml201/reference/pipe/clk_mask.h"
#include <stdarg.h>
#include <string.h>
#if defined(BC250_SHIM_KERNEL)
typedef int int32_t;
#endif
#ifndef true
#define true 1
#define false 0
#endif
#include "pipe_registers.inc"
#include "pipe_layouts.inc"

struct access { BC250_DISP_IO* io; long status; unsigned validate, hubp; };
struct hubp { unsigned mpcc_id,opp_id; };
struct dcn10_hubp { struct hubp base; struct access* a; };
#define dcn20_hubp dcn10_hubp
#define dcn201_hubp dcn10_hubp
#define TO_DCN10_HUBP(p) ((struct dcn10_hubp*)(p))
#define TO_DCN20_HUBP(p) TO_DCN10_HUBP(p)
#define TO_DCN201_HUBP(p) TO_DCN10_HUBP(p)
#define OPP_ID_INVALID 0xf
struct dccg;
struct stream_encoder;
struct dccg_funcs {
    void (*set_fifo_errdet_ovr_en)(struct dccg*,bool);
    void (*otg_add_pixel)(struct dccg*,uint32_t);
    void (*otg_drop_pixel)(struct dccg*,uint32_t);
};
struct dccg { const struct dccg_funcs* funcs; };
struct dcn_dccg { struct dccg base; struct access* a; };
#define TO_DCN_DCCG(p) ((struct dcn_dccg*)(p))
struct encoder_funcs { uint32_t (*get_fifo_cal_average_level)(struct stream_encoder*); };
struct stream_encoder { const struct encoder_funcs* funcs; };
struct dcn10_stream_encoder { struct stream_encoder base;struct access* a; };
#define DCN10STRENC_FROM_STRENC(p) ((struct dcn10_stream_encoder*)(p))
struct timing_generator { uint32_t inst; };
struct pipe_ctx { struct { struct stream_encoder* stream_enc;struct timing_generator* tg; } stream_res; };
struct dc_state { struct { struct pipe_ctx pipe_ctx[1]; } res_ctx; };
struct resource_pool { uint32_t pipe_count;struct dccg* dccg; };
struct dc { struct resource_pool* res_pool;struct { bool disable_stutter,force_chroma_subsampling_1tap; } debug; };
struct dc_context { struct dc* dc; };
struct output_pixel_processor { struct dc_context* ctx; };
struct dcn10_opp { struct output_pixel_processor base;struct access* a; };
#define dcn20_opp dcn10_opp
#define TO_DCN10_OPP(p) ((struct dcn10_opp*)(p))
#define TO_DCN20_OPP(p) TO_DCN10_OPP(p)
struct mpc { unsigned unused; };
struct dcn201_mpc { struct mpc base;struct access* a; };
#define TO_DCN201_MPC(p) ((struct dcn201_mpc*)(p))
struct mpc_dwb_flow_control { unsigned flow_ctrl_mode,flow_ctrl_cnt0,flow_ctrl_cnt1; };
struct link_encoder { unsigned unused; };
struct dcn10_link_encoder { struct link_encoder base;struct access* a; };
#define TO_DCN10_LINK_ENC(p) ((struct dcn10_link_encoder*)(p))
struct clk_mgr_internal {
    struct { struct { int dispclk_khz,dppclk_khz; } clks;
        int dentist_vco_freq_khz;struct dc_context* ctx; } base;
    struct access* a;
};
struct watermark { uint32_t urgent_ns,pte_meta_urgent_ns;
    struct { uint32_t pstate_change_ns; } cstate_pstate; };
#pragma warning(push)
#pragma warning(disable: 4201) // AMD watermark union exposes named sets and an array.
union dcn_watermark_set { struct { struct watermark a,b,c,d; };struct watermark sets[4]; };
#pragma warning(pop)
struct hubbub { struct dc_context* ctx; };
struct dcn10_hubbub { struct hubbub base;struct access* a;union dcn_watermark_set watermarks; };
#define dcn20_hubbub dcn10_hubbub
#define TO_DCN10_HUBBUB(p) ((struct dcn10_hubbub*)(p))
#define TO_DCN20_HUBBUB(p) TO_DCN10_HUBBUB(p)
#define OTG_MASTER 1
static bool resource_is_pipe_type(const struct pipe_ctx* p,int type)
{ return p && p->stream_res.stream_enc && type==OTG_MASTER; }

static uint32_t selectedReg(const struct access* a,uint32_t reg)
{
    if(a->hubp==1) {
        switch(reg) {
#include "pipe_hubp_remap.inc"
        default:break; // Global/output state always remains on OTG/OPP0.
        }
    }
    return reg;
}
static uint32_t readReg(struct access* a,uint32_t reg)
{
    unsigned long value=0;
    if(a->status>=0 && !a->validate) {
        if(!reg)a->status=BC250_DISP_STATUS_INVALID;
        else a->status=Bc250DispRead(a->io,selectedReg(a,reg),&value);
        if(a->status>=0 && value==0xfffffffful)a->status=BC250_DISP_STATUS_DEVICE;
    }
    return (uint32_t)value;
}
static void writeReg(struct access* a,uint32_t reg,uint32_t value)
{
    if(a->status<0)return;
    if(!reg) { if(value)a->status=BC250_DISP_STATUS_INVALID;return; }
    if(!a->validate)a->status=Bc250DispWrite(a->io,selectedReg(a,reg),value);
}
static void fields(struct access* a,uint32_t reg,int fresh,unsigned count,...)
{
    uint32_t value=fresh?0:readReg(a,reg);unsigned i;va_list ap;
    va_start(ap,count);
    for(i=0;i<count;++i) {
        uint32_t mask=va_arg(ap,uint32_t),x=va_arg(ap,uint32_t);
        if(x>Bc250DispGet(mask,mask) && a->status>=0)a->status=BC250_DISP_STATUS_INVALID;
        value=(value&~mask)|(uint32_t)Bc250DispField(mask,x);
    }
    va_end(ap);writeReg(a,reg,value);
}
static void waitReg(struct access* a,uint32_t reg,uint32_t mask,unsigned want,unsigned delay,unsigned count)
{
    unsigned i;
    for(i=0;i<=count && a->status>=0;++i) {
        uint32_t value=readReg(a,reg);
        if(a->status<0 || Bc250DispGet(value,mask)==want)return;
        if(i!=count)a->status=Bc250DispStall(a->io,delay);
    }
    if(a->status>=0)a->status=BC250_DISP_STATUS_TIMEOUT;
}
#define REG(r) PIPE_REG_##r
#define MASK(r,f) PIPE_MASK_##r##_##f
#define PAIR(r,f,v) (uint32_t)MASK(r,f),(uint32_t)(v)
#define REG_READ(r) readReg(CTX,REG(r))
#define REG_WRITE(r,v) writeReg(CTX,REG(r),(uint32_t)(v))
#define REG_GET(r,f,p) (*(p)=(uint32_t)Bc250DispGet(readReg(CTX,REG(r)),MASK(r,f)))
#define REG_UPDATE(r,f,v) fields(CTX,REG(r),0,1,PAIR(r,f,v))
#define REG_UPDATE_2(r,f,v,g,w) fields(CTX,REG(r),0,2,PAIR(r,f,v),PAIR(r,g,w))
#define REG_UPDATE_3(r,f,v,g,w,h,x) fields(CTX,REG(r),0,3,PAIR(r,f,v),PAIR(r,g,w),PAIR(r,h,x))
#define REG_UPDATE_4(r,f,v,g,w,h,x,j,y) fields(CTX,REG(r),0,4,PAIR(r,f,v),PAIR(r,g,w),PAIR(r,h,x),PAIR(r,j,y))
#define REG_UPDATE_6(r,f,v,g,w,h,x,j,y,k,z,l,u) fields(CTX,REG(r),0,6,PAIR(r,f,v),PAIR(r,g,w),PAIR(r,h,x),PAIR(r,j,y),PAIR(r,k,z),PAIR(r,l,u))
#define REG_SET(r,init,f,v) fields(CTX,REG(r),1,1,PAIR(r,f,v))
#define REG_SET_2(r,init,f,v,g,w) fields(CTX,REG(r),1,2,PAIR(r,f,v),PAIR(r,g,w))
#define REG_SET_3(r,init,f,v,g,w,h,x) fields(CTX,REG(r),1,3,PAIR(r,f,v),PAIR(r,g,w),PAIR(r,h,x))
#define REG_SET_4(r,init,f,v,g,w,h,x,j,y) fields(CTX,REG(r),1,4,PAIR(r,f,v),PAIR(r,g,w),PAIR(r,h,x),PAIR(r,j,y))
#define REG_SET_5(r,init,f,v,g,w,h,x,j,y,k,z) fields(CTX,REG(r),1,5,PAIR(r,f,v),PAIR(r,g,w),PAIR(r,h,x),PAIR(r,j,y),PAIR(r,k,z))
#define REG_WAIT(r,f,w,d,n) waitReg(CTX,REG(r),MASK(r,f),w,d,n)
#define DC_LOG_BANDWIDTH_CALCS(...) ((void)0)
#pragma warning(push)
#pragma warning(disable: 4100 4127 4018 4244) // unchanged AMD helpers, shared OTG0
#define CTX hubp1->a
#include "pipe_hubp1.inc"
#undef CTX
#define CTX hubp2->a
#include "pipe_hubp2.inc"
#undef CTX
#define CTX hubp201->a
#include "pipe_hubp201.inc"
#undef CTX
#define CTX hubbub1->a
#include "pipe_hubbub1.inc"
#include "pipe_hubbub201.inc"
#undef CTX
#define CTX dccg_dcn->a
#include "pipe_dccg.inc"
#undef CTX
#define CTX enc1->a
#include "pipe_encoder.inc"
#include "pipe_encoder_route.inc"
#undef CTX
#define CTX mpc201->a
#include "pipe_mpc201.inc"
#undef CTX
#define CTX oppn10->a
#include "pipe_opp1.inc"
#undef CTX
#define CTX oppn20->a
#define ASSERT(x) do { if(!(x))CTX->status=BC250_DISP_STATUS_INVALID; } while(0)
#include "pipe_opp2.inc"
#undef ASSERT
#undef CTX
#define CTX enc10->a
// Only constant DIGA and the three explicitly admitted signals reach these
// helpers. Their default arms return/refuse and post-write readback is checked.
#define ASSERT_CRITICAL(x) ((void)(x))
#include "pipe_route.inc"
#undef ASSERT_CRITICAL
#undef CTX
#define CTX clk_mgr->a
#include "pipe_did.inc"
#include "pipe_dentist.inc"
#undef CTX
#pragma warning(pop)

static int initialize(struct access* a,BC250_DISP_IO* io)
{
    memset(a,0,sizeof(*a));a->io=io;
    return io && io->Read && io->Write && io->Stall;
}
static uint32_t get(struct access* a,uint32_t reg,uint32_t mask)
{ return (uint32_t)Bc250DispGet(readReg(a,reg),mask); }

long Bc250Dcn201ReadClocks(BC250_DISP_IO* io,unsigned int crystal,BC250_DCN201_CLOCKS* out)
{
    struct access a;BC250_DCN201_CLOCKS c;uint32_t timer,ref,disp,dpp,counter;
    if(out)memset(out,0,sizeof(*out));
    if(!out || !crystal || crystal>120000 || !initialize(&a,io))return BC250_DISP_STATUS_INVALID;
    memset(&c,0,sizeof(c));c.CrystalClockKhz=crystal;
    ref=readReg(&a,REG(REFCLK_CNTL));timer=readReg(&a,REG(DCHUBBUB_GLOBAL_TIMER_CNTL));
    if(a.status<0)return a.status;
    if(Bc250DispGet(ref,MASK(REFCLK_CNTL,REFCLK_CLOCK_EN)) ||
       !Bc250DispGet(timer,MASK(DCHUBBUB_GLOBAL_TIMER_CNTL,DCHUBBUB_GLOBAL_TIMER_ENABLE)))return BC250_DISP_STATUS_NOT_SUPPORTED;
    c.DchubRefClockKhz=crystal;
    if(Bc250DispGet(timer,MASK(DCHUBBUB_GLOBAL_TIMER_CNTL,DCHUBBUB_GLOBAL_TIMER_REFDIV))==2)c.DchubRefClockKhz/=2;
    c.DentistVcoKhz=get(&a,REG(CLK4_CLK_PLL_REQ),MASK(CLK4_CLK_PLL_REQ,FbMult_int))*100000u;
    counter=get(&a,REG(CLK4_CLK2_CURRENT_CNT),MASK(CLK4_CLK2_CURRENT_CNT,CURRENT_COUNT));
    if(a.status<0)return a.status;
    if(counter>0xffffffffu/100u)return BC250_DISP_STATUS_MISMATCH;
    c.DpRefClockKhz=counter*100u;
    ref=readReg(&a,REG(DENTIST_DISPCLK_CNTL));
    disp=(uint32_t)Bc250DispGet(ref,MASK(DENTIST_DISPCLK_CNTL,DENTIST_DISPCLK_WDIVIDER));
    dpp=(uint32_t)Bc250DispGet(ref,MASK(DENTIST_DISPCLK_CNTL,DENTIST_DPPCLK_WDIVIDER));
    if(a.status<0)return a.status;
    if(c.DchubRefClockKhz<40000 || c.DchubRefClockKhz>60000 ||
       c.DentistVcoKhz<1000000 || c.DentistVcoKhz>6000000 || !c.DpRefClockKhz ||
       disp<DENTIST_BASE_DID_1 || disp>DENTIST_MAX_DID || dpp<DENTIST_BASE_DID_1 || dpp>DENTIST_MAX_DID)return BC250_DISP_STATUS_MISMATCH;
    c.DispClockKhz=4*c.DentistVcoKhz/(uint32_t)dentist_get_divider_from_did((int)disp);
    c.DppClockKhz=4*c.DentistVcoKhz/(uint32_t)dentist_get_divider_from_did((int)dpp);
    *out=c;return BC250_DISP_STATUS_SUCCESS;
}

long Bc250Dcn201PlaneBlankAt(BC250_DISP_IO* io,unsigned pipe,int blank)
{
    struct access a;struct dcn10_hubp h;uint32_t value;
    if(!initialize(&a,io) || pipe>1 || (blank!=0 && blank!=1))return BC250_DISP_STATUS_INVALID;
    a.hubp=pipe;
    memset(&h,0,sizeof(h));h.a=&a;hubp1_set_blank(&h.base,(bool)blank);
    value=readReg(&a,REG(DCHUBP_CNTL));
    if(a.status<0)return a.status;
    if(Bc250DispGet(value,MASK(DCHUBP_CNTL,HUBP_BLANK_EN))!=(unsigned)blank ||
       Bc250DispGet(value,MASK(DCHUBP_CNTL,HUBP_TTU_DISABLE))!=(unsigned)blank ||
       (blank && !Bc250DispGet(value,MASK(DCHUBP_CNTL,HUBP_NO_OUTSTANDING_REQ))))return BC250_DISP_STATUS_MISMATCH;
    return a.status;
}

long Bc250Dcn201PlaneBlank(BC250_DISP_IO* io,int blank)
{ return Bc250Dcn201PlaneBlankAt(io,0,blank); }

static void programRequestor(struct access* a,const BC250_DML201_PLAN* p)
{
    struct dcn10_hubp h;
    memset(&h,0,sizeof(h));h.a=a;
    // AMD's helpers take mutable pointers but only read these plan fields.
    hubp1_vtg_sel(&h.base,0);
    hubp201_setup(&h.base,(display_dlg_regs_st*)&p->Dlg,(display_ttu_regs_st*)&p->Ttu,
        (display_rq_regs_st*)&p->Rq,(display_pipe_dest_params_st*)&p->Destination);
    hubp2_setup_interdependent(&h.base,(display_dlg_regs_st*)&p->Dlg,(display_ttu_regs_st*)&p->Ttu);
}
static void programWatermarks(struct access* a,const BC250_DML201_PLAN* p,const BC250_DCN201_CLOCKS* clocks)
{
    struct dcn10_hubbub b;union dcn_watermark_set wm;
    struct dc dc;struct dc_context ctx;unsigned i;
    memset(&b,0,sizeof(b));memset(&dc,0,sizeof(dc));memset(&wm,0,sizeof(wm));
    ctx.dc=&dc;b.base.ctx=&ctx;b.a=a;
    // This plan was computed without cstate. Do not enable an unplanned state.
    dc.debug.disable_stutter=true;
    for(i=0;i<4;++i) {
        wm.sets[i].urgent_ns=p->Watermarks[i].UrgentNs;
        wm.sets[i].pte_meta_urgent_ns=p->Watermarks[i].MemoryTripNs;
        wm.sets[i].cstate_pstate.pstate_change_ns=p->Watermarks[i].DramClockChangeNs;
    }
    (void)hubbub201_program_watermarks(&b.base,&wm,clocks->DchubRefClockKhz/1000u,true);
}
static void programPlane(struct access* a,const BC250_DML201_PLAN* p,const BC250_DCN201_CLOCKS* clocks)
{
    programRequestor(a,p);programWatermarks(a,p,clocks);
}

static long validatePlane(struct access* a,const BC250_DML201_PLAN* p,const BC250_DCN201_CLOCKS* clocks)
{
    unsigned i;
    if(!p || !clocks || p->Valid!=1 || p->Assertions || p->CStateEnabled ||
       !p->DispClockKhz || p->DispClockKhz>1200000 || !p->DppClockKhz || p->DppClockKhz>1200000 ||
       !p->Destination.htotal || p->Destination.htotal>16384 ||
       p->VStartup!=p->Destination.vstartup_start || p->VReady!=p->Destination.vready_offset ||
       p->VUpdateOffset!=p->Destination.vupdate_offset || p->VUpdateWidth!=p->Destination.vupdate_width ||
       p->VReady>16384 || p->VUpdateWidth>16384 || p->VUpdateOffset>16384 || p->VStartup>8192 ||
       clocks->DchubRefClockKhz<40000 || clocks->DchubRefClockKhz>60000)return BC250_DISP_STATUS_INVALID;
    for(i=0;i<4;++i) {
        unsigned mhz=clocks->DchubRefClockKhz/1000;
        if(p->Watermarks[i].UrgentNs>0xffffffffu/mhz || p->Watermarks[i].MemoryTripNs>0xffffffffu/mhz ||
           p->Watermarks[i].DramClockChangeNs>0xffffffffu/mhz)
            return BC250_DISP_STATUS_INVALID;
    }
    // Run precisely the same AMD setters in validation mode before any MMIO.
    // Every shifted field is range checked; absent DCN201 cursor1 accepts zero.
    a->validate=1;programPlane(a,p,clocks);a->validate=0;return a->status;
}

long Bc250Dcn201PipeApply(BC250_DISP_IO* io,const BC250_DML201_PLAN* p,const BC250_DCN201_CLOCKS* clocks)
{
    struct access a;BC250_DCN201_CLOCKS fresh;uint32_t ctl,override;
    struct clk_mgr_internal clk;struct dc_state state;struct dc dc;struct dc_context ctx;
    struct resource_pool pool;struct dcn_dccg dccg;struct dcn10_stream_encoder encoder;
    struct timing_generator tg;long cleanup;
    static const struct dccg_funcs df={dccg2_set_fifo_errdet_ovr_en,dccg2_otg_add_pixel,dccg2_otg_drop_pixel};
    static const struct encoder_funcs ef={enc2_get_fifo_cal_average_level};
    if(!initialize(&a,io))return BC250_DISP_STATUS_INVALID;
    a.status=validatePlane(&a,p,clocks);if(a.status<0)return a.status;
    a.status=Bc250Dcn201ReadClocks(io,clocks->CrystalClockKhz,&fresh);
    if(a.status<0)return a.status;
    if(memcmp(&fresh,clocks,sizeof(fresh)))return BC250_DISP_STATUS_MISMATCH;
    ctl=readReg(&a,REG(DCHUBP_CNTL));
    if(a.status<0)return a.status;
    if(!Bc250DispGet(ctl,MASK(DCHUBP_CNTL,HUBP_BLANK_EN)) ||
       !Bc250DispGet(ctl,MASK(DCHUBP_CNTL,HUBP_TTU_DISABLE)) ||
       !Bc250DispGet(ctl,MASK(DCHUBP_CNTL,HUBP_NO_OUTSTANDING_REQ)))return BC250_DISP_STATUS_BUSY;
    override=readReg(&a,REG(DISPCLK_FREQ_CHANGE_CNTL));if(a.status<0)return a.status;
    memset(&clk,0,sizeof(clk));memset(&state,0,sizeof(state));memset(&dc,0,sizeof(dc));
    memset(&pool,0,sizeof(pool));memset(&dccg,0,sizeof(dccg));memset(&encoder,0,sizeof(encoder));
    tg.inst=0;dccg.base.funcs=&df;dccg.a=&a;encoder.base.funcs=&ef;encoder.a=&a;
    pool.pipe_count=1;pool.dccg=&dccg.base;dc.res_pool=&pool;ctx.dc=&dc;
    state.res_ctx.pipe_ctx[0].stream_res.stream_enc=&encoder.base;
    state.res_ctx.pipe_ctx[0].stream_res.tg=&tg;
    clk.a=&a;clk.base.ctx=&ctx;clk.base.dentist_vco_freq_khz=(int)clocks->DentistVcoKhz;
    clk.base.clks.dispclk_khz=(int)p->DispClockKhz;clk.base.clks.dppclk_khz=(int)p->DppClockKhz;
    dcn20_update_clocks_update_dentist(&clk,&state);
    // Error latching must not suppress cleanup after a failed FIFO workaround.
    cleanup=Bc250DispUpdate(io,REG(DISPCLK_FREQ_CHANGE_CNTL),MASK(DISPCLK_FREQ_CHANGE_CNTL,DCCG_FIFO_ERRDET_OVR_EN),
        override&MASK(DISPCLK_FREQ_CHANGE_CNTL,DCCG_FIFO_ERRDET_OVR_EN));
    if(a.status>=0)a.status=cleanup;
    if(a.status<0)return a.status;
    a.status=Bc250Dcn201ReadClocks(io,clocks->CrystalClockKhz,&fresh);
    if(a.status<0)return a.status;
    if(fresh.DispClockKhz<p->DispClockKhz || fresh.DppClockKhz<p->DppClockKhz)return BC250_DISP_STATUS_MISMATCH;
    programPlane(&a,p,clocks);return a.status;
}

static int sameOutput(const display_pipe_dest_params_st* x,const display_pipe_dest_params_st* y)
{
    display_pipe_dest_params_st a=*x,b=*y;
    /* All remaining bytes describe shared output timing. Plans are private,
     * zero-initialized DML results. No floating arithmetic in this writer. */
    a.recout_width=b.recout_width=0;a.recout_height=b.recout_height=0;
    a.full_recout_width=b.full_recout_width=0;a.full_recout_height=b.full_recout_height=0;
    a.vstartup_start=b.vstartup_start=0;a.vupdate_offset=b.vupdate_offset=0;
    a.vupdate_width=b.vupdate_width=0;a.vready_offset=b.vready_offset=0;
    return !memcmp(&a,&b,sizeof(a));
}

long Bc250Dcn201GroupProgramBlanked(BC250_DISP_IO* io,const BC250_DML201_GROUP_PLAN* g,
    const unsigned* pipes,const BC250_DCN201_CLOCKS* clocks)
{
    struct access a;BC250_DCN201_CLOCKS fresh;unsigned n;uint32_t ctl;
    const BC250_DML201_PLAN* first;
    if(!initialize(&a,io) || !g || !pipes || !clocks || g->Valid!=1 ||
       g->Assertions || g->Count<1 || g->Count>2 || pipes[0]>1 ||
       (g->Count==2 && (pipes[1]>1 || pipes[0]==pipes[1])))return BC250_DISP_STATUS_INVALID;
    first=&g->Plane[0];
    for(n=0;n<g->Count;++n) {
        const BC250_DML201_PLAN* p=&g->Plane[n];
        a.hubp=pipes[n];a.status=validatePlane(&a,p,clocks);if(a.status<0)return a.status;
        if(p->Destination.otg_inst || p->Destination.odm_combine || p->Destination.interlaced ||
           !sameOutput(&first->Destination,&p->Destination) ||
           p->DispClockKhz!=first->DispClockKhz || p->VoltageLevel!=first->VoltageLevel ||
           p->DcfClockKhz!=first->DcfClockKhz || p->SocClockKhz!=first->SocClockKhz ||
           p->DcfDeepSleepKhz!=first->DcfDeepSleepKhz || p->FabricClockKhz!=first->FabricClockKhz ||
           p->DramClockKhz!=first->DramClockKhz || p->PStateSupported!=first->PStateSupported ||
           memcmp(p->Watermarks,first->Watermarks,sizeof(p->Watermarks)))return BC250_DISP_STATUS_INVALID;
    }
    a.status=Bc250Dcn201ReadClocks(io,clocks->CrystalClockKhz,&fresh);if(a.status<0)return a.status;
    if(memcmp(&fresh,clocks,sizeof(fresh)))return BC250_DISP_STATUS_MISMATCH;
    for(n=0;n<g->Count;++n)
        if(fresh.DispClockKhz<g->Plane[n].DispClockKhz || fresh.DppClockKhz<g->Plane[n].DppClockKhz)
            return BC250_DISP_STATUS_NOT_SUPPORTED;
    // Shared watermarks can change. BOTH physical consumers must be quiet,
    // even when this new group contains only one enabled plane.
    for(n=0;n<2;++n) {
        a.hubp=n;ctl=readReg(&a,REG(DCHUBP_CNTL));if(a.status<0)return a.status;
        if(!Bc250DispGet(ctl,MASK(DCHUBP_CNTL,HUBP_BLANK_EN)) ||
           !Bc250DispGet(ctl,MASK(DCHUBP_CNTL,HUBP_TTU_DISABLE)) ||
           !Bc250DispGet(ctl,MASK(DCHUBP_CNTL,HUBP_NO_OUTSTANDING_REQ)))return BC250_DISP_STATUS_BUSY;
    }
    for(n=0;n<g->Count;++n) { a.hubp=pipes[n];programRequestor(&a,&g->Plane[n]); }
    a.hubp=0;programWatermarks(&a,first,clocks);
    return a.status;
}

static void outputPattern(struct access* a,unsigned width,unsigned height,int blank)
{
    struct dcn10_opp opp;struct tg_color black={0};struct dc dc;struct dc_context ctx;
    memset(&opp,0,sizeof(opp));memset(&dc,0,sizeof(dc));ctx.dc=&dc;opp.base.ctx=&ctx;opp.a=a;
    opp2_set_disp_pattern_generator(&opp.base,blank?CONTROLLER_DP_TEST_PATTERN_SOLID_COLOR:CONTROLLER_DP_TEST_PATTERN_VIDEOMODE,
        CONTROLLER_DP_COLOR_SPACE_RGB,COLOR_DEPTH_888,&black,(int)width,(int)height,0);
}
static void outputProgram(struct access* a,const BC250_DML201_PLAN* p)
{
    struct dcn201_mpc mpc;struct mpc_dwb_flow_control flow;
    struct dcn10_opp opp;struct dc dc;struct dc_context ctx;
    memset(&mpc,0,sizeof(mpc));mpc.a=a;
    // dcn20_enable_stream_timing: one progressive RGB8 OPP, no border/ODM.
    flow.flow_ctrl_mode=0;flow.flow_ctrl_cnt0=0x80;
    flow.flow_ctrl_cnt1=p->Destination.htotal-p->Destination.hactive;
    mpc201_set_out_rate_control(&mpc.base,0,true,false,&flow);
    memset(&opp,0,sizeof(opp));memset(&dc,0,sizeof(dc));ctx.dc=&dc;opp.base.ctx=&ctx;opp.a=a;
    opp1_pipe_clock_control(&opp.base,true);
    opp2_program_left_edge_extra_pixel(&opp.base,PIXEL_ENCODING_RGB,true);
    outputPattern(a,p->Destination.hactive,p->Destination.vactive,1);
}
long Bc250Dcn201OutputProgram(BC250_DISP_IO* io,const BC250_DML201_PLAN* p)
{
    struct access a;uint32_t ctl;
    if(!initialize(&a,io) || !p || p->Valid!=1 || p->Assertions || !p->Destination.hactive ||
       !p->Destination.vactive || p->Destination.hactive>=p->Destination.htotal ||
       p->Destination.htotal>16384 || p->Destination.vactive>8192)return BC250_DISP_STATUS_INVALID;
    a.validate=1;outputProgram(&a,p);a.validate=0;if(a.status<0)return a.status;
    ctl=readReg(&a,REG(DCHUBP_CNTL));if(a.status<0)return a.status;
    if(!Bc250DispGet(ctl,MASK(DCHUBP_CNTL,HUBP_BLANK_EN)) ||
       !Bc250DispGet(ctl,MASK(DCHUBP_CNTL,HUBP_TTU_DISABLE)) ||
       !Bc250DispGet(ctl,MASK(DCHUBP_CNTL,HUBP_NO_OUTSTANDING_REQ)))return BC250_DISP_STATUS_BUSY;
    outputProgram(&a,p);return a.status;
}
long Bc250Dcn201OutputBlank(BC250_DISP_IO* io,unsigned width,unsigned height,int blank)
{
    struct access a;uint32_t control;
    if(!initialize(&a,io) || !width || width>8192 || !height || height>8192 || (blank!=0 && blank!=1))
        return BC250_DISP_STATUS_INVALID;
    a.validate=1;outputPattern(&a,width,height,blank);a.validate=0;if(a.status<0)return a.status;
    outputPattern(&a,width,height,blank);
    waitReg(&a,REG(DPG_STATUS),MASK(DPG_STATUS,DPG_DOUBLE_BUFFER_PENDING),0,10,5000);
    control=readReg(&a,REG(DPG_CONTROL));if(a.status<0)return a.status;
    if(Bc250DispGet(control,MASK(DPG_CONTROL,DPG_EN))!=(unsigned)blank)return BC250_DISP_STATUS_MISMATCH;
    return a.status;
}
long Bc250Dcn201Route(BC250_DISP_IO* io,unsigned mode,int connect)
{
    struct access a;struct dcn10_link_encoder link;struct dcn10_stream_encoder encoder;
    enum signal_type signal;uint32_t source,control;
    if(!initialize(&a,io) || (mode!=0 && mode!=2 && mode!=3) || (connect!=0 && connect!=1))
        return BC250_DISP_STATUS_INVALID;
    memset(&link,0,sizeof(link));memset(&encoder,0,sizeof(encoder));link.a=&a;encoder.a=&a;
    source=get(&a,REG(DIG_BE_CNTL),MASK(DIG_BE_CNTL,DIG_FE_SOURCE_SELECT));
    if(a.status<0)return a.status;
    if(source && source!=DCN10_DIG_FE_SOURCE_SELECT_DIGA)return BC250_DISP_STATUS_BUSY;
    signal=mode==0?SIGNAL_TYPE_DISPLAY_PORT:(mode==2?SIGNAL_TYPE_DVI_SINGLE_LINK:SIGNAL_TYPE_HDMI_TYPE_A);
    if(connect) {
        enc1_dig_connect_to_otg(&encoder.base,0);
        dcn10_link_encoder_setup(&link.base,signal);
    }
    dcn10_link_encoder_connect_dig_be_to_fe(&link.base,ENGINE_ID_DIGA,(bool)connect);
    control=readReg(&a,REG(DIG_BE_CNTL));source=get(&a,REG(DIG_FE_CNTL),MASK(DIG_FE_CNTL,DIG_SOURCE_SELECT));
    if(a.status<0)return a.status;
    if(Bc250DispGet(control,MASK(DIG_BE_CNTL,DIG_FE_SOURCE_SELECT))!=(unsigned)connect ||
       (connect && (Bc250DispGet(control,MASK(DIG_BE_CNTL,DIG_MODE))!=mode || source)))return BC250_DISP_STATUS_MISMATCH;
    return a.status;
}
