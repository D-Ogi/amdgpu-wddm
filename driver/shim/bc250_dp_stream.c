// PROVENANCE: AMD DC (MIT), Linux 7d0a66e4, dcn10 DP stream methods used by DCN201.
#include "bc250_dp_stream.h"
#include "bc250_optc.h"
#include "bc250_atom_types.h"
#include "../kmd/regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include <stdarg.h>
#include <string.h>
#if defined(BC250_SHIM_KERNEL)
typedef unsigned char bool;
#define true 1
#define false 0
#else
#include <stdbool.h>
#endif
#include "hdmi_link_layouts.inc"
#include "dp_stream_layouts.inc"
#include "dp_stream_registers.inc"

struct dc_crtc_timing {
    uint32_t h_total,h_addressable,h_front_porch,h_sync_width,h_border_left,h_border_right;
    uint32_t v_total,v_addressable,v_front_porch,v_sync_width,v_border_top,v_border_bottom;
    uint32_t pix_clk_100hz;
    enum dc_pixel_encoding pixel_encoding;
    enum dc_color_depth display_color_depth;
    struct { unsigned INTERLACE,Y_ONLY,HSYNC_POSITIVE_POLARITY,VSYNC_POSITIVE_POLARITY; } flags;
};
enum { LINK_RATE_UNKNOWN=0 };
struct dc_link_settings { unsigned link_rate; };
struct encoder_unblank_param { struct dc_crtc_timing timing;struct dc_link_settings link_settings; };
struct stream_encoder { unsigned inst; };
struct dcn10_stream_encoder { struct stream_encoder base;BC250_DISP_IO* io;long status; };
struct dc_link;
struct link_service { void (*dp_trace_source_sequence)(struct dc_link*,enum dpcd_source_sequence); };
struct dc { const struct link_service* link_srv; };
struct dc_link { const struct dc* dc; };
// The original callback writes DPCD debug bytes only with
// enable_driver_sequence_debug. This driver leaves that debug mode disabled.
static void traceDisabled(struct dc_link* link,enum dpcd_source_sequence seq) {(void)link;(void)seq;}
static const struct link_service service={traceDisabled};
static const struct dc dc={&service};
static struct dc_link debugLink={&dc};
#define DCN10STRENC_FROM_STRENC(p) ((struct dcn10_stream_encoder*)(p))
#define REG(r) REG_##r
#define MASK(r,f) MASK_##r##_##f
static uint32_t readReg(struct dcn10_stream_encoder* e,uint32_t r)
{
    unsigned long v=0;
    if(e->status>=0)e->status=Bc250DispRead(e->io,r,&v);
    return (uint32_t)v;
}
static void writeReg(struct dcn10_stream_encoder* e,uint32_t r,uint32_t v)
{ if(e->status>=0)e->status=Bc250DispWrite(e->io,r,v); }
static void fields(struct dcn10_stream_encoder* e,uint32_t r,int fresh,unsigned count,...)
{
    uint32_t v=fresh?0:readReg(e,r);unsigned i;va_list ap;
    va_start(ap,count);
    for(i=0;i<count;++i) {
        uint32_t m=va_arg(ap,uint32_t),x=va_arg(ap,uint32_t);
        v=(v&~m)|(uint32_t)Bc250DispField(m,x);
    }
    va_end(ap);writeReg(e,r,v);
}
static void delay(struct dcn10_stream_encoder* e,unsigned us)
{ if(e->status>=0)e->status=Bc250DispStall(e->io,us); }
static void waitReg(struct dcn10_stream_encoder* e,uint32_t r,uint32_t m,unsigned want,unsigned us,unsigned count)
{
    unsigned i;
    for(i=0;i<=count && e->status>=0;++i) {
        uint32_t v=readReg(e,r);
        if(e->status<0 || Bc250DispGet(v,m)==want)return;
        if(i!=count)delay(e,us);
    }
    if(e->status>=0)e->status=BC250_DISP_STATUS_TIMEOUT;
}
#define PAIR(r,f,v) (uint32_t)MASK(r,f),(uint32_t)(v)
#define REG_READ(r) readReg(enc1,REG(r))
#define REG_WRITE(r,v) writeReg(enc1,REG(r),(v))
#define REG_GET(r,f,p) (*(p)=(uint32_t)Bc250DispGet(REG_READ(r),MASK(r,f)))
#define REG_UPDATE(r,f,v) fields(enc1,REG(r),0,1,PAIR(r,f,v))
#define REG_UPDATE_2(r,f,v,g,w) fields(enc1,REG(r),0,2,PAIR(r,f,v),PAIR(r,g,w))
#define REG_SET(r,init,f,v) fields(enc1,REG(r),1,1,PAIR(r,f,v))
#define REG_SET_2(r,init,f,v,g,w) fields(enc1,REG(r),1,2,PAIR(r,f,v),PAIR(r,g,w))
#define REG_SET_4(r,init,f,v,g,w,h,x,j,y) fields(enc1,REG(r),1,4,PAIR(r,f,v),PAIR(r,g,w),PAIR(r,h,x),PAIR(r,j,y))
#define REG_WAIT(r,f,w,d,n) waitReg(enc1,REG(r),MASK(r,f),w,d,n)
#define udelay(us) delay(enc1,us)
#define div_u64(a,b) ((uint64_t)(a)/(uint32_t)(b))
#pragma warning(push)
#pragma warning(disable:4100) // unchanged AMD code has an unused SDP splitting argument
#include "dp_stream.inc"
#pragma warning(pop)
static int init(struct dcn10_stream_encoder* e,BC250_DISP_IO* io)
{ memset(e,0,sizeof(*e));e->io=io;return io && io->Read && io->Write && io->Stall; }
long Bc250DpStreamProgram(BC250_DISP_IO* io,const BC250_TARGET_TIMING* t)
{
    const BC250_OPTC_SYNC check={0,1,0,0};struct dcn10_stream_encoder e;struct dc_crtc_timing timing;
    if(!init(&e,io) || !Bc250OptcTimingValid(t,&check))return BC250_DISP_STATUS_INVALID;
    if(readReg(&e,REG(DP_VID_STREAM_CNTL))&(DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK|
        DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_STATUS_MASK))return BC250_DISP_STATUS_BUSY;
    if(e.status<0)return e.status;
    memset(&timing,0,sizeof(timing));
    timing.h_total=t->HActive+t->HBlank;timing.h_addressable=t->HActive;
    timing.h_front_porch=t->HSyncOffset;timing.h_sync_width=t->HSyncWidth;
    timing.v_total=t->VActive+t->VBlank;timing.v_addressable=t->VActive;
    timing.v_front_porch=t->VSyncOffset;timing.v_sync_width=t->VSyncWidth;
    timing.pixel_encoding=PIXEL_ENCODING_RGB;timing.display_color_depth=COLOR_DEPTH_888;
    timing.flags.HSYNC_POSITIVE_POLARITY=(t->Flags&2)!=0;
    timing.flags.VSYNC_POSITIVE_POLARITY=(t->Flags&4)!=0;
    enc1_stream_encoder_dp_set_stream_attribute(&e.base,&timing,COLOR_SPACE_SRGB,false,0);
    return e.status;
}
long Bc250DpStreamBlank(BC250_DISP_IO* io)
{
    struct dcn10_stream_encoder e;
    if(!init(&e,io))return BC250_DISP_STATUS_INVALID;
    enc1_stream_encoder_dp_blank(&debugLink,&e.base);return e.status;
}
long Bc250DpStreamUnblank(BC250_DISP_IO* io,unsigned khz,unsigned rate)
{
    struct dcn10_stream_encoder e;struct encoder_unblank_param p;
    if(!init(&e,io) || khz<25000 || khz>300000 || (rate!=6 && rate!=10))return BC250_DISP_STATUS_INVALID;
    memset(&p,0,sizeof(p));p.link_settings.link_rate=rate;p.timing.pix_clk_100hz=khz*10;
    p.timing.pixel_encoding=PIXEL_ENCODING_RGB;
    enc1_stream_encoder_dp_unblank(&debugLink,&e.base,&p);return e.status;
}
