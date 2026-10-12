// PROVENANCE: AMD DC (MIT), Linux v6.18 7d0a66e4; generated stream/AVI functions.
#include "bc250_hdmi_link.h"
#include <stdarg.h>
#include <string.h>
#if defined(BC250_SHIM_KERNEL)
typedef unsigned char bool;
#define true 1
#define false 0
#else
#include <stdbool.h>
#endif
#include "../kmd/regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "atom_layouts.inc"
#include "hdmi_link_layouts.inc"

#define R(r) ((uint32_t)(BC250_REG_DMU_DIG0_##r / 4u))
#define M(r,f) ((uint32_t)DIG0_##r##__##f##_MASK)
#define WRITE_REGS(X) \
 X(TMDS_CTL_BITS) X(DIG_CLOCK_PATTERN) X(DIG_FE_CNTL) X(DIG_BE_CNTL) \
 X(HDMI_CONTROL) X(HDMI_VBI_PACKET_CONTROL) X(HDMI_GC) X(HDMI_DB_CONTROL) \
 X(HDMI_GENERIC_PACKET_CONTROL0) X(HDMI_GENERIC_PACKET_CONTROL1) \
 X(HDMI_GENERIC_PACKET_CONTROL2) X(HDMI_GENERIC_PACKET_CONTROL3) X(HDMI_GENERIC_PACKET_CONTROL4) \
 X(AFMT_CNTL) X(AFMT_VBI_PACKET_CONTROL) X(AFMT_VBI_PACKET_CONTROL1) \
 X(AFMT_GENERIC_HDR) X(AFMT_GENERIC_0) X(AFMT_GENERIC_1) X(AFMT_GENERIC_2) \
 X(AFMT_GENERIC_3) X(AFMT_GENERIC_4) X(AFMT_GENERIC_5) X(AFMT_GENERIC_6) X(AFMT_GENERIC_7)
#define REG_ITEM(r) R(r),
static const uint32_t WriteRegs[] = { WRITE_REGS(REG_ITEM) BC250_REG_DMU_DIO_MEM_PWR_CTRL/4u };
#undef REG_ITEM
static const BC250_HDMI_FIELD_POLICY FieldPolicy[] = {
#include "hdmi_link_policy.inc"
};
static const uint32_t ReadOnlyRegs[] = {
    R(DIG_BE_EN_CNTL), R(HDMI_STATUS), BC250_REG_DMU_DIG1_DIG_BE_EN_CNTL/4u,
    BC250_REG_DMU_OTG0_OTG_CONTROL/4u, BC250_REG_DMU_OTG1_OTG_CONTROL/4u
};
unsigned Bc250HdmiLinkPolicyCount(void)
{ return (unsigned)(sizeof(FieldPolicy)/sizeof(FieldPolicy[0])); }
const BC250_HDMI_FIELD_POLICY *Bc250HdmiLinkPolicyAt(unsigned index)
{ return index<Bc250HdmiLinkPolicyCount()?&FieldPolicy[index]:NULL; }
const BC250_HDMI_FIELD_POLICY *Bc250HdmiLinkPolicy(uint32_t r)
{
    unsigned i;
    for(i=0;i<sizeof(FieldPolicy)/sizeof(FieldPolicy[0]);++i)
        if(FieldPolicy[i].Register==r) return &FieldPolicy[i];
    return NULL;
}

static uint32_t snapshotRegister(unsigned index)
{
    unsigned i;
    for(i=0;i<Bc250HdmiLinkPolicyCount();++i) {
        if(FieldPolicy[i].PacketData)continue;
        if(!index)return FieldPolicy[i].Register;
        --index;
    }
    return index<sizeof(ReadOnlyRegs)/sizeof(ReadOnlyRegs[0])?ReadOnlyRegs[index]:UINT32_MAX;
}
int Bc250HdmiLinkSnapshot(const BC250_ATOM_CALLBACKS* io,BC250_HDMI_SNAPSHOT* snapshot)
{
    unsigned i;
    int result=0;
    if(!snapshot)return -1;
    memset(snapshot,0,sizeof(*snapshot));
    if(!io || !io->Owned || !io->Read)return -1;
    while(snapshotRegister(snapshot->Count)!=UINT32_MAX) {
        if(snapshot->Count==BC250_HDMI_SNAPSHOT_REGS)return -1;
        ++snapshot->Count;
    }
    for(i=0;i<snapshot->Count;++i) {
        BC250_HDMI_SNAPSHOT_ENTRY* e=&snapshot->Entries[i];
        e->Register=snapshotRegister(i);e->Status=-1;
        if(io->Owned(io->Context) && !io->Read(io->Context,e->Register,&e->Value))e->Status=0;
        if(e->Status)result=-1;
    }
    snapshot->Complete=result==0;return result;
}

static int unknownRefusal(BC250_HDMI_LINK* l,uint32_t reg,uint32_t before,uint32_t wanted,uint32_t mask)
{
    unsigned i;BC250_HDMI_PREFLIGHT* d=l->Shadow;
    BC250_HDMI_REFUSAL r={reg,6,wanted,before,mask};
    l->FailureRegister=reg;l->FailureOperation=6;l->FailureWanted=wanted;
    l->FailureReadback=before;l->FailureMask=mask;
    if(!d)return l->Error=-1;
    ++d->Events;
    for(i=0;i<d->Count;++i)if(!memcmp(&d->Refusals[i],&r,sizeof(r)))return 0;
    if(d->Count==BC250_HDMI_PREFLIGHT_REFUSALS) {d->Overflow=1;return l->Error=-1;}
    d->Refusals[d->Count++]=r;return 0;
}

static uint32_t field(uint32_t mask, uint32_t value)
{
    unsigned shift=0;
    if (!mask) return 0;
    while (!(mask & (UINT32_C(1)<<shift))) ++shift;
    return (value<<shift)&mask;
}
static uint32_t verifyMask(uint32_t r)
{
    const BC250_HDMI_FIELD_POLICY *p=Bc250HdmiLinkPolicy(r);
    return p?p->Configuration:0;
}
int Bc250HdmiLinkAllowed(uint32_t reg, int write)
{
    unsigned i;
    for (i=0;i<sizeof(WriteRegs)/sizeof(WriteRegs[0]);++i)
        if (reg==WriteRegs[i]) {
            const BC250_HDMI_FIELD_POLICY* p=Bc250HdmiLinkPolicy(reg);
            return p && (write || !p->PacketData);
        }
    if (!write && (reg==R(DIG_BE_EN_CNTL) || reg==R(HDMI_STATUS) ||
        reg==BC250_REG_DMU_DIG1_DIG_BE_EN_CNTL/4u || reg==BC250_REG_DMU_OTG0_OTG_CONTROL/4u ||
        reg==BC250_REG_DMU_OTG1_OTG_CONTROL/4u)) return 1;
    return 0;
}
static int readReg(BC250_HDMI_LINK *l,uint32_t reg,uint32_t *v)
{
    if (!Bc250HdmiLinkAllowed(reg,0) || !l->Io.Owned(l->Io.Context) ||
        l->Io.Read(l->Io.Context,reg,v)) {
        l->FailureRegister=reg;l->FailureOperation=1;
        l->FailureWanted=l->FailureReadback=l->FailureMask=0;return l->Error=-1;
    }
    return 0;
}
static int saveConfiguration(BC250_HDMI_LINK* l,uint32_t reg,uint32_t before)
{
    unsigned i;
    for(i=0;i<l->Count;++i)if(l->Offset[i]==reg)return 0;
    if(i==BC250_HDMI_LINK_REGS)return l->Error=-1;
    l->Offset[i]=reg;l->Before[i]=before;++l->Count;return 0;
}
static int admitPacket0(BC250_HDMI_LINK* l,int allInactive)
{
    uint32_t control,line,mask=allInactive?UINT32_MAX:
        M(HDMI_GENERIC_PACKET_CONTROL0,HDMI_GENERIC0_CONT)|M(HDMI_GENERIC_PACKET_CONTROL0,HDMI_GENERIC0_SEND);
    if(readReg(l,R(HDMI_GENERIC_PACKET_CONTROL0),&control))return -1;
    if(control&mask) {
        l->FailureRegister=R(HDMI_GENERIC_PACKET_CONTROL0);l->FailureOperation=4;
        l->FailureWanted=0;l->FailureReadback=control;l->FailureMask=mask;return l->Error=-1;
    }
    // Save control state before any packet data write, including partial starts.
    // No old packet RAM is sampled or replayed. Only inactive packet 0 is owned.
    if(readReg(l,R(HDMI_GENERIC_PACKET_CONTROL1),&line) ||
       saveConfiguration(l,R(HDMI_GENERIC_PACKET_CONTROL0),control) ||
       saveConfiguration(l,R(HDMI_GENERIC_PACKET_CONTROL1),line))return -1;
    l->PacketAdmitted=1;return 0;
}
static int writeReg(BC250_HDMI_LINK *l,uint32_t reg,uint32_t v)
{
    uint32_t before=0,after=0;
    const BC250_HDMI_FIELD_POLICY *policy=Bc250HdmiLinkPolicy(reg);
    if (l->Error) return -1;
    if (!policy || !Bc250HdmiLinkAllowed(reg,1) || !l->Io.Owned(l->Io.Context)) {
        l->FailureRegister=reg;l->FailureOperation=2;return l->Error=-1;
    }
    if(policy->PacketData) {
        // AMD enc1_update_generic_info_packet writes the complete indexed data
        // word, then commits. It does not specify an immediate MMIO echo. Keep
        // ordinary register verification intact; never use packet reads as a
        // configuration journal. Successful callbacks do not prove wire output.
        if(policy->PacketData!=UINT32_MAX || !l->PacketAdmitted) {
            l->FailureRegister=reg;l->FailureOperation=2;return l->Error=-1;
        }
        l->Dirty=1;l->PacketDirty=1;
        if(l->Io.Write(l->Io.Context,reg,v)) {
            l->FailureRegister=reg;l->FailureOperation=3;l->FailureWanted=v;
            l->FailureReadback=0;l->FailureMask=UINT32_MAX;return l->Error=-1;
        }
        return 0; // Packet data callback accepted, not readback-verified.
    }
    // Preserve inherited unknown bits. Only their equality is admitted; this
    // is not a claim that an unknown field is writable configuration.
    if (readReg(l,reg,&before)) return -1;
    if (((before^v)&policy->Unknown) && unknownRefusal(l,reg,before,v,policy->Unknown))return -1;
    if(saveConfiguration(l,reg,before))return -1;
    // A callback failure can follow a posted write. Restore also this register.
    l->Dirty=1;
    v &= ~policy->Status;
    if (l->Io.Write(l->Io.Context,reg,v)) {
        l->FailureRegister=reg;l->FailureOperation=3;l->FailureWanted=v;
        l->FailureReadback=0;l->FailureMask=verifyMask(reg);return l->Error=-1;
    }
    if (readReg(l,reg,&after)) {l->FailureWanted=v;l->FailureMask=verifyMask(reg);return -1;}
    if ((after^v)&verifyMask(reg)) {
        l->FailureRegister=reg;l->FailureOperation=4;l->FailureWanted=v;
        l->FailureReadback=after;l->FailureMask=verifyMask(reg);return l->Error=-1;
    }
    return 0;
}
static int updateReg(BC250_HDMI_LINK *l,uint32_t reg,uint32_t mask,uint32_t value)
{
    uint32_t v;
    const BC250_HDMI_FIELD_POLICY* p=Bc250HdmiLinkPolicy(reg);
    if(p && p->PacketData) {
        if(mask==UINT32_MAX)return writeReg(l,reg,value);
        l->FailureRegister=reg;l->FailureOperation=2;return l->Error=-1;
    }
    if (l->Error || readReg(l,reg,&v)) return l->Error=-1;
    // A read-modify-write must not replay a previously observed command bit.
    if (Bc250HdmiLinkPolicy(reg)) v &= ~Bc250HdmiLinkPolicy(reg)->Command;
    return writeReg(l,reg,(v&~mask)|(value&mask));
}
static int atomOwned(void *p) { BC250_HDMI_LINK *l=p; return !l->Error && l->Io.Owned(l->Io.Context); }
static int atomAllowed(void *p,uint32_t reg,int write)
{
    (void)p;
    // The inherited PHY must remain powered: transmitter ENABLE is admitted
    // only while it takes the firmware's already-enabled branch. No PHY, PLL,
    // clock, SMU or transmitter-disable register can be written through ATOM.
    if (reg==R(DIG_BE_CNTL) || reg==R(DIG_FE_CNTL) || reg==R(TMDS_CTL_BITS) ||
        reg==R(DIG_CLOCK_PATTERN) || reg==R(HDMI_CONTROL) || reg==R(HDMI_VBI_PACKET_CONTROL)) return 1;
    return !write && reg==R(DIG_BE_EN_CNTL);
}
static int atomRead(void *p,uint32_t reg,uint32_t *v)
{
    int result=readReg(p,reg,v);const BC250_HDMI_FIELD_POLICY* policy=Bc250HdmiLinkPolicy(reg);
    // Do not replay a command sampled during an ATOM read-modify-write. An
    // intentional command remains possible through its explicit write value.
    if(!result && policy)*v&=~policy->Command;
    return result;
}
static int atomWrite(void *p,uint32_t reg,uint32_t v) { return writeReg(p,reg,v); }
static uint64_t atomNow(void *p) { BC250_HDMI_LINK *l=p; return l->Io.NowUs(l->Io.Context); }
static int atomDelay(void *p,uint32_t us) { BC250_HDMI_LINK *l=p; return l->Io.DelayUs(l->Io.Context,us); }

struct dc_info_packet { bool valid; uint8_t hb0,hb1,hb2,hb3; uint8_t sb[32]; };
struct dc_crtc_timing {
    enum dc_pixel_encoding pixel_encoding;
    enum dc_color_depth display_color_depth;
    enum dc_aspect_ratio aspect_ratio;
    enum dc_timing_3d_format timing_3d_format;
    unsigned vic,rid,fr_index,hdmi_vic,h_total,v_total;
    unsigned h_border_left,h_border_right,v_border_top,v_border_bottom;
    struct { unsigned LTE_340MCSC_SCRAMBLE; } flags;
};
struct dc_stream_state {
    struct dc_info_packet avi_infopacket;
    struct dc_crtc_timing timing;
    enum dc_color_space output_color_space;
    enum display_content_type content_type;
    struct { unsigned tf; } out_transfer_func;
    unsigned qs_bit;
};
struct pipe_ctx { struct dc_stream_state *stream; };
// HDMI AVI header constants (HDMI 1.4b AVI InfoFrame / CTA-861); no Linux code.
#define HDMI_INFOFRAME_TYPE_AVI 0x82
#define HDMI_AVI_INFOFRAME_SIZE 13
#define TRANSFER_FUNCTION_GAMMA22 0
struct dcn10_stream_encoder { BC250_HDMI_LINK *link; };
static void fields(struct dcn10_stream_encoder *e,uint32_t reg,unsigned count,...)
{
    va_list args; unsigned i; uint32_t mask=0,value=0;
    va_start(args,count);
    for (i=0;i<count;++i) { uint32_t m=va_arg(args,uint32_t),v=va_arg(args,uint32_t); mask|=m; value|=field(m,v); }
    va_end(args);
    (void)updateReg(e->link,reg,mask,value);
}
static void waitField(struct dcn10_stream_encoder *e,uint32_t reg,uint32_t mask,uint32_t value,unsigned us,unsigned count)
{
    uint32_t v=0; unsigned i;
    if (e->link->Error) return; // preserve the first failed operation
    for (i=0;i<count && !e->link->Error;++i) {
        if (readReg(e->link,reg,&v)) return;
        if ((v&mask)==field(mask,value)) return;
        if (e->link->Io.DelayUs(e->link->Io.Context,us)) break;
    }
    e->link->FailureRegister=reg;e->link->FailureOperation=5;
    e->link->FailureWanted=field(mask,value);e->link->FailureReadback=v;
    e->link->FailureMask=mask;e->link->Error=-1;
}
#define PAIR(r,f,v) M(r,f),(uint32_t)(v)
#define REG_UPDATE(r,f,v) fields(enc1,R(r),1,PAIR(r,f,v))
#define REG_UPDATE_2(r,f,v,g,w) fields(enc1,R(r),2,PAIR(r,f,v),PAIR(r,g,w))
#define REG_UPDATE_3(r,f,v,g,w,h,x) fields(enc1,R(r),3,PAIR(r,f,v),PAIR(r,g,w),PAIR(r,h,x))
#define REG_UPDATE_6(r,f,v,g,w,h,x,i,y,j,z,k,a) fields(enc1,R(r),6,PAIR(r,f,v),PAIR(r,g,w),PAIR(r,h,x),PAIR(r,i,y),PAIR(r,j,z),PAIR(r,k,a))
// AMD uses REG_SET_4 only for the complete packet header, whose four bytes cover the register.
#define REG_SET_4(r,init,f,v,g,w,h,x,i,y) fields(enc1,R(r),4,PAIR(r,f,v),PAIR(r,g,w),PAIR(r,h,x),PAIR(r,i,y))
#define REG_WRITE(r,v) ((void)writeReg(enc1->link,R(r),(uint32_t)(v)))
#define REG_WAIT(r,f,v,us,n) waitField(enc1,R(r),M(r,f),(v),(us),(n))
#define ASSERT(ok) do { if (!(ok)) { enc1->link->Error=-1; return; } } while(0)
#define DC_LOG_DEBUG(...) ((void)0)
#define DC_LOG_WARNING(...) ((void)0)
#define HDMI_CLOCK_CHANNEL_RATE_MORE_340M 340000
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4244) // AMD packs checked bounded values into HDMI bitfields.
#endif
#include "hdmi_link_stream.inc"
#include "hdmi_link_avi.inc"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

void Bc250HdmiLinkAvi(unsigned width,unsigned height,unsigned htotal,unsigned vtotal,uint8_t header[4],uint8_t payload[32])
{
    struct dc_stream_state stream={0}; struct pipe_ctx pipe={&stream}; struct dc_info_packet packet={0};
    stream.timing.pixel_encoding=PIXEL_ENCODING_RGB;
    stream.timing.display_color_depth=COLOR_DEPTH_888;
    stream.timing.h_total=htotal; stream.timing.v_total=vtotal;
    stream.timing.aspect_ratio=width*9u==height*16u?ASPECT_RATIO_16_9:
        (width*3u==height*4u?ASPECT_RATIO_4_3:ASPECT_RATIO_NO_DATA);
    stream.timing.timing_3d_format=TIMING_3D_FORMAT_NONE;
    stream.output_color_space=COLOR_SPACE_SRGB;
    stream.content_type=DISPLAY_CONTENT_TYPE_GRAPHICS;
    // No VIC is inferred from resolution: 1080p75 is not CTA VIC16 (60 Hz).
    // RGB default quantization at VIC0 is full range. No new color transform.
    set_avi_info_frame(&packet,&pipe);
    header[0]=packet.hb0; header[1]=packet.hb1; header[2]=packet.hb2; header[3]=0;
    memcpy(payload,packet.sb,32);
}

static int restoreFailure(BC250_HDMI_LINK* l,uint32_t op,uint32_t reg,uint32_t wanted,
    uint32_t readback,uint32_t mask,uint32_t readable)
{
    BC250_HDMI_RESTORE_DETAIL* d=&l->RestoreFailure;
    if(!l->RestoreRegister)l->RestoreRegister=reg;
    if(!d->Valid) {
        d->Valid=1;d->Operation=op;d->Register=reg;d->Wanted=wanted;
        d->Readback=readback;d->Mask=mask;d->ReadbackValid=readable;
    }
    return -1;
}
static int restoreRead(BC250_HDMI_LINK* l,uint32_t reg,uint32_t* value,uint32_t wanted,uint32_t mask)
{
    if(!l->Io.Owned(l->Io.Context))return restoreFailure(l,2,reg,wanted,0,mask,0);
    if(l->Io.Read(l->Io.Context,reg,value))return restoreFailure(l,1,reg,wanted,0,mask,0);
    return 0;
}
static int restoreFields(BC250_HDMI_LINK* l,uint32_t reg,uint32_t mask,uint32_t value,
    uint32_t commands,uint32_t* after)
{
    const BC250_HDMI_FIELD_POLICY* p=Bc250HdmiLinkPolicy(reg);
    uint32_t before,want;
    if(!p || (mask&~p->Configuration) || (commands&~p->Command))
        return restoreFailure(l,2,reg,value,0,mask,0);
    if(restoreRead(l,reg,&before,value,mask))return -1;
    // Restore saved configuration only. Reserved/unknown bits come from the
    // CURRENT register, never from the old journal. Commands are explicit.
    want=(before&~(mask|p->Command|p->Status))|(value&mask)|commands;
    if((before^want)&p->Unknown)return restoreFailure(l,6,reg,want,before,p->Unknown,1);
    if(l->Io.Write(l->Io.Context,reg,want)) {
        uint32_t valueAfter=0;
        uint32_t readable=l->Io.Owned(l->Io.Context) && !l->Io.Read(l->Io.Context,reg,&valueAfter);
        return restoreFailure(l,3,reg,want,valueAfter,verifyMask(reg),readable);
    }
    if(restoreRead(l,reg,after,want,verifyMask(reg)))return -1;
    if((*after^want)&verifyMask(reg))return restoreFailure(l,4,reg,want,*after,verifyMask(reg),1);
    return 0;
}

static int stopPacket0(BC250_HDMI_LINK* l)
{
    uint32_t after;int failed=0;
    // AMD enc2_stream_encoder_stop_hdmi_info_packets stops CONT/SEND and LINE.
    // This owner admits inactive packet 0 only, so restore its controls rather
    // than inventing a readable old-data image. Other packet slots are untouched.
    if(restoreFields(l,R(HDMI_GENERIC_PACKET_CONTROL0),
        M(HDMI_GENERIC_PACKET_CONTROL0,HDMI_GENERIC0_CONT)|M(HDMI_GENERIC_PACKET_CONTROL0,HDMI_GENERIC0_SEND),
        0,0,&after))failed=1;
    if(restoreFields(l,R(HDMI_GENERIC_PACKET_CONTROL1),
        M(HDMI_GENERIC_PACKET_CONTROL1,HDMI_GENERIC0_LINE),0,0,&after))failed=1;
    return failed?-1:0;
}

int Bc250HdmiLinkRestore(BC250_HDMI_LINK *l)
{
    unsigned i; int result=0; uint32_t after;
    if (!l || !l->Dirty) return 0;
    l->Restoring=1; l->RestoreRegister=0;
    if(l->PacketDirty && stopPacket0(l))result=-1;
    // Restore configuration, including the original bank selector, clock and
    // power. Try all entries but retain debt after any failed stop operation.
    for (i=l->Count;i>0;--i) {
        uint32_t r=l->Offset[i-1], want=l->Before[i-1];
        const BC250_HDMI_FIELD_POLICY *policy=Bc250HdmiLinkPolicy(r);
        if (!policy) {restoreFailure(l,2,r,want,0,0,0);result=-1;continue;}
        if(policy->PacketData) {restoreFailure(l,2,r,want,0,0,0);result=-1;continue;}
        if (r==R(AFMT_VBI_PACKET_CONTROL1)) continue; // do not replay update commands
        if (restoreFields(l,r,policy->Configuration,want,0,&after)) {
            result=-1;
        }
    }
    l->Restoring=0; l->Active=0;
    if (!result) { l->Dirty=0; l->Count=0; l->PacketDirty=0; l->PacketAdmitted=0; }
    return result;
}

static int streamFailure(BC250_HDMI_LINK* l,uint32_t op,uint32_t reg,uint32_t wanted,
    uint32_t readback,uint32_t mask,uint32_t readable)
{
    BC250_HDMI_RESTORE_DETAIL* d=&l->StreamFailure;
    if(!d->Valid) {
        d->Valid=1;d->Operation=op;d->Register=reg;d->Wanted=wanted;
        d->Readback=readback;d->Mask=mask;d->ReadbackValid=readable;
    }
    return -1;
}
static int streamResult(BC250_HDMI_LINK* l)
{
    if(!l->Error)return 0;
    return streamFailure(l,l->FailureOperation,l->FailureRegister,l->FailureWanted,
        l->FailureReadback,l->FailureMask,
        l->FailureOperation==4 || l->FailureOperation==5 || l->FailureOperation==6);
}

static int retirePacket0(BC250_HDMI_LINK* l)
{
    uint32_t control,line,enabled,otg;
    const uint32_t controlMask=M(HDMI_GENERIC_PACKET_CONTROL0,HDMI_GENERIC0_CONT)|
        M(HDMI_GENERIC_PACKET_CONTROL0,HDMI_GENERIC0_SEND);
    const uint32_t lineMask=M(HDMI_GENERIC_PACKET_CONTROL1,HDMI_GENERIC0_LINE);
    if(readReg(l,R(HDMI_GENERIC_PACKET_CONTROL0),&control))return -1;
    if(!(control&controlMask))return 0;
    // PnP stop preserves the live picture, but frees the old software journal.
    // The cold modeset owner must stop TX and OTG before replacing that AVI.
    if(readReg(l,R(DIG_BE_EN_CNTL),&enabled) ||
       readReg(l,BC250_REG_DMU_OTG0_OTG_CONTROL/4u,&otg))return -1;
    if(enabled&M(DIG_BE_EN_CNTL,DIG_ENABLE)) {
        l->Error=-1;return streamFailure(l,4,R(DIG_BE_EN_CNTL),0,enabled,
            M(DIG_BE_EN_CNTL,DIG_ENABLE),1);
    }
    if(otg&OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK) {
        l->Error=-1;return streamFailure(l,4,BC250_REG_DMU_OTG0_OTG_CONTROL/4u,0,otg,
            OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK,1);
    }
    if(readReg(l,R(HDMI_GENERIC_PACKET_CONTROL1),&line))return -1;
    // AMD enc2_stream_encoder_stop_hdmi_info_packets clears CONT/SEND and LINE.
    // Limit it to our slot 0. Journal the stopped baseline: recovery must rebuild
    // AVI from its known mode, never enable old controls over replaced packet RAM.
    if(saveConfiguration(l,R(HDMI_GENERIC_PACKET_CONTROL0),control&~controlMask) ||
       saveConfiguration(l,R(HDMI_GENERIC_PACKET_CONTROL1),line&~lineMask))return -1;
    if(updateReg(l,R(HDMI_GENERIC_PACKET_CONTROL0),controlMask,0) ||
       updateReg(l,R(HDMI_GENERIC_PACKET_CONTROL1),lineMask,0))return -1;
    return 0;
}

int Bc250HdmiStreamProgram(BC250_HDMI_LINK *l,const BC250_ATOM_CALLBACKS *io,uint32_t pixelKHz,
    unsigned width,unsigned height,unsigned htotal,unsigned vtotal)
{
    struct dcn10_stream_encoder stream={l};struct dcn10_stream_encoder *enc1=&stream;
    struct dc_crtc_timing timing={0};struct dc_info_packet packet={0};
    uint8_t hdr[4];uint32_t be,peer,mask,wanted;
    if(!l)return -1;
    if(!l->StreamFailure.Valid)l->StreamStage=1;
    if(l->Dirty)return streamFailure(l,8,0,0,l->Dirty,UINT32_MAX,1);
    if(!io || !io->Owned || !io->Read || !io->Write || !io->NowUs || !io->DelayUs ||
       pixelKHz<25000 || pixelKHz>300000 || !width || !height ||
       width>=htotal || height>=vtotal || htotal>65534 || vtotal>65534)
        return streamFailure(l,7,0,0,0,0,0);
    memset(l,0,sizeof(*l));l->Io=*io;
    // Only the route admitted by the outer link owner. This function cannot
    // convert DVI/DP or enable a transmitter on its own.
    l->StreamStage=2;
    if(readReg(l,R(DIG_BE_CNTL),&be) ||
       readReg(l,BC250_REG_DMU_DIG1_DIG_BE_EN_CNTL/4u,&peer))return streamResult(l);
    mask=M(DIG_BE_CNTL,DIG_MODE)|M(DIG_BE_CNTL,DIG_FE_SOURCE_SELECT);
    wanted=field(M(DIG_BE_CNTL,DIG_MODE),3)|field(M(DIG_BE_CNTL,DIG_FE_SOURCE_SELECT),1);
    if((be&mask)!=wanted) {l->Error=-1;return streamFailure(l,4,R(DIG_BE_CNTL),wanted,be,mask,1);}
    if(peer&DIG1_DIG_BE_EN_CNTL__DIG_ENABLE_MASK) {
        l->Error=-1;return streamFailure(l,4,BC250_REG_DMU_DIG1_DIG_BE_EN_CNTL/4u,
            0,peer,DIG1_DIG_BE_EN_CNTL__DIG_ENABLE_MASK,1);
    }
    if(retirePacket0(l))return streamResult(l);
    if(admitPacket0(l,0))return streamResult(l);
    l->StreamStage=3;
    REG_UPDATE(HDMI_GC,HDMI_GC_AVMUTE,1);
    if(l->Error)return streamResult(l);
    l->StreamStage=4;
    timing.pixel_encoding=PIXEL_ENCODING_RGB;timing.display_color_depth=COLOR_DEPTH_888;
    hdmi_stream_registers(enc1,&timing,(int)pixelKHz);
    if(l->Error)return streamResult(l);
    l->StreamStage=5;
    (void)updateReg(l,BC250_REG_DMU_DIO_MEM_PWR_CTRL/4u,DIO_MEM_PWR_CTRL__HDMI0_MEM_PWR_FORCE_MASK,0);
    if(l->Error)return streamResult(l);
    l->StreamStage=6;
    Bc250HdmiLinkAvi(width,height,htotal,vtotal,hdr,packet.sb);
    packet.valid=true;packet.hb0=hdr[0];packet.hb1=hdr[1];packet.hb2=hdr[2];
    REG_UPDATE(HDMI_DB_CONTROL,HDMI_DB_DISABLE,1);
    enc2_update_hdmi_info_packet(enc1,0,&packet);
    // link_dpms programs stream attributes before transmitter enable. Keep
    // AVMUTE asserted until the outer owner has enabled TX and unblanked.
    if(!l->Error)l->Active=1;
    return streamResult(l);
}

int Bc250HdmiStreamMute(BC250_HDMI_LINK *l,int muted)
{
    uint32_t enabled,mask=M(DIG_BE_EN_CNTL,DIG_ENABLE)|M(DIG_BE_EN_CNTL,DIG_SYMCLK_BE_ON);
    if(!l)return -1;
    if(!l->StreamFailure.Valid)l->StreamStage=7;
    if(!l->Active || !l->Dirty || l->Error || (muted!=0 && muted!=1))
        return streamFailure(l,7,0,0,0,0,0);
    if(!muted) {
        if(readReg(l,R(DIG_BE_EN_CNTL),&enabled))return streamResult(l);
        if((enabled&mask)!=mask)return streamFailure(l,4,R(DIG_BE_EN_CNTL),mask,enabled,mask,1);
    }
    (void)updateReg(l,R(HDMI_GC),M(HDMI_GC,HDMI_GC_AVMUTE),muted?M(HDMI_GC,HDMI_GC_AVMUTE):0);
    return streamResult(l);
}

static int startCore(BC250_HDMI_LINK *l,const BC250_ATOM_CALLBACKS *io,const BC250_ATOM_IMAGE *image,
    BC250_ATOM_PLAN *plan,void *workspace,size_t workspaceSize,uint32_t pixelKHz,unsigned width,unsigned height,
    unsigned htotal,unsigned vtotal,BC250_HDMI_PREFLIGHT* shadow)
{
    const unsigned encoderRoot=offsetof(struct atom_master_list_of_command_functions_v2_1,digxencodercontrol)/2;
    const unsigned transmitterRoot=offsetof(struct atom_master_list_of_command_functions_v2_1,dig1transmittercontrol)/2;
    struct dig_encoder_stream_setup_parameters_v1_5 ep={0};
    struct dig_transmitter_control_ps_allocation_v1_6 tp={0};
    BC250_ATOM_TABLE table; BC250_ATOM_CALLBACKS atomIo={l,atomOwned,atomAllowed,atomRead,atomWrite,atomNow,atomDelay};
    BC250_ATOM_LIMITS limits={4096,2000,100000};
    struct dcn10_stream_encoder stream={l}; struct dcn10_stream_encoder *enc1=&stream;
    struct dc_crtc_timing timing={0}; struct dc_info_packet packet={0};
    uint8_t hdr[4]; uint32_t be,enable,peer,fe,control; unsigned root;
    uint64_t began,elapsed;
    if (!l || !io || !image || !plan || !workspace || workspaceSize<Bc250AtomContextSize() ||
        !io->Owned || !io->Read || !io->Write || !io->NowUs || !io->DelayUs || l->Dirty ||
        pixelKHz<25000 || pixelKHz>300000 || !width || !height || width>htotal || height>vtotal ||
        htotal>65534 || vtotal>65534) return -1;
    memset(l,0,sizeof(*l)); l->Io=*io;l->Shadow=shadow;
    began=io->NowUs(io->Context);
    // Full call-tree validation of both commands before even the AVMUTE write.
    for (root=0;root<2;++root) {
        unsigned index=root?transmitterRoot:encoderRoot;
        if (Bc250AtomGetTable(image,index,1,&table) || table.Format!=1 || table.Content!=(root?6:5) ||
            Bc250AtomPreflight(image,index,plan)) return l->Error=-1;
    }
    if (readReg(l,R(DIG_BE_CNTL),&be) || readReg(l,R(DIG_BE_EN_CNTL),&enable) ||
        readReg(l,BC250_REG_DMU_DIG1_DIG_BE_EN_CNTL/4u,&peer) || readReg(l,R(DIG_FE_CNTL),&fe) ||
        readReg(l,R(HDMI_CONTROL),&control)) return -1;
    if ((be&M(DIG_BE_CNTL,DIG_MODE))!=field(M(DIG_BE_CNTL,DIG_MODE),2) ||
        (be&M(DIG_BE_CNTL,DIG_FE_SOURCE_SELECT))!=field(M(DIG_BE_CNTL,DIG_FE_SOURCE_SELECT),1) ||
        !(enable&M(DIG_BE_EN_CNTL,DIG_ENABLE)) || !(enable&M(DIG_BE_EN_CNTL,DIG_SYMCLK_BE_ON)) ||
        (peer&DIG1_DIG_BE_EN_CNTL__DIG_ENABLE_MASK) || !(fe&M(DIG_FE_CNTL,DIG_SYMCLK_FE_ON)) ||
        (fe&(M(DIG_FE_CNTL,TMDS_PIXEL_ENCODING)|M(DIG_FE_CNTL,TMDS_COLOR_FORMAT))) ||
        (control&(M(HDMI_CONTROL,HDMI_DEEP_COLOR_ENABLE)|M(HDMI_CONTROL,HDMI_CLOCK_CHANNEL_RATE)|
                  M(HDMI_CONTROL,HDMI_DATA_SCRAMBLE_EN)))) return l->Error=-1;
    // PnP supplies the validated inherited OTG0 timing. No other active head.
    if (readReg(l,BC250_REG_DMU_OTG1_OTG_CONTROL/4u,&peer) || (peer&OTG1_OTG_CONTROL__OTG_MASTER_EN_MASK)) return l->Error=-1;
    // No packet RAM currently in use; this is the DVI branch only.
    if(admitPacket0(l,1))return -1;
    REG_UPDATE(HDMI_GC,HDMI_GC_AVMUTE,1);
    REG_UPDATE(DIG_BE_CNTL,DIG_MODE,3); // dcn10_link_encoder_setup, before stream attributes
    ep.digid=0; ep.action=ATOM_ENCODER_CMD_STREAM_SETUP; ep.digmode=3; ep.lanenum=4; ep.pclk_10khz=pixelKHz/10;
    if (Bc250AtomPreflight(image,encoderRoot,plan) ||
        Bc250AtomExecute(workspace,workspaceSize,image,encoderRoot,plan,&atomIo,&limits,&ep,sizeof(ep),&l->Atom))
        return l->Error=-1;
    limits.Instructions-=l->Atom.Instructions;
    limits.DelayUs-=l->Atom.DelayUs;
    timing.pixel_encoding=PIXEL_ENCODING_RGB; timing.display_color_depth=COLOR_DEPTH_888;
    hdmi_stream_registers(enc1,&timing,(int)pixelKHz);
    if(l->Error) return -1;
    // dcn201_init_hw: AFMT RAM must be powered before generic packet writes.
    (void)updateReg(l,BC250_REG_DMU_DIO_MEM_PWR_CTRL/4u,DIO_MEM_PWR_CTRL__HDMI0_MEM_PWR_FORCE_MASK,0);
    Bc250HdmiLinkAvi(width,height,htotal,vtotal,hdr,packet.sb);
    packet.valid=true; packet.hb0=hdr[0]; packet.hb1=hdr[1]; packet.hb2=hdr[2];
    REG_UPDATE(HDMI_DB_CONTROL,HDMI_DB_DISABLE,1);
    enc2_update_hdmi_info_packet(enc1,0,&packet);
    if(l->Error) return -1;
    // command_table2.c transmitter_control_v1_6, DCN201/DCE112 helpers:
    // DIGFE selection is zero (DCN configures the connection itself). PHY/HPD0
    // identify the sole admitted inherited route; no connector object override.
    tp.param.phyid=0; tp.param.action=ATOM_TRANSMITTER_ACTION_ENABLE;
    tp.param.mode_laneset.digmode=3; tp.param.lanenum=4; tp.param.symclk_10khz=pixelKHz/10;
    tp.param.hpdsel=1;
    elapsed=io->NowUs(io->Context)-began;
    if (!limits.Instructions || elapsed>=limits.ElapsedUs) return l->Error=-1;
    limits.ElapsedUs-=(uint32_t)elapsed;
    if (Bc250AtomPreflight(image,transmitterRoot,plan) ||
        Bc250AtomExecute(workspace,workspaceSize,image,transmitterRoot,plan,&atomIo,&limits,&tp,sizeof(tp),&l->Atom))
        return l->Error=-1;
    // link_set_dpms_on reapplies setup after transmitter_control (firmware may
    // write DVI mode). Restore the same FE connection; the PHY stayed enabled.
    REG_UPDATE_2(DIG_BE_CNTL,DIG_MODE,3,DIG_FE_SOURCE_SELECT,1);
    REG_UPDATE(HDMI_GC,HDMI_GC_AVMUTE,0);
    if (!l->Error) l->Active=1;
    return l->Error;
}

int Bc250HdmiLinkStart(BC250_HDMI_LINK* l,const BC250_ATOM_CALLBACKS* io,const BC250_ATOM_IMAGE* image,
    BC250_ATOM_PLAN* plan,void* workspace,size_t workspaceSize,uint32_t pixelKHz,unsigned width,unsigned height,
    unsigned htotal,unsigned vtotal)
{ return startCore(l,io,image,plan,workspace,workspaceSize,pixelKHz,width,height,htotal,vtotal,NULL); }

#define SHADOW_MAX_OPS 32768u
#define SHADOW_MAX_US 100000u
typedef struct HDMI_SHADOW {
    BC250_HDMI_LINK Link;
    BC250_HDMI_SNAPSHOT Registers;
    BC250_HDMI_PREFLIGHT* Result;
    uint64_t Clock;
    unsigned Ops,Failed;
    uint32_t PacketWords[9];
} HDMI_SHADOW;
size_t Bc250HdmiLinkShadowSize(void) { return sizeof(HDMI_SHADOW); }
static int shadowOwned(void* context)
{
    HDMI_SHADOW* s=context;
    if(s->Ops>=SHADOW_MAX_OPS || s->Clock>=SHADOW_MAX_US)s->Failed=1;
    return !s->Failed;
}
static int shadowAllowed(void* context,uint32_t reg,int write)
{ (void)context;return Bc250HdmiLinkAllowed(reg,write); }
static BC250_HDMI_SNAPSHOT_ENTRY* shadowFind(HDMI_SHADOW* s,uint32_t reg)
{
    unsigned i;for(i=0;i<s->Registers.Count;++i)if(s->Registers.Entries[i].Register==reg)return &s->Registers.Entries[i];
    s->Failed=1;return NULL;
}
static int shadowRead(void* context,uint32_t reg,uint32_t* value)
{
    HDMI_SHADOW* s=context;BC250_HDMI_SNAPSHOT_ENTRY* e;
    if(!shadowOwned(s))return -1;
    ++s->Ops;++s->Result->Reads;e=shadowFind(s,reg);
    if(!e || e->Status)return -1;
    *value=e->Value;return 0;
}
static int shadowWrite(void* context,uint32_t reg,uint32_t value)
{
    HDMI_SHADOW* s=context;BC250_HDMI_SNAPSHOT_ENTRY* e;
    const BC250_HDMI_FIELD_POLICY* p=Bc250HdmiLinkPolicy(reg);
    if(!shadowOwned(s) || !p)return -1;
    ++s->Ops;++s->Result->Writes;
    if(p->PacketData) {
        unsigned index=reg==R(AFMT_GENERIC_HDR)?0u:1u+reg-R(AFMT_GENERIC_0);
        if(index>=9 || !s->Link.PacketAdmitted)return -1;
        s->PacketWords[index]=value;return 0;
    }
    e=shadowFind(s,reg);if(!e || e->Status)return -1;
    // A sampled model follows intended configuration writes. Commands do not
    // latch, status does not become writable, and sampled status is preserved.
    // Conflict-clear is the generated packet helper's explicit acknowledgement;
    // model its completion only to inspect later field policies, not as proof
    // that hardware has completed the operation.
    // PacketWords holds intended writes only; it is not a hardware RAM image.
    if(reg==R(AFMT_VBI_PACKET_CONTROL) && (value&M(AFMT_VBI_PACKET_CONTROL,AFMT_GENERIC_CONFLICT_CLR)))
        e->Value&=~M(AFMT_VBI_PACKET_CONTROL,AFMT_GENERIC_CONFLICT);
    e->Value=(value&~(p->Command|p->Status))|(e->Value&p->Status);
    return 0;
}
static uint64_t shadowNow(void* context)
{ HDMI_SHADOW* s=context;++s->Clock;return s->Clock; }
static int shadowDelay(void* context,uint32_t us)
{
    HDMI_SHADOW* s=context;
    if(us>SHADOW_MAX_US || s->Clock>SHADOW_MAX_US-us) {s->Failed=1;return -1;}
    s->Clock+=us;s->Result->DelayUs+=us;return shadowOwned(s)?0:-1;
}
int Bc250HdmiLinkPreflight(const BC250_HDMI_SNAPSHOT* snapshot,
    const BC250_ATOM_IMAGE* image,BC250_ATOM_PLAN* plan,void* atomWorkspace,size_t atomBytes,
    void* shadowWorkspace,size_t shadowBytes,uint32_t pixelKHz,unsigned width,unsigned height,
    unsigned htotal,unsigned vtotal,BC250_HDMI_PREFLIGHT* result)
{
    HDMI_SHADOW* s=shadowWorkspace;
    BC250_ATOM_CALLBACKS io={s,shadowOwned,shadowAllowed,shadowRead,shadowWrite,shadowNow,shadowDelay};
    unsigned i;
    int status;
    if(!result)return -1;
    memset(result,0,sizeof(*result));result->Fatal=1;
    if(!snapshot || !snapshot->Complete || !snapshot->Count || snapshotRegister(snapshot->Count)!=UINT32_MAX || snapshotRegister(snapshot->Count-1)==UINT32_MAX ||
       snapshot->Count>BC250_HDMI_SNAPSHOT_REGS || !s || shadowBytes<sizeof(*s))return -1;
    for(i=0;i<snapshot->Count;++i)if(snapshot->Entries[i].Status ||
        snapshot->Entries[i].Register!=snapshotRegister(i))return -1;
    memset(s,0,sizeof(*s));s->Registers=*snapshot;s->Result=result;
    status=startCore(&s->Link,&io,image,plan,atomWorkspace,atomBytes,pixelKHz,width,height,htotal,vtotal,result);
    result->Atom=s->Link.Atom;
    result->Last.Register=s->Link.FailureRegister;result->Last.Operation=s->Link.FailureOperation;
    result->Last.Wanted=s->Link.FailureWanted;result->Last.Before=s->Link.FailureReadback;result->Last.Mask=s->Link.FailureMask;
    result->ElapsedUs=s->Clock;
    result->Fatal=status!=0 || s->Failed || result->Overflow;
    result->Complete=!result->Fatal;
    return result->Complete && !result->Count?0:-1;
}
