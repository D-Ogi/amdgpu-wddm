// PROVENANCE: Linux AMD DC (MIT) training loops; drm_dp.h (MIT/X11) constants.
#include "bc250_dp_train.h"
#include <string.h>
#if defined(BC250_SHIM_KERNEL)
typedef unsigned char bool;
#define true 1
#define false 0
#else
#include <stdbool.h>
#endif
#include "dp_training_types.inc"
#include "dpcd_constants.inc"

#define TRAIN_MAX_US ((uint64_t)2000000)
struct dc_link_settings { enum dc_lane_count lane_count; unsigned link_rate; };
struct dc_lane_settings { unsigned Swing, Emphasis; };
struct link_training_settings {
    struct dc_link_settings link_settings;
    struct dc_lane_settings hw_lane_settings[4];
    union dpcd_training_lane dpcd_lane_settings[4];
    enum dc_dp_training_pattern pattern_for_cr, pattern_for_eq;
    uint32_t cr_pattern_time, eq_pattern_time;
};
struct link_resource { unsigned unused; };
struct shim_dc { struct { unsigned lt_early_cr_pattern; } work_arounds; };
struct shim_ctx { struct shim_dc *dc; };
struct dc_link {
    struct shim_ctx *ctx;
    const BC250_DP_TRAIN_IO *io;
    BC250_DP_TRAIN_RESULT *r;
    uint64_t began;
};
enum dc_status { DC_OK, DC_ERROR };

static int current(struct dc_link *l)
{
    if(l->r->Status!=BC250_DP_TRAIN_OK) return 0;
    if(!l->io->Current(l->io->Context)) l->r->Status=BC250_DP_TRAIN_CANCELLED;
    else if(l->io->NowUs(l->io->Context)-l->began>=TRAIN_MAX_US) l->r->Status=BC250_DP_TRAIN_TIMEOUT;
    return l->r->Status==BC250_DP_TRAIN_OK;
}
static int checked(struct dc_link *l,int rc)
{
    if(rc && l->r->Status==BC250_DP_TRAIN_OK) l->r->Status=BC250_DP_TRAIN_IO_ERROR;
    return current(l);
}
static int wr(struct dc_link *l,uint32_t addr,const uint8_t *p,unsigned n)
{
    if(!current(l)) return 0;
    ++l->r->Writes;
    return checked(l,l->io->Write(l->io->Context,addr,p,n));
}
static enum dc_status rd(struct dc_link *l,uint32_t addr,uint8_t *p,unsigned n)
{
    if(!current(l)) return DC_ERROR;
    ++l->r->Reads;
    return checked(l,l->io->Read(l->io->Context,addr,p,n))?DC_OK:DC_ERROR;
}
static void dp_wait_for_training_aux_rd_interval(struct dc_link *l,uint32_t us)
{
    uint64_t elapsed;
    if(!current(l)) return;
    elapsed=l->io->NowUs(l->io->Context)-l->began;
    if(elapsed>=TRAIN_MAX_US || us>=TRAIN_MAX_US-elapsed) {
        l->r->Status=BC250_DP_TRAIN_TIMEOUT;return;
    }
    l->r->WaitUs+=us;
    (void)checked(l,l->io->DelayUs(l->io->Context,us));
}
static enum dp_link_encoding link_dp_get_encoding_format(const struct dc_link_settings *s)
{ (void)s;return DP_8b_10b_ENCODING; }
static bool is_repeater(const struct link_training_settings *s,uint32_t offset)
{ (void)s;(void)offset;return false; } // no LTTPR in this SST route

#include "dp_training_checks.inc"

static bool dp_check_interlane_aligned(union lane_align_status_updated a,struct dc_link *l,uint32_t retries)
{ (void)l;(void)retries;return dp_is_interlane_aligned(a); } // no sink-specific bypass
static uint32_t dp_get_eq_aux_rd_interval(struct dc_link *l,const struct link_training_settings *s,
    uint32_t offset,uint32_t retries)
{ (void)l;(void)offset;(void)retries;return s->eq_pattern_time; }
static bool dp_check_dpcd_reqeust_status(struct dc_link *l,enum dc_status s)
{
    // Deliberate deviation: AMD's helper aborts only USB4 on AUX failure. This
    // driver aborts on every transport error, never treating stale data as ACK.
    return s!=DC_OK || !current(l);
}
static void dp_set_hw_training_pattern(struct dc_link *l,const struct link_resource *res,
    enum dc_dp_training_pattern pattern,uint32_t offset)
{
    (void)res;(void)offset;
    if(!current(l)) return;
    l->r->Phase=pattern==DP_TRAINING_PATTERN_SEQUENCE_1?1u:2u;
    // The PHY callback takes TPS1 to TPS4 as 1 to 4 (SEQUENCE_1 to SEQUENCE_4 are 0 to 3).
    (void)checked(l,l->io->Pattern(l->io->Context,(unsigned)pattern+1u));
}
static void dp_set_hw_lane_settings(struct dc_link *l,const struct link_resource *res,
    const struct link_training_settings *s,uint32_t offset)
{
    (void)res;(void)offset;
    if(!current(l)) return;
    l->r->Swing=s->hw_lane_settings[0].Swing;l->r->PreEmphasis=s->hw_lane_settings[0].Emphasis;
    (void)checked(l,l->io->Drive(l->io->Context,l->r->Swing,l->r->PreEmphasis));
}
static void dpcd_set_lane_settings(struct dc_link *l,const struct link_training_settings *s,uint32_t offset)
{
    uint8_t b[4];unsigned i;(void)offset;
    for(i=0;i<(unsigned)s->link_settings.lane_count;++i) b[i]=s->dpcd_lane_settings[i].raw;
    (void)wr(l,DP_TRAINING_LANE0_SET,b,(unsigned)s->link_settings.lane_count);
}
static void dpcd_set_lt_pattern_and_lane_settings(struct dc_link *l,const struct link_training_settings *s,
    enum dc_dp_training_pattern pattern,uint32_t offset)
{
    static const uint8_t dpcd[4]={DP_TRAINING_PATTERN_1,DP_TRAINING_PATTERN_2,DP_TRAINING_PATTERN_3,
        DP_TRAINING_PATTERN_4};
    uint8_t b[5];unsigned i;(void)offset;
    // dp_training_pattern_to_dpcd_training_pattern and dp_initialize_scrambling_data_symbols: TPS1 to TPS3 go
    // out with scrambling disabled, TPS4 scrambled. Only the four 8b/10b sequences reach this function.
    b[0]=(uint8_t)(dpcd[(unsigned)pattern&3u]|(pattern==DP_TRAINING_PATTERN_SEQUENCE_4?0u:DP_LINK_SCRAMBLING_DISABLE));
    for(i=0;i<(unsigned)s->link_settings.lane_count;++i) b[i+1]=s->dpcd_lane_settings[i].raw;
    (void)wr(l,DP_TRAINING_PATTERN_SET,b,1+(unsigned)s->link_settings.lane_count);
}
static enum dc_status dp_get_lane_status_and_lane_adjust(struct dc_link *l,const struct link_training_settings *s,
    union lane_status lane[4],union lane_align_status_updated *align,union lane_adjust adjust[4],uint32_t offset)
{
    unsigned i;enum dc_status status;(void)offset;
    status=rd(l,DP_LANE0_1_STATUS,l->r->LastStatus,6);
    if(status!=DC_OK) return status;
    for(i=0;i<(unsigned)s->link_settings.lane_count;++i) {
        lane[i].raw=(l->r->LastStatus[i/2]>>((i%2)*4))&15;
        adjust[i].raw=(l->r->LastStatus[4+i/2]>>((i%2)*4))&15;
    }
    align->raw=l->r->LastStatus[2];
    if(l->r->Phase==1) ++l->r->CrSamples;else ++l->r->EqSamples;
    return DC_OK;
}
static void dp_decide_lane_settings(struct link_training_settings *s,const union lane_adjust adjust[4],
    struct dc_lane_settings hw[4],union dpcd_training_lane lane[4])
{
    unsigned i,vs=0,pe=0;
    // Same policy as AMD maximize_lane_settings, restricted to the legacy PHY
    // that programs one common drive setting: no UHBR or per-lane override.
    for(i=0;i<(unsigned)s->link_settings.lane_count;++i) {
        if(adjust[i].bits.VOLTAGE_SWING_LANE>vs) vs=adjust[i].bits.VOLTAGE_SWING_LANE;
        if(adjust[i].bits.PRE_EMPHASIS_LANE>pe) pe=adjust[i].bits.PRE_EMPHASIS_LANE;
    }
    if(pe>3u-vs) pe=3u-vs;
    for(i=0;i<4;++i) {
        hw[i].Swing=vs;hw[i].Emphasis=pe;
        lane[i].raw=(uint8_t)(vs|(vs==3u?4u:0u)|(pe<<3)|(pe==3u?32u:0u));
    }
}
#define DC_LOG_HW_LINK_TRAINING(...) ((void)0)
#define DC_LOG_ERROR(...) ((void)0)
// An exhausted upstream retry count is a measured refusal, not a kernel assert.
#define ASSERT(ok) ((void)(ok))
#include "dp_training_loops.inc"

static unsigned maxRate(const uint8_t caps[16])
{
    unsigned lanes,rate;
    if(!caps || caps[DP_DPCD_REV]<DP_DPCD_REV_10 || caps[DP_DPCD_REV]>DP_DPCD_REV_14) return 0;
    lanes=caps[DP_MAX_LANE_COUNT]&DP_MAX_LANE_COUNT_MASK;rate=caps[DP_MAX_LINK_RATE];
    if(lanes!=1 && lanes!=2 && lanes!=4) return 0;
    if(rate!=LINK_RATE_LOW && rate!=LINK_RATE_HIGH && rate!=LINK_RATE_HIGH2 && rate!=LINK_RATE_HIGH3) return 0;
    if((caps[DP_TRAINING_AUX_RD_INTERVAL]&DP_TRAINING_AUX_RD_MASK)>6) return 0;
    // BD-139: every 8b/10b rate the sink names. dcn201_resource.c link_enc_feature declares this encoder HBR2, HBR3,
    // TPS3 and TPS4 capable (Linux 7d0a66e4), and DIG1TransmitterControl takes the four symbol clocks.
    return rate;
}
// 27 MHz reference * rate * lanes * 8 payload bits / RGB8's 24 bits.
static uint32_t capacity(unsigned rate,unsigned lanes) { return rate*27000u*lanes/3u; }
static const unsigned g_Rates[4]={LINK_RATE_LOW,LINK_RATE_HIGH,LINK_RATE_HIGH2,LINK_RATE_HIGH3};
// BD-139: every mode HBR carries starts at HBR as before (RBR when the sink has nothing faster), so the links that
// trained before keep their rate. A mode HBR cannot carry starts at the lowest faster rate that carries it.
static unsigned firstRate(unsigned max,unsigned lanes,uint32_t pixel)
{
    unsigned i=max>=LINK_RATE_HIGH?1u:0u;
    while(i<3u && g_Rates[i]<max && pixel>capacity(g_Rates[i],lanes)) ++i;
    return g_Rates[i];
}
static unsigned lowerRate(unsigned rate)
{
    unsigned i;
    for(i=1;i<4u;++i) if(g_Rates[i]==rate) return g_Rates[i-1];
    return 0;
}
// Linux decide_eq_training_pattern: TPS4 when the sink sets TPS4_SUPPORTED, else TPS3 when it sets TPS3_SUPPORTED,
// else TPS2. Deviation: RBR and HBR keep TPS2, the pattern every link of this driver trained with before BD-139, so
// a mode HBR carries trains exactly as it did. DP 1.2 makes TPS3 mandatory for an HBR2 sink.
static enum dc_dp_training_pattern eqPattern(const uint8_t caps[16],unsigned rate)
{
    if(rate<LINK_RATE_HIGH2) return DP_TRAINING_PATTERN_SEQUENCE_2;
    if(caps[DP_MAX_DOWNSPREAD]&DP_TPS4_SUPPORTED) return DP_TRAINING_PATTERN_SEQUENCE_4;
    if(caps[DP_MAX_LANE_COUNT]&DP_TPS3_SUPPORTED) return DP_TRAINING_PATTERN_SEQUENCE_3;
    return DP_TRAINING_PATTERN_SEQUENCE_2;
}
uint32_t Bc250DpMaxPixelClock(const uint8_t caps[16])
{
    unsigned rate=maxRate(caps);
    if(!rate) return 0;
    return capacity(rate,caps[DP_MAX_LANE_COUNT]&DP_MAX_LANE_COUNT_MASK);
}
unsigned Bc250DpFirstRate(const uint8_t caps[16],uint32_t pixel)
{
    unsigned rate=maxRate(caps),lanes;
    if(!rate || !pixel) return 0;
    lanes=caps[DP_MAX_LANE_COUNT]&DP_MAX_LANE_COUNT_MASK;
    return pixel>capacity(rate,lanes)?0u:firstRate(rate,lanes,pixel);
}
typedef char downstreamPortAddress[BC250_DP_DOWNSTREAM_PORT_DPCD==DP_DOWNSTREAM_PORT_0?1:-1];
int Bc250DpDownstreamPortReadable(const uint8_t caps[16])
{
    return caps && (caps[DP_DOWNSTREAMPORT_PRESENT]&DP_DWN_STRM_PORT_PRESENT) && caps[DP_DPCD_REV]>=DP_DPCD_REV_11;
}
// Linux get_active_converter_info: DPCD 0x005 gives the class of the port, and on DPCD 1.1 and later the type of
// 0x080 replaces it (a DP type there means no converter). Only the first port is read, as Linux reads it.
// Deviation: Linux dp_active_dongle_validate_timing limits only an HDMI converter that states its clock. This
// driver also limits a VGA or DVI converter by its own byte, and gives every converter port that states no clock
// BC250_DP_CONVERTER_DEFAULT_KHZ, the TMDS limit of this driver's own HDMI and DVI route. Before the flat 300 MHz
// DP limit went (BD-139), that limit covered these converters.
uint32_t Bc250DpDownstreamLimit(const uint8_t caps[16],const uint8_t *port,unsigned *type)
{
    unsigned kind=BC250_DP_DOWNSTREAM_NONE;uint32_t khz=0;
    if(caps && (caps[DP_DOWNSTREAMPORT_PRESENT]&DP_DWN_STRM_PORT_PRESENT)) {
        switch(caps[DP_DOWNSTREAMPORT_PRESENT]&DP_DWN_STRM_PORT_TYPE_MASK) {
        case DP_DWN_STRM_PORT_TYPE_DP: kind=BC250_DP_DOWNSTREAM_DP;break;
        case DP_DWN_STRM_PORT_TYPE_ANALOG: kind=BC250_DP_DOWNSTREAM_VGA;break;
        case DP_DWN_STRM_PORT_TYPE_TMDS: kind=BC250_DP_DOWNSTREAM_DVI;break;   // Linux: DVI until 0x080 says more
        default: kind=BC250_DP_DOWNSTREAM_OTHER;break;
        }
        if(port && Bc250DpDownstreamPortReadable(caps)) {
            switch(port[0]&DP_DS_PORT_TYPE_MASK) {
            case DP_DS_PORT_TYPE_DP: kind=BC250_DP_DOWNSTREAM_DP;break;
            case DP_DS_PORT_TYPE_VGA: kind=BC250_DP_DOWNSTREAM_VGA;break;
            case DP_DS_PORT_TYPE_DVI: kind=BC250_DP_DOWNSTREAM_DVI;break;
            case DP_DS_PORT_TYPE_HDMI: case DP_DS_PORT_TYPE_DP_DUALMODE: kind=BC250_DP_DOWNSTREAM_HDMI;break;
            default: kind=BC250_DP_DOWNSTREAM_OTHER;break;
            }
            if(caps[DP_DOWNSTREAMPORT_PRESENT]&DP_DETAILED_CAP_INFO_AVAILABLE) {
                if(kind==BC250_DP_DOWNSTREAM_VGA) khz=port[1]*8000u;
                else if(kind==BC250_DP_DOWNSTREAM_DVI || kind==BC250_DP_DOWNSTREAM_HDMI) khz=port[1]*2500u;
            }
        }
        if(kind!=BC250_DP_DOWNSTREAM_DP && !khz) khz=BC250_DP_CONVERTER_DEFAULT_KHZ;
    }
    if(type) *type=kind;
    return khz;
}
uint32_t Bc250DpSinkMaxPixelClock(const uint8_t caps[16],const uint8_t *port,unsigned *type,uint32_t *converter)
{
    uint32_t link=Bc250DpMaxPixelClock(caps),khz=Bc250DpDownstreamLimit(caps,port,type);
    if(converter) *converter=khz;
    return khz && khz<link?khz:link;
}
static int cleanup(struct dc_link *l)
{
    uint8_t off=DP_TRAINING_PATTERN_DISABLE;int rc=0;
    // Do not send AUX to a replacement sink. Hardware Disable is always owed,
    // under the caller's serialized lifetime, even if Current became false.
    if(l->io->Current(l->io->Context) && l->io->Write(l->io->Context,DP_TRAINING_PATTERN_SET,&off,1)) rc=1;
    if(l->io->Disable(l->io->Context)) rc=1;
    l->r->CleanupFailed|=(unsigned)rc;return rc;
}
enum BC250_DP_TRAIN_STATUS Bc250DpTrain(const BC250_DP_TRAIN_IO *io,const uint8_t caps[16],
    uint32_t pixel,BC250_DP_TRAIN_RESULT *r)
{
    struct shim_dc dc={0};struct shim_ctx ctx={&dc};struct dc_link l={&ctx,io,r,0};
    struct link_resource resource={0};struct link_training_settings s;
    enum link_training_result result;unsigned rate,next,interval,lanes;
    BC250_DP_TRAIN_ATTEMPT *tried;
    uint8_t b[2];int touched=0;
    if(!r) return BC250_DP_TRAIN_ARGUMENT;
    memset(r,0,sizeof(*r));
    if(!io || !caps || !pixel || !io->Current || !io->Read || !io->Write || !io->Enable ||
        !io->Pattern || !io->Drive || !io->Disable || !io->NowUs || !io->DelayUs) return r->Status=BC250_DP_TRAIN_ARGUMENT;
    rate=maxRate(caps);
    if(!rate || pixel>Bc250DpMaxPixelClock(caps)) return r->Status=BC250_DP_TRAIN_UNSUPPORTED;
    lanes=caps[DP_MAX_LANE_COUNT]&DP_MAX_LANE_COUNT_MASK;
    interval=caps[DP_TRAINING_AUX_RD_INTERVAL]&DP_TRAINING_AUX_RD_MASK;
    l.began=io->NowUs(io->Context);
    r->Lanes=lanes;r->Enhanced=(caps[DP_MAX_LANE_COUNT]&DP_ENHANCED_FRAME_CAP)!=0;
    rate=firstRate(rate,lanes,pixel);r->FirstRate=rate;
    // Rates only go down, so there are at most four attempts and each has its own record.
    for(;;) {
        r->Rate=rate;tried=&r->Tried[r->Attempts++&3u];
        memset(&s,0,sizeof(s));s.link_settings.lane_count=(enum dc_lane_count)lanes;s.link_settings.link_rate=rate;
        s.pattern_for_cr=DP_TRAINING_PATTERN_SEQUENCE_1;s.pattern_for_eq=eqPattern(caps,rate);
        r->EqPattern=(unsigned)s.pattern_for_eq+1u;tried->Rate=rate;tried->EqPattern=r->EqPattern;
        s.eq_pattern_time=dp_translate_training_aux_read_interval(interval);
        s.cr_pattern_time=interval?s.eq_pattern_time:100;
        // DP 1.4 CR reads use 100 us irrespective of the EQ interval.
        if(caps[DP_DPCD_REV]>=DP_DPCD_REV_14) s.cr_pattern_time=100;
        if(!current(&l)) break;
        touched=1;
        if(!checked(&l,io->Enable(io->Context,rate,lanes,(int)r->Enhanced))) break;
        if(caps[DP_DPCD_REV]>=DP_DPCD_REV_11) {
            b[0]=DP_SET_POWER_D0;if(!wr(&l,DP_SET_POWER,b,1)) break;
            dp_wait_for_training_aux_rd_interval(&l,1000);
        }
        b[0]=(uint8_t)rate;b[1]=(uint8_t)(lanes|(r->Enhanced?DP_LANE_COUNT_ENHANCED_FRAME_EN:0));
        if(!wr(&l,DP_LINK_BW_SET,b,2)) break;
        b[0]=0;b[1]=DP_SET_ANSI_8B10B;
        if(!wr(&l,DP_DOWNSPREAD_CTRL,b,2)) break;
        result=perform_8b_10b_clock_recovery_sequence(&l,&resource,&s,0);
        if(result==LINK_TRAINING_SUCCESS) result=perform_8b_10b_channel_equalization_sequence(&l,&resource,&s,0);
        r->AmdResult=(unsigned)result;tried->Phase=r->Phase;tried->AmdResult=r->AmdResult;
        if(!current(&l)) break;
        if(result==LINK_TRAINING_SUCCESS) {
            b[0]=DP_TRAINING_PATTERN_DISABLE;
            if(!wr(&l,DP_TRAINING_PATTERN_SET,b,1) || !current(&l) || !checked(&l,io->Pattern(io->Context,0))) break;
            r->Phase=3;r->ElapsedUs=io->NowUs(io->Context)-l.began;return r->Status;
        }
        if(cleanup(&l)) { r->Status=BC250_DP_TRAIN_IO_ERROR;touched=0;break; }
        touched=0;
        // The next lower rate is tried while it still carries the mode: HBR to RBR as before BD-139. A first rate
        // above HBR was chosen because HBR does not carry the mode, so its failure ends here; the connector then
        // keeps this sink below it (connector_link.c).
        next=lowerRate(rate);
        if(next && pixel<=capacity(next,lanes)) { rate=next;continue; }
        r->Status=r->Phase==1?BC250_DP_TRAIN_CR_FAILED:BC250_DP_TRAIN_EQ_FAILED;break;
    }
    if(touched) (void)cleanup(&l);
    r->ElapsedUs=io->NowUs(io->Context)-l.began;return r->Status;
}
