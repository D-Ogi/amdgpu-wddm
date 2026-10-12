// PROVENANCE: AMD DC (MIT), Linux v6.18 7d0a66e4. The original ACR tables,
// get_audio_clock_info, enc1_se_setup_hdmi_audio and HDMI DTO calculator are
// extracted without edits. Checked MMIO and fail-silent teardown are the shim.
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include "bc250_hdmi_audio.h"
#include "../kmd/display_io.h"
#include "../kmd/regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"

typedef unsigned long uint32_t;
enum dc_color_depth { COLOR_DEPTH_888, COLOR_DEPTH_121212, COLOR_DEPTH_161616 };
struct audio_clock_info { uint32_t pixel_clock_in_10khz, n_32khz, cts_32khz, n_44khz, cts_44khz, n_48khz, cts_48khz; };
struct azalia_clock_info { uint32_t audio_dto_phase, audio_dto_module; };
struct audio_crtc_info { enum dc_color_depth color_depth; uint32_t requested_pixel_clock_100Hz, calculated_pixel_clock_100Hz; };
struct stream_encoder { BC250_AZ_IO *io; BC250_DPAUDIO_STREAM_RESULT *result; unsigned long stream; long error; };
#define dcn10_stream_encoder stream_encoder
#define DCN10STRENC_FROM_STRENC(e) (e)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define DC_LOG_HW_AUDIO(...) ((void)0)
#define HDMI_REGS(X) \
    X(HDMI_AUDIO_PACKET_CONTROL) X(HDMI_ACR_PACKET_CONTROL) X(HDMI_ACR_32_0) X(HDMI_ACR_32_1) \
    X(HDMI_ACR_44_0) X(HDMI_ACR_44_1) X(HDMI_ACR_48_0) X(HDMI_ACR_48_1) \
    X(AFMT_AUDIO_PACKET_CONTROL) X(AFMT_AUDIO_PACKET_CONTROL2) X(AFMT_60958_0) X(AFMT_60958_1) X(AFMT_60958_2) \
    X(AFMT_CNTL) X(AFMT_AUDIO_SRC_CONTROL) X(HDMI_INFOFRAME_CONTROL0) X(HDMI_INFOFRAME_CONTROL1) X(AFMT_INFOFRAME_CONTROL0)
#define REG_PAIR(r) static const unsigned long reg_##r[2] = { BC250_REG_DMU_DIG0_##r, BC250_REG_DMU_DIG1_##r };
HDMI_REGS(REG_PAIR)
#undef REG_PAIR
#define MASK(r,f) DIG0_##r##__##f##_MASK
#define PAIR(r,f,v) MASK(r,f),(unsigned long)(v)

static long update(BC250_AZ_IO *io, BC250_DPAUDIO_STREAM_RESULT *result, unsigned long offset,
                    unsigned long mask, unsigned long value)
{
    unsigned long before = 0, after = 0, want, check = mask;
    long status = Bc250AzRead(io, offset, &before);
    want = (before & ~mask) | (value & mask);
    if (status >= 0) {
        status = Bc250AzWrite(io, offset, want);
        if (status >= 0) result->Writes++;
    }
    if (status >= 0) status = Bc250AzRead(io, offset, &after);
    // Update strobes clear themselves; do not require their read-back to be 1.
    if (offset == reg_AFMT_AUDIO_PACKET_CONTROL[0] || offset == reg_AFMT_AUDIO_PACKET_CONTROL[1])
        check &= ~MASK(AFMT_AUDIO_PACKET_CONTROL, AFMT_60958_CS_UPDATE);
    if (offset == reg_AFMT_INFOFRAME_CONTROL0[0] || offset == reg_AFMT_INFOFRAME_CONTROL0[1])
        check &= ~MASK(AFMT_INFOFRAME_CONTROL0, AFMT_AUDIO_INFO_UPDATE);
    if (status >= 0 && ((after ^ want) & check)) status = BC250_AZ_STATUS_MISMATCH;
    if (status < 0 && result->Status >= 0) {
        result->Status = status;
        result->Step = BC250_DPAUDIO_STEP_HDMI_REGISTER; // offset below identifies the exact HDMI step
        result->MismatchOffset = offset;
        result->MismatchExpected = want & check;
        result->MismatchActual = after & check;
    }
    return status;
}
static void fields(struct stream_encoder *e, unsigned long reg, unsigned int count, ...)
{
    va_list args;
    unsigned int i;
    unsigned long mask = 0, value = 0;
    if (e->error < 0) return;
    va_start(args, count);
    for (i = 0; i < count; i++) {
        unsigned long part = va_arg(args, unsigned long);
        unsigned long field = va_arg(args, unsigned long);
        mask |= part; value |= Bc250DispField(part, field);
    }
    va_end(args);
    e->error = update(e->io, e->result, reg, mask, value);
}
#define REG_UPDATE(r,f,v) fields(enc1,reg_##r[enc1->stream],1,PAIR(r,f,v))
#define REG_UPDATE_2(r,f,v,g,w) fields(enc1,reg_##r[enc1->stream],2,PAIR(r,f,v),PAIR(r,g,w))
#define REG_UPDATE_3(r,f,v,g,w,h,x) fields(enc1,reg_##r[enc1->stream],3,PAIR(r,f,v),PAIR(r,g,w),PAIR(r,h,x))
#define REG_UPDATE_6(r,f,v,g,w,h,x,i,y,j,z,k,a) fields(enc1,reg_##r[enc1->stream],6,PAIR(r,f,v),PAIR(r,g,w),PAIR(r,h,x),PAIR(r,i,y),PAIR(r,j,z),PAIR(r,k,a))
#include "hdmi_audio.inc"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4100) // AMD DTO calculator does not use the requested clock
#endif
#include "hdmi_dto.inc"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

int bc250_hdmi_audio_clock(unsigned long clock, unsigned long n[3], unsigned long cts[3])
{
    struct audio_clock_info info;
    if (!n || !cts || clock < 250000 || clock > 3000000) return 0;
    get_audio_clock_info(COLOR_DEPTH_888, clock, clock, &info);
    n[0] = info.n_32khz; n[1] = info.n_44khz; n[2] = info.n_48khz;
    cts[0] = info.cts_32khz; cts[1] = info.cts_44khz; cts[2] = info.cts_48khz;
    return 1;
}

long bc250_hdmi_audio_enable(BC250_AZ_IO *io, unsigned long stream, unsigned long clock,
                              BC250_DPAUDIO_STREAM_RESULT *result)
{
    struct stream_encoder encoder;
    struct audio_crtc_info timing;
    struct azalia_clock_info dto;
    long status;
    memset(result, 0, sizeof(*result));
    if (stream >= 2 || clock < 250000 || clock > 3000000) return result->Status = BC250_AZ_STATUS_INVALID_PARAMETER;
    encoder.io = io; encoder.result = result; encoder.stream = stream; encoder.error = 0;
    timing.color_depth = COLOR_DEPTH_888;
    timing.requested_pixel_clock_100Hz = timing.calculated_pixel_clock_100Hz = clock;
    get_azalia_clock_info_hdmi(clock, clock, &dto);
#define STEP(r,m,v) do { status = update(io,result,r,m,v); if (status < 0) return status; } while (0)
    // dcn201_init_hw powers AFMT RAM; change only this encoder's named field.
    STEP(BC250_REG_DMU_DIO_MEM_PWR_CTRL, stream ? DIO_MEM_PWR_CTRL__HDMI1_MEM_PWR_FORCE_MASK :
        DIO_MEM_PWR_CTRL__HDMI0_MEM_PWR_FORCE_MASK, 0);
    // dce_aud_wall_dto_setup: select OTG0 and DTO0 BEFORE module and phase.
    STEP(BC250_REG_DMU_DCCG_AUDIO_DTO_SOURCE, DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO0_SOURCE_SEL_MASK |
        DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO_SEL_MASK, 0);
    STEP(BC250_REG_DMU_DCCG_AUDIO_DTO0_MODULE, DCCG_AUDIO_DTO0_MODULE__DCCG_AUDIO_DTO0_MODULE_MASK, dto.audio_dto_module);
    STEP(BC250_REG_DMU_DCCG_AUDIO_DTO0_PHASE, DCCG_AUDIO_DTO0_PHASE__DCCG_AUDIO_DTO0_PHASE_MASK, dto.audio_dto_phase);
    result->DtoModule = dto.audio_dto_module; result->DtoPhase = dto.audio_dto_phase;
    STEP(reg_AFMT_CNTL[stream], MASK(AFMT_CNTL, AFMT_AUDIO_CLOCK_EN), MASK(AFMT_CNTL, AFMT_AUDIO_CLOCK_EN));
    enc1_se_setup_hdmi_audio(&encoder, &timing);
    if (encoder.error < 0) return encoder.error;
    // enc1_se_audio_setup. This path advertises stereo LPCM only, so channel
    // allocation is FL/FR even if the sink has more speakers.
    STEP(reg_AFMT_AUDIO_SRC_CONTROL[stream], MASK(AFMT_AUDIO_SRC_CONTROL, AFMT_AUDIO_SRC_SELECT),
        Bc250DispField(MASK(AFMT_AUDIO_SRC_CONTROL, AFMT_AUDIO_SRC_SELECT), stream));
    STEP(reg_AFMT_AUDIO_PACKET_CONTROL2[stream], MASK(AFMT_AUDIO_PACKET_CONTROL2, AFMT_AUDIO_CHANNEL_ENABLE),
        Bc250DispField(MASK(AFMT_AUDIO_PACKET_CONTROL2, AFMT_AUDIO_CHANNEL_ENABLE), 3));
    // Audio subset of enc1_stream_encoder_hdmi_set_stream_attribute. AVI and
    // video encoding remain GOP-owned. AFMT/Azalia forms the audio InfoFrame;
    // this is not a software generic-packet payload.
    STEP(reg_HDMI_INFOFRAME_CONTROL1[stream], MASK(HDMI_INFOFRAME_CONTROL1, HDMI_AUDIO_INFO_LINE),
        Bc250DispField(MASK(HDMI_INFOFRAME_CONTROL1, HDMI_AUDIO_INFO_LINE), 2));
    STEP(reg_AFMT_INFOFRAME_CONTROL0[stream], MASK(AFMT_INFOFRAME_CONTROL0, AFMT_AUDIO_INFO_UPDATE),
        MASK(AFMT_INFOFRAME_CONTROL0, AFMT_AUDIO_INFO_UPDATE));
    STEP(reg_HDMI_INFOFRAME_CONTROL0[stream], MASK(HDMI_INFOFRAME_CONTROL0, HDMI_AUDIO_INFO_SEND),
        MASK(HDMI_INFOFRAME_CONTROL0, HDMI_AUDIO_INFO_SEND));
    // enc1_se_audio_mute_control(false), last before exposing the endpoint.
    STEP(reg_AFMT_AUDIO_PACKET_CONTROL[stream], MASK(AFMT_AUDIO_PACKET_CONTROL, AFMT_AUDIO_SAMPLE_SEND),
        MASK(AFMT_AUDIO_PACKET_CONTROL, AFMT_AUDIO_SAMPLE_SEND));
#undef STEP
    return 0;
}

long bc250_hdmi_audio_disable(BC250_AZ_IO *io, unsigned long stream, BC250_DPAUDIO_STREAM_RESULT *result)
{
    memset(result, 0, sizeof(*result));
    if (stream >= 2) return result->Status = BC250_AZ_STATUS_INVALID_PARAMETER;
    // Deviation: all teardown writes run after a failure; mute first, suppress
    // InfoFrames/ACR, then turn the AFMT clock off. Never touch DP_SEC registers.
    (void)update(io, result, reg_AFMT_AUDIO_PACKET_CONTROL[stream], MASK(AFMT_AUDIO_PACKET_CONTROL, AFMT_AUDIO_SAMPLE_SEND), 0);
    (void)update(io, result, reg_HDMI_INFOFRAME_CONTROL0[stream], MASK(HDMI_INFOFRAME_CONTROL0, HDMI_AUDIO_INFO_SEND), 0);
    (void)update(io, result, reg_HDMI_ACR_PACKET_CONTROL[stream], MASK(HDMI_ACR_PACKET_CONTROL, HDMI_ACR_AUTO_SEND), 0);
    (void)update(io, result, reg_AFMT_CNTL[stream], MASK(AFMT_CNTL, AFMT_AUDIO_CLOCK_EN), 0);
    return result->Status;
}
