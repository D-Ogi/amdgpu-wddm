// PROVENANCE: AMD DC (MIT), Linux 7d0a66e4, optc1 timing and optc2 enable used by DCN201.
#include "bc250_optc.h"
#include "bc250_dp_train.h"
#include "bc250_atom_types.h"
#include "../kmd/regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include <stdarg.h>
#include <string.h>
#if defined(BC250_SHIM_KERNEL)
typedef int int32_t;
typedef unsigned char bool;
#define true 1
#define false 0
#else
#include <stdbool.h>
#endif
#include "optc_registers.inc"
#include "dp_stream_layouts.inc"

enum signal_type { SIGNAL_TYPE_DISPLAY_PORT, SIGNAL_TYPE_DISPLAY_PORT_MST, SIGNAL_TYPE_EDP, SIGNAL_TYPE_HDMI_TYPE_A };
enum { PIXEL_ENCODING_RGB, PIXEL_ENCODING_YCBCR422, PIXEL_ENCODING_YCBCR420 };
struct dc_crtc_timing {
    uint32_t h_total,h_addressable,h_front_porch,h_sync_width,h_border_left,h_border_right;
    uint32_t v_total,v_addressable,v_front_porch,v_sync_width,v_border_top,v_border_bottom;
    unsigned pixel_encoding;
    struct { unsigned INTERLACE,HSYNC_POSITIVE_POLARITY,VSYNC_POSITIVE_POLARITY; } flags;
};
struct timing_generator;
struct tg_funcs {
    void (*set_vtotal_min_max)(struct timing_generator*,int,int);
    void (*program_global_sync)(struct timing_generator*,int,int,int,int,int);
    void (*set_vtg_params)(struct timing_generator*,const struct dc_crtc_timing*,bool);
    bool (*is_two_pixels_per_container)(const struct dc_crtc_timing*);
};
struct timing_generator { const struct tg_funcs* funcs;unsigned inst; };
struct optc_mask { uint32_t OPTC_DATA_FORMAT,OTG_H_TIMING_DIV_MODE; };
struct optc {
    struct timing_generator base;
    BC250_DISP_IO* io;long status;
    enum signal_type signal;int vready_offset,vstartup_start,vupdate_offset,vupdate_width,pstate_keepout;
    struct dc_crtc_timing orginal_patched_timing;
    const struct optc_mask* tg_mask;unsigned opp_count;
};
#define DCN10TG_FROM_TG(p) ((struct optc*)(p))
#define REG(r) REG_##r
#define MASK(r,f) MASK_##r##_##f
// DCN201's OPTC_COMMON_MASK_SH_LIST_DCN201 does not initialize these optional
// DCN2 multi-pixel fields. AMD tests their zero masks before taking those arms.
#ifndef MASK_OPTC_DATA_FORMAT_CONTROL_OPTC_DATA_FORMAT
#define MASK_OPTC_DATA_FORMAT_CONTROL_OPTC_DATA_FORMAT 0u
#endif
#ifndef MASK_OTG_H_TIMING_CNTL_OTG_H_TIMING_DIV_MODE
#define MASK_OTG_H_TIMING_CNTL_OTG_H_TIMING_DIV_MODE 0u
#endif
static uint32_t readReg(struct optc* o,uint32_t r)
{
    unsigned long v=0;
    if(o->status>=0)o->status=Bc250DispRead(o->io,r,&v);
    return (uint32_t)v;
}
static void fields(struct optc* o,uint32_t reg,int fresh,unsigned count,...)
{
    uint32_t v=fresh?0:readReg(o,reg);unsigned i;va_list ap;
    va_start(ap,count);
    for(i=0;i<count;++i) {
        uint32_t m=va_arg(ap,uint32_t),x=va_arg(ap,uint32_t);
        v=(v&~m)|(uint32_t)Bc250DispField(m,x);
    }
    va_end(ap);
    if(o->status>=0)o->status=Bc250DispWrite(o->io,reg,v);
}
static void waitReg(struct optc* o,uint32_t r,uint32_t mask,unsigned want,unsigned delay,unsigned count)
{
    unsigned i;
    for(i=0;i<=count && o->status>=0;++i) {
        uint32_t v=readReg(o,r);
        if(o->status<0 || Bc250DispGet(v,mask)==want)return;
        if(i!=count)o->status=Bc250DispStall(o->io,delay);
    }
    if(o->status>=0)o->status=BC250_DISP_STATUS_TIMEOUT;
}
#define PAIR(r,f,v) (uint32_t)MASK(r,f),(uint32_t)(v)
#define REG_UPDATE(r,f,v) fields(optc1,REG(r),0,1,PAIR(r,f,v))
#define REG_UPDATE_2(r,f,v,g,w) fields(optc1,REG(r),0,2,PAIR(r,f,v),PAIR(r,g,w))
#define REG_SET(r,init,f,v) fields(optc1,REG(r),1,1,PAIR(r,f,v))
#define REG_SET_2(r,init,f,v,g,w) fields(optc1,REG(r),1,2,PAIR(r,f,v),PAIR(r,g,w))
#define REG_WAIT(r,f,w,d,n) waitReg(optc1,REG(r),MASK(r,f),w,d,n)
#define BREAK_TO_DEBUGGER() do { optc1->status=BC250_DISP_STATUS_INVALID; } while(0)
// No firmware command-buffer offload: our checked writes are synchronous MMIO.
#define REG_SEQ_START() ((void)0)
#define REG_SEQ_SUBMIT() ((void)0)
#define REG_SEQ_WAIT_DONE() ((void)0)
#pragma warning(push)
#pragma warning(disable: 4100 4127 4018) // unchanged AMD code: unused arg, constant optional register, signed compare
#include "optc_timing.inc"
#pragma warning(pop)

static bool singleContainer(const struct dc_crtc_timing* t)
{ (void)t;return false; } // admitted RGB8 only
static const struct tg_funcs funcs={optc1_set_vtotal_min_max,optc1_program_global_sync,optc1_set_vtg_params,singleContainer};
static const struct optc_mask masks={0,0};
static int init(struct optc* o,BC250_DISP_IO* io)
{
    memset(o,0,sizeof(*o));o->io=io;o->base.funcs=&funcs;o->tg_mask=&masks;o->opp_count=1;
    return io && io->Read && io->Write && io->Stall;
}

// BD-177: the same gate as before, term by term, so the caller can name what refused. The terms are OR-ed,
// as they were in the single condition this replaces, so the order below decides only which name a timing that
// breaks several terms gets, never whether it is admitted.
int Bc250OptcTimingReason(const BC250_TARGET_TIMING* t,const BC250_OPTC_SYNC* s)
{
    unsigned long ht,vt;
    if(!t || !s || !t->PixelClockKhz || !t->HActive || !t->VActive ||
       t->HActive>8192 || t->VActive>8192 || t->HBlank>8192 || t->VBlank>8192)return BC250_OPTC_REASON_EXTENT;
    if((t->Flags&0x98)!=0x18)return BC250_OPTC_REASON_FLAGS;
    if(t->HSyncWidth<8 || t->VSyncWidth<1 ||
       t->HBlank<32 || t->VBlank<3 || !t->HSyncOffset || !t->VSyncOffset ||
       t->HSyncOffset>=t->HBlank || t->HSyncWidth>t->HBlank-t->HSyncOffset ||
       t->VSyncOffset>=t->VBlank || t->VSyncWidth>t->VBlank-t->VSyncOffset)return BC250_OPTC_REASON_SYNC;
    ht=t->HActive+t->HBlank;vt=t->VActive+t->VBlank;
    if(ht>OTG0_OTG_H_TOTAL__OTG_H_TOTAL_MASK+1ul ||
       vt>OTG0_OTG_V_TOTAL__OTG_V_TOTAL_MASK+1ul)return BC250_OPTC_REASON_TOTAL;
    if(!s->VStartup || s->VStartup>=t->VBlank)return BC250_OPTC_REASON_VSTARTUP_BLANK;
    if(s->VStartup>Bc250DispGet(OTG0_OTG_VSTARTUP_PARAM__VSTARTUP_START_MASK,OTG0_OTG_VSTARTUP_PARAM__VSTARTUP_START_MASK))
        return BC250_OPTC_REASON_VSTARTUP_FIELD;
    if(s->VReady>Bc250DispGet(OTG0_OTG_VREADY_PARAM__VREADY_OFFSET_MASK,OTG0_OTG_VREADY_PARAM__VREADY_OFFSET_MASK))
        return BC250_OPTC_REASON_VREADY_FIELD;
    if(s->VUpdateOffset>Bc250DispGet(OTG0_OTG_VUPDATE_PARAM__VUPDATE_OFFSET_MASK,OTG0_OTG_VUPDATE_PARAM__VUPDATE_OFFSET_MASK))
        return BC250_OPTC_REASON_VUPDATE_OFFSET_FIELD;
    if(s->VUpdateWidth>Bc250DispGet(OTG0_OTG_VUPDATE_PARAM__VUPDATE_WIDTH_MASK,OTG0_OTG_VUPDATE_PARAM__VUPDATE_WIDTH_MASK))
        return BC250_OPTC_REASON_VUPDATE_WIDTH_FIELD;
    return BC250_OPTC_REASON_OK;
}
int Bc250OptcTimingValid(const BC250_TARGET_TIMING* t,const BC250_OPTC_SYNC* s)
{ return Bc250OptcTimingReason(t,s)==BC250_OPTC_REASON_OK; }
const char* Bc250OptcReasonName(int reason)
{
    // One hyphenated word per term, each naming this gate, so a caller can print it as the whole reason a
    // timing was refused and a reader can grep "optc-" for the gate. The longest is 25 characters, inside the
    // 32 that tools/quality/guardlog_width.py charges a %s in a driver log line.
    static const char* const names[]={"optc-ok","optc-extent","optc-flags","optc-sync","optc-total",
        "optc-vstartup-blank","optc-vstartup-field","optc-vready-field","optc-vupdate-offset-field",
        "optc-vupdate-width-field"};
    if(reason<0 || (unsigned)reason>=sizeof(names)/sizeof(names[0]))return "optc-invalid";
    return names[reason];
}
long Bc250OptcDisable(BC250_DISP_IO* io)
{
    struct optc o;
    if(!init(&o,io))return BC250_DISP_STATUS_INVALID;
    (void)optc1_disable_crtc(&o.base);return o.status;
}
long Bc250OptcProgram(BC250_DISP_IO* io,const BC250_TARGET_TIMING* t,const BC250_OPTC_SYNC* s,int dp)
{
    struct optc o;struct dc_crtc_timing timing;
    if(!init(&o,io) || !Bc250OptcTimingValid(t,s) || (dp!=0 && dp!=1))return BC250_DISP_STATUS_INVALID;
    // Do not change timings in an active OTG. Caller must first blank the plane,
    // disable the stream and reach the successful Disable return.
    if(readReg(&o,REG_OTG_CONTROL)&OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK)return BC250_DISP_STATUS_BUSY;
    if(o.status<0)return o.status;
    memset(&timing,0,sizeof(timing));
    timing.h_total=t->HActive+t->HBlank;timing.h_addressable=t->HActive;
    timing.h_front_porch=t->HSyncOffset;timing.h_sync_width=t->HSyncWidth;
    timing.v_total=t->VActive+t->VBlank;timing.v_addressable=t->VActive;
    timing.v_front_porch=t->VSyncOffset;timing.v_sync_width=t->VSyncWidth;
    timing.flags.HSYNC_POSITIVE_POLARITY=(t->Flags&2)!=0;
    timing.flags.VSYNC_POSITIVE_POLARITY=(t->Flags&4)!=0;
    optc1_program_timing(&o.base,&timing,(int)s->VReady,(int)s->VStartup,
        (int)s->VUpdateOffset,(int)s->VUpdateWidth,0,dp?SIGNAL_TYPE_DISPLAY_PORT:SIGNAL_TYPE_HDMI_TYPE_A,false);
    return o.status;
}
long Bc250OptcEnable(BC250_DISP_IO* io)
{
    struct optc o;
    if(!init(&o,io))return BC250_DISP_STATUS_INVALID;
    (void)optc2_enable_crtc(&o.base);return o.status;
}

long Bc250OptcReadGlobalSync(BC250_DISP_IO* io,BC250_OPTC_SYNC* s)
{
    struct optc o;uint32_t start,update,ready;
    if(!s)return BC250_DISP_STATUS_INVALID;
    memset(s,0,sizeof(*s));
    if(!init(&o,io))return BC250_DISP_STATUS_INVALID;
    start=readReg(&o,REG_OTG_VSTARTUP_PARAM);update=readReg(&o,REG_OTG_VUPDATE_PARAM);ready=readReg(&o,REG_OTG_VREADY_PARAM);
    if(o.status<0)return o.status;
    s->VStartup=Bc250DispGet(start,MASK_OTG_VSTARTUP_PARAM_VSTARTUP_START);
    s->VUpdateOffset=Bc250DispGet(update,MASK_OTG_VUPDATE_PARAM_VUPDATE_OFFSET);
    s->VUpdateWidth=Bc250DispGet(update,MASK_OTG_VUPDATE_PARAM_VUPDATE_WIDTH);
    s->VReady=Bc250DispGet(ready,MASK_OTG_VREADY_PARAM_VREADY_OFFSET);
    return BC250_DISP_STATUS_SUCCESS;
}
long Bc250OptcGlobalSyncLocked(BC250_DISP_IO* io,const BC250_OPTC_SYNC* s)
{
    struct optc o;BC250_OPTC_SYNC now;long status;
    if(!s || !s->VStartup || s->VStartup>Bc250DispGet(MASK_OTG_VSTARTUP_PARAM_VSTARTUP_START,MASK_OTG_VSTARTUP_PARAM_VSTARTUP_START) ||
       s->VReady>Bc250DispGet(MASK_OTG_VREADY_PARAM_VREADY_OFFSET,MASK_OTG_VREADY_PARAM_VREADY_OFFSET) ||
       s->VUpdateOffset>Bc250DispGet(MASK_OTG_VUPDATE_PARAM_VUPDATE_OFFSET,MASK_OTG_VUPDATE_PARAM_VUPDATE_OFFSET) ||
       s->VUpdateWidth>Bc250DispGet(MASK_OTG_VUPDATE_PARAM_VUPDATE_WIDTH,MASK_OTG_VUPDATE_PARAM_VUPDATE_WIDTH) ||
       !init(&o,io))return BC250_DISP_STATUS_INVALID;
    status=Bc250OptcReadGlobalSync(io,&now);if(status<0)return status;
    if(now.VStartup!=s->VStartup || now.VUpdateOffset!=s->VUpdateOffset)return BC250_DISP_STATUS_NOT_SUPPORTED;
    optc1_program_global_sync(&o.base,(int)s->VReady,(int)s->VStartup,(int)s->VUpdateOffset,(int)s->VUpdateWidth,0);
    return o.status;
}

long Bc250OptcReadTiming(BC250_DISP_IO* io,unsigned long pixelKhz,BC250_TARGET_TIMING* output)
{
    // Original AMD optc1_program_timing: total=N-1, sync start=0,
    // blank start=total-front porch, blank end=blank start-active.
    static const unsigned long registers[]={
        BC250_REG_DMU_OTG0_OTG_CONTROL,BC250_REG_DMU_OTG0_OTG_INTERLACE_CONTROL,
        BC250_REG_DMU_OTG0_OTG_H_TOTAL,BC250_REG_DMU_OTG0_OTG_V_TOTAL,
        BC250_REG_DMU_OTG0_OTG_H_BLANK_START_END,BC250_REG_DMU_OTG0_OTG_V_BLANK_START_END,
        BC250_REG_DMU_OTG0_OTG_H_SYNC_A,BC250_REG_DMU_OTG0_OTG_V_SYNC_A,
        BC250_REG_DMU_OTG0_OTG_H_SYNC_A_CNTL,BC250_REG_DMU_OTG0_OTG_V_SYNC_A_CNTL,
        BC250_REG_DMU_OTG0_OTG_V_TOTAL_CONTROL,BC250_REG_DMU_OTG0_OTG_H_TIMING_CNTL,
        BC250_REG_DMU_OTG0_PIXEL_RATE_CNTL,BC250_REG_DMU_OTG0_OTG_STEREO_CONTROL};
    unsigned long sample[2][sizeof(registers)/sizeof(registers[0])];
    unsigned pass,i;long status;unsigned long ht,vt,hs,he,vs,ve;
    BC250_TARGET_TIMING t;const BC250_OPTC_SYNC validation={0,1,0,0};
    if(!output)return BC250_DISP_STATUS_INVALID;
    memset(output,0,sizeof(*output));memset(&t,0,sizeof(t));
    // BD-139: the widest of the protocol ranges. The caller's plan applies the protocol's own ceiling, so a
    // firmware TMDS timing above 300 MHz is still refused there, by name (connector_link.c, clock-ceiling).
    if(!io || !io->Read || pixelKhz<25000 || pixelKhz>BC250_DP_MAX_PIXEL_KHZ)return BC250_DISP_STATUS_INVALID;
    for(pass=0;pass<2;++pass)for(i=0;i<sizeof(registers)/sizeof(registers[0]);++i) {
        status=Bc250DispRead(io,registers[i],&sample[pass][i]);if(status<0)return status;
    }
    if(memcmp(sample[0],sample[1],sizeof(sample[0])))return BC250_DISP_STATUS_BUSY;
#define S(i,r,f) Bc250DispGet(sample[0][i],OTG0_##r##__##f##_MASK)
    if(!S(0,OTG_CONTROL,OTG_MASTER_EN) ||
       (sample[0][0]&(OTG0_OTG_CONTROL__OTG_FIELD_NUMBER_CNTL_MASK|
                      OTG0_OTG_CONTROL__OTG_FIELD_NUMBER_POLARITY_MASK|
                      OTG0_OTG_CONTROL__OTG_DISP_READ_REQUEST_DISABLE_MASK)) ||
       sample[0][1] || sample[0][10] || sample[0][11] ||
       (sample[0][12]&(OTG0_PIXEL_RATE_CNTL__OTG0_ADD_PIXEL_MASK|
                       OTG0_PIXEL_RATE_CNTL__OTG0_DROP_PIXEL_MASK|
                       OTG0_PIXEL_RATE_CNTL__OTG0_DISPOUT_HALF_RATE_EN_MASK)) ||
       S(13,OTG_STEREO_CONTROL,OTG_STEREO_EN) ||
       S(6,OTG_H_SYNC_A,OTG_H_SYNC_A_START) || S(7,OTG_V_SYNC_A,OTG_V_SYNC_A_START) ||
       (sample[0][8]&~OTG0_OTG_H_SYNC_A_CNTL__OTG_H_SYNC_A_POL_MASK) ||
       (sample[0][9]&~OTG0_OTG_V_SYNC_A_CNTL__OTG_V_SYNC_A_POL_MASK))return BC250_DISP_STATUS_NOT_SUPPORTED;
    ht=S(2,OTG_H_TOTAL,OTG_H_TOTAL)+1;vt=S(3,OTG_V_TOTAL,OTG_V_TOTAL)+1;
    hs=S(4,OTG_H_BLANK_START_END,OTG_H_BLANK_START);he=S(4,OTG_H_BLANK_START_END,OTG_H_BLANK_END);
    vs=S(5,OTG_V_BLANK_START_END,OTG_V_BLANK_START);ve=S(5,OTG_V_BLANK_START_END,OTG_V_BLANK_END);
    if(hs<=he || hs>=ht || vs<=ve || vs>=vt)return BC250_DISP_STATUS_INVALID;
    t.PixelClockKhz=pixelKhz;t.HActive=hs-he;t.HBlank=ht-t.HActive;t.HSyncOffset=ht-hs;
    t.HSyncWidth=S(6,OTG_H_SYNC_A,OTG_H_SYNC_A_END);
    t.VActive=vs-ve;t.VBlank=vt-t.VActive;t.VSyncOffset=vt-vs;
    t.VSyncWidth=S(7,OTG_V_SYNC_A,OTG_V_SYNC_A_END);
    t.Flags=(unsigned char)(0x18u|(!S(8,OTG_H_SYNC_A_CNTL,OTG_H_SYNC_A_POL)?2u:0u)|
        (!S(9,OTG_V_SYNC_A_CNTL,OTG_V_SYNC_A_POL)?4u:0u));
#undef S
    if(!Bc250OptcTimingValid(&t,&validation))return BC250_DISP_STATUS_INVALID;
    *output=t;return BC250_DISP_STATUS_SUCCESS;
}
