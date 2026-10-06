// DisplayPort audio, steps 0 and 1, the part with no Windows in it (dpaudio_seq.h says what is here and why).
#define BC250_REGS_WITH_AUDIO_TABLES
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"      // field masks only; every offset and index comes from regs.generated.h
#include "dpaudio_seq.h"

// The two endpoints share one register layout. The driver takes every field from the ENDPOINT0 names; these checks
// make the build fail if the ENDPOINT1 names of the fields it touches ever say otherwise.
#define AZ0(reg, field) AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_##reg##__##field
#define AZ1(reg, field) AZF0ENDPOINT1_AZALIA_F0_CODEC_PIN_CONTROL_##reg##__##field
#define AZ_SAME(reg, field) typedef char Bc250AzSame_##reg##_##field[(AZ0(reg, field##_MASK) == AZ1(reg, field##_MASK) && \
                                                                    AZ0(reg, field##__SHIFT) == AZ1(reg, field##__SHIFT)) ? 1 : -1]
AZ_SAME(HOT_PLUG_CONTROL, CLOCK_GATING_DISABLE);
AZ_SAME(HOT_PLUG_CONTROL, AUDIO_ENABLED);
AZ_SAME(CHANNEL_SPEAKER, SPEAKER_ALLOCATION);
AZ_SAME(CHANNEL_SPEAKER, HDMI_CONNECTION);
AZ_SAME(CHANNEL_SPEAKER, DP_CONNECTION);
AZ_SAME(CHANNEL_SPEAKER, EXTRA_CONNECTION_INFO);
AZ_SAME(CHANNEL_SPEAKER, LFE_PLAYBACK_LEVEL);
AZ_SAME(RESPONSE_HBR, HBR_CAPABLE);
AZ_SAME(RESPONSE_LIPSYNC, VIDEO_LIPSYNC);
AZ_SAME(RESPONSE_LIPSYNC, AUDIO_LIPSYNC);
AZ_SAME(UNSOLICITED_RESPONSE, ENABLE);

#define HPC_CLOCK_GATING_DISABLE AZ0(HOT_PLUG_CONTROL, CLOCK_GATING_DISABLE_MASK)
#define HPC_AUDIO_ENABLED AZ0(HOT_PLUG_CONTROL, AUDIO_ENABLED_MASK)

static unsigned long SetField(unsigned long Value, unsigned long Mask, unsigned long Shift, unsigned long Field)
{
    return (Value & ~Mask) | ((Field << Shift) & Mask);
}
#define SET(v, reg, field, x) SetField((v), AZ0(reg, field##_MASK), AZ0(reg, field##__SHIFT), (x))

static int InTable(const unsigned long* Table, unsigned long Count, unsigned long Value)
{
    unsigned long low = 0, high = Count;

    while (low < high) {
        unsigned long middle = low + (high - low) / 2;
        if (Table[middle] == Value) return 1;
        if (Table[middle] < Value) low = middle + 1; else high = middle;
    }
    return 0;
}

// ---- the checked accessors ---------------------------------------------------------------------------------------

static const unsigned long g_EndpointIndex[BC250_DPAUDIO_ENDPOINTS] = {
    BC250_REG_DMU_AZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX, BC250_REG_DMU_AZF0ENDPOINT1_AZALIA_F0_CODEC_ENDPOINT_INDEX };
static const unsigned long g_EndpointData[BC250_DPAUDIO_ENDPOINTS] = {
    BC250_REG_DMU_AZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_DATA, BC250_REG_DMU_AZF0ENDPOINT1_AZALIA_F0_CODEC_ENDPOINT_DATA };

int Bc250AzReadAllowed(unsigned long Offset)
{
    return (Offset & 3) == 0 && InTable(g_MmioAudioAllow, BC250_MMIO_AUDIO_ALLOW_COUNT, Offset);
}

int Bc250AzWriteAllowed(unsigned long Offset)
{
    return (Offset & 3) == 0 && InTable(g_MmioAudioWriteAllow, BC250_MMIO_AUDIO_WRITE_ALLOW_COUNT, Offset);
}

long Bc250AzRead(BC250_AZ_IO* Io, unsigned long Offset, unsigned long* Value)
{
    *Value = 0;
    if (!Bc250AzReadAllowed(Offset)) { Io->Refusals++; return BC250_AZ_STATUS_ACCESS_DENIED; }
    return Io->Read(Io->Context, Offset, Value);
}

// The one place a register write leaves this file. Counted by the caller, which knows what the write was for.
static long CheckedWrite(BC250_AZ_IO* Io, unsigned long Offset, unsigned long Value)
{
    if (!Bc250AzWriteAllowed(Offset)) { Io->Refusals++; return BC250_AZ_STATUS_ACCESS_DENIED; }
    return Io->Write(Io->Context, Offset, Value);
}

long Bc250AzWrite(BC250_AZ_IO* Io, unsigned long Offset, unsigned long Value)
{
    long status = CheckedWrite(Io, Offset, Value);

    if (status >= 0) Io->DirectWrites++;
    return status;
}

// write_indirect_azalia_reg / read_indirect_azalia_reg (dce_audio.c:55-84): the index into the endpoint's INDEX
// register (AZALIA_ENDPOINT_REG_INDEX, mask 0x3FFF; every index on the tables fits), then the DATA register. The
// pair is not atomic in hardware: dpaudio.c holds its spin lock across every call of this file.
long Bc250AzIndirectRead(BC250_AZ_IO* Io, unsigned long Endpoint, unsigned long Index, unsigned long* Value)
{
    long status;

    *Value = 0;
    if (Endpoint >= BC250_DPAUDIO_ENDPOINTS) { Io->Refusals++; return BC250_AZ_STATUS_INVALID_PARAMETER; }
    if (!InTable(g_AzIxReadAllow, BC250_AZ_IX_READ_ALLOW_COUNT, Index)) { Io->Refusals++; return BC250_AZ_STATUS_ACCESS_DENIED; }
    status = CheckedWrite(Io, g_EndpointIndex[Endpoint],
                          Index & AZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX__AZALIA_ENDPOINT_REG_INDEX_MASK);
    if (status < 0) return status;
    status = Bc250AzRead(Io, g_EndpointData[Endpoint], Value);
    if (status >= 0) Io->IndirectReads++;
    return status;
}

long Bc250AzIndirectWrite(BC250_AZ_IO* Io, unsigned long Endpoint, unsigned long Index, unsigned long Value)
{
    long status;

    if (Endpoint >= BC250_DPAUDIO_ENDPOINTS) { Io->Refusals++; return BC250_AZ_STATUS_INVALID_PARAMETER; }
    if (!InTable(g_AzIxWriteAllow, BC250_AZ_IX_WRITE_ALLOW_COUNT, Index)) { Io->Refusals++; return BC250_AZ_STATUS_ACCESS_DENIED; }
    status = CheckedWrite(Io, g_EndpointIndex[Endpoint],
                          Index & AZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX__AZALIA_ENDPOINT_REG_INDEX_MASK);
    if (status < 0) return status;
    status = CheckedWrite(Io, g_EndpointData[Endpoint], Value);
    if (status >= 0) Io->IndirectWrites++;
    return status;
}

// ---- step 0: the observation -------------------------------------------------------------------------------------

#define SLOT_DIRECT 1u
#define SLOT_INDIRECT 2u
typedef struct _OBS_SLOT {
    unsigned long Kind;                     // 0 = not bound: a slot the table below forgot, refused at run time
    unsigned long Endpoint;                 // SLOT_INDIRECT only
    unsigned long Where;                    // BAR5 offset or indirect index
} OBS_SLOT;
#define D(slot, reg) [BC250_DPAUDIO_OBS_##slot] = { SLOT_DIRECT, 0, BC250_REG_DMU_##reg }
#define I(slot, e, ix) [BC250_DPAUDIO_OBS_##slot] = { SLOT_INDIRECT, e, BC250_AZ_IX_##ix }
static const OBS_SLOT g_ObsSlots[BC250_DPAUDIO_OBS_COUNT] = {
    D(CODEC_VENDOR_DEVICE, AZALIA_F0_CODEC_ROOT_PARAMETER_VENDOR_AND_DEVICE_ID),
    D(CODEC_REVISION, AZALIA_F0_CODEC_ROOT_PARAMETER_REVISION_ID),
    D(SUPPORTED_SIZE_RATES, AZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES),
    D(STREAM_FORMATS, AZALIA_F0_CODEC_FUNCTION_PARAMETER_STREAM_FORMATS),
    D(POWER_STATES, AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES),
    D(DC_PINSTRAPS, DC_PINSTRAPS),
    D(DTO_SOURCE, DCCG_AUDIO_DTO_SOURCE), D(DTO0_PHASE, DCCG_AUDIO_DTO0_PHASE), D(DTO0_MODULE, DCCG_AUDIO_DTO0_MODULE),
    D(DTO1_PHASE, DCCG_AUDIO_DTO1_PHASE), D(DTO1_MODULE, DCCG_AUDIO_DTO1_MODULE),
    D(DIG0_FE_CNTL, DIG0_DIG_FE_CNTL), D(DIG0_BE_CNTL, DIG0_DIG_BE_CNTL), D(DP0_VID_STREAM_CNTL, DP0_DP_VID_STREAM_CNTL),
    D(DP0_SEC_CNTL, DP0_DP_SEC_CNTL), D(DP0_SEC_AUD_N, DP0_DP_SEC_AUD_N), D(DP0_SEC_AUD_M_READBACK, DP0_DP_SEC_AUD_M_READBACK),
    D(DP0_SEC_TIMESTAMP, DP0_DP_SEC_TIMESTAMP), D(DIG0_AFMT_CNTL, DIG0_AFMT_CNTL),
    D(DIG0_AFMT_AUDIO_SRC_CONTROL, DIG0_AFMT_AUDIO_SRC_CONTROL), D(DIG0_AFMT_AUDIO_PACKET_CONTROL, DIG0_AFMT_AUDIO_PACKET_CONTROL),
    D(DIG0_AFMT_AUDIO_PACKET_CONTROL2, DIG0_AFMT_AUDIO_PACKET_CONTROL2), D(DIG0_AFMT_STATUS, DIG0_AFMT_STATUS),
    D(HPD0_INT_STATUS, HPD0_DC_HPD_INT_STATUS),
    D(DIG1_FE_CNTL, DIG1_DIG_FE_CNTL), D(DIG1_BE_CNTL, DIG1_DIG_BE_CNTL), D(DP1_VID_STREAM_CNTL, DP1_DP_VID_STREAM_CNTL),
    D(DP1_SEC_CNTL, DP1_DP_SEC_CNTL), D(DP1_SEC_AUD_N, DP1_DP_SEC_AUD_N), D(DP1_SEC_AUD_M_READBACK, DP1_DP_SEC_AUD_M_READBACK),
    D(DP1_SEC_TIMESTAMP, DP1_DP_SEC_TIMESTAMP), D(DIG1_AFMT_CNTL, DIG1_AFMT_CNTL),
    D(DIG1_AFMT_AUDIO_SRC_CONTROL, DIG1_AFMT_AUDIO_SRC_CONTROL), D(DIG1_AFMT_AUDIO_PACKET_CONTROL, DIG1_AFMT_AUDIO_PACKET_CONTROL),
    D(DIG1_AFMT_AUDIO_PACKET_CONTROL2, DIG1_AFMT_AUDIO_PACKET_CONTROL2), D(DIG1_AFMT_STATUS, DIG1_AFMT_STATUS),
    D(HPD1_INT_STATUS, HPD1_DC_HPD_INT_STATUS),
    I(EP0_CONFIG_DEFAULT, 0, RESPONSE_CONFIGURATION_DEFAULT), I(EP0_HOT_PLUG_CONTROL, 0, HOT_PLUG_CONTROL),
    I(EP0_PIN_SENSE, 0, RESPONSE_PIN_SENSE), I(EP0_UNSOLICITED_RESPONSE, 0, UNSOLICITED_RESPONSE),
    I(EP0_WIDGET_CONTROL, 0, WIDGET_CONTROL), I(EP0_CHANNEL_SPEAKER, 0, CHANNEL_SPEAKER),
    I(EP0_AUDIO_DESCRIPTOR0, 0, AUDIO_DESCRIPTOR0), I(EP0_SINK_INFO1, 0, SINK_INFO1),
    I(EP1_CONFIG_DEFAULT, 1, RESPONSE_CONFIGURATION_DEFAULT), I(EP1_HOT_PLUG_CONTROL, 1, HOT_PLUG_CONTROL),
    I(EP1_PIN_SENSE, 1, RESPONSE_PIN_SENSE), I(EP1_UNSOLICITED_RESPONSE, 1, UNSOLICITED_RESPONSE),
    I(EP1_WIDGET_CONTROL, 1, WIDGET_CONTROL), I(EP1_CHANNEL_SPEAKER, 1, CHANNEL_SPEAKER),
    I(EP1_AUDIO_DESCRIPTOR0, 1, AUDIO_DESCRIPTOR0), I(EP1_SINK_INFO1, 1, SINK_INFO1),
};
#undef D
#undef I

void Bc250DpAudioObserve(BC250_AZ_IO* Io, BC250_DPAUDIO_OBSERVATION* Obs)
{
    unsigned long i;

    for (i = 0; i < BC250_DPAUDIO_OBS_SLOTS; i++) Obs->Regs[i] = 0;
    Obs->ValidMask = 0;
    Obs->FirstFailure = BC250_AZ_STATUS_SUCCESS;
    for (i = 0; i < BC250_DPAUDIO_OBS_COUNT; i++) {
        const OBS_SLOT* slot = &g_ObsSlots[i];
        unsigned long value = 0;
        long status;

        if (slot->Kind == SLOT_DIRECT) status = Bc250AzRead(Io, slot->Where, &value);
        else if (slot->Kind == SLOT_INDIRECT) status = Bc250AzIndirectRead(Io, slot->Endpoint, slot->Where, &value);
        else status = BC250_AZ_STATUS_INVALID_PARAMETER;
        if (status >= 0) {
            Obs->Regs[i] = value;
            Obs->ValidMask |= 1ull << i;
        } else if (Obs->FirstFailure >= 0) {
            Obs->FirstFailure = status;
        }
    }
}

// ---- step 1: the decision ----------------------------------------------------------------------------------------

unsigned long Bc250DpAudioGate(int MmioMapped, unsigned long EnableDpAudio, unsigned long EnableDpAudioEndpoint)
{
    if (EnableDpAudio != 1) return BC250_DPAUDIO_REASON_SWITCH_OFF;
    if (EnableDpAudioEndpoint != 1) return BC250_DPAUDIO_REASON_ENDPOINT_SWITCH_OFF;
    if (!MmioMapped) return BC250_DPAUDIO_REASON_NO_MMIO;
    return BC250_DPAUDIO_REASON_OK;
}

#define BIT(slot) (1ull << BC250_DPAUDIO_OBS_##slot)
#define VALID(o, slot) (((o)->ValidMask & BIT(slot)) != 0)

static const unsigned long g_VidStreamSlot[2] = { BC250_DPAUDIO_OBS_DP0_VID_STREAM_CNTL, BC250_DPAUDIO_OBS_DP1_VID_STREAM_CNTL };
static const unsigned long g_VidStreamEnable[2] = { DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK,
                                                    DP1_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK };
static const unsigned long g_BeSlot[2] = { BC250_DPAUDIO_OBS_DIG0_BE_CNTL, BC250_DPAUDIO_OBS_DIG1_BE_CNTL };
static const unsigned long g_BeModeMask[2] = { DIG0_DIG_BE_CNTL__DIG_MODE_MASK, DIG1_DIG_BE_CNTL__DIG_MODE_MASK };
static const unsigned long g_BeModeShift[2] = { DIG0_DIG_BE_CNTL__DIG_MODE__SHIFT, DIG1_DIG_BE_CNTL__DIG_MODE__SHIFT };
static const unsigned long g_BeFeMask[2] = { DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT_MASK, DIG1_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT_MASK };
static const unsigned long g_BeFeShift[2] = { DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT, DIG1_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT };
static const unsigned long g_HpdSlot[2] = { BC250_DPAUDIO_OBS_HPD0_INT_STATUS, BC250_DPAUDIO_OBS_HPD1_INT_STATUS };
static const unsigned long g_HpdSense[2] = { HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK, HPD1_DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK };
static const unsigned long g_ConfigSlot[2] = { BC250_DPAUDIO_OBS_EP0_CONFIG_DEFAULT, BC250_DPAUDIO_OBS_EP1_CONFIG_DEFAULT };
static const unsigned long g_HotPlugSlot[2] = { BC250_DPAUDIO_OBS_EP0_HOT_PLUG_CONTROL, BC250_DPAUDIO_OBS_EP1_HOT_PLUG_CONTROL };
static const unsigned long g_UnsolSlot[2] = { BC250_DPAUDIO_OBS_EP0_UNSOLICITED_RESPONSE, BC250_DPAUDIO_OBS_EP1_UNSOLICITED_RESPONSE };
// DIG_MODE 0 is DP SST, 5 DP MST (dcn10_link_encoder.c:880-907 dcn10_link_encoder_setup). Endpoint presence is
// written for an SST stream only: MST would need one endpoint per stream and a different speaker allocation path.
#define DIG_MODE_DP_SST 0u

unsigned long Bc250DpAudioDecide(const BC250_DPAUDIO_OBSERVATION* Obs, BC250_DPAUDIO_PLAN* Plan)
{
    const unsigned long long required = BIT(CODEC_VENDOR_DEVICE) | BIT(DC_PINSTRAPS) | BIT(DP0_VID_STREAM_CNTL) |
        BIT(DP1_VID_STREAM_CNTL) | BIT(DIG0_BE_CNTL) | BIT(DIG1_BE_CNTL) | BIT(EP0_CONFIG_DEFAULT) |
        BIT(EP1_CONFIG_DEFAULT) | BIT(EP0_HOT_PLUG_CONTROL) | BIT(EP1_HOT_PLUG_CONTROL);
    unsigned long n, m, streams = 0, stream = 0, e;
    int sst = 0;

    Plan->Reason = BC250_DPAUDIO_REASON_OK;
    Plan->Stream = Plan->Endpoint = 0;
    Plan->Notes = 0;
    if ((Obs->ValidMask & required) != required) return Plan->Reason = BC250_DPAUDIO_REASON_READ_FAILED;
    if (Obs->Regs[BC250_DPAUDIO_OBS_CODEC_VENDOR_DEVICE] != BC250_DPAUDIO_CODEC_ID)
        return Plan->Reason = BC250_DPAUDIO_REASON_CODEC_ID;
    // dcn201_resource.c:840-845: DC_PINSTRAPS_AUDIO 0 means no audio endpoint at all (audio_support.bits.DP_AUDIO).
    if ((Obs->Regs[BC250_DPAUDIO_OBS_DC_PINSTRAPS] & DC_PINSTRAPS__DC_PINSTRAPS_AUDIO_MASK) == 0)
        return Plan->Reason = BC250_DPAUDIO_REASON_STRAPS;
    // Which stream encoder drives the monitor. The firmware enabled exactly one DP stream; the audio endpoint that
    // belongs to it is the one with the encoder's engine id (dc_resource.c:3530-3563, audio instance = engine id;
    // Linux on unit A: DIG0, DP0, endpoint 0, M820).
    for (n = 0; n < 2; n++)
        if (Obs->Regs[g_VidStreamSlot[n]] & g_VidStreamEnable[n]) { streams++; stream = n; }
    if (streams == 0) return Plan->Reason = BC250_DPAUDIO_REASON_NO_STREAM;
    if (streams > 1) return Plan->Reason = BC250_DPAUDIO_REASON_TWO_STREAMS;
    Plan->Stream = stream;
    Plan->Endpoint = e = stream;
    // The back end that carries that stream encoder, in DP SST mode. DIG_FE_SOURCE_SELECT is a bit mask of front
    // ends, DIGA = 0x1 for engine 0 (dcn10_link_encoder.c:54-61 and get_frontend_source :425-440).
    for (m = 0; m < 2; m++) {
        unsigned long be = Obs->Regs[g_BeSlot[m]];
        if ((((be & g_BeFeMask[m]) >> g_BeFeShift[m]) & (1ul << stream)) != 0 &&
            ((be & g_BeModeMask[m]) >> g_BeModeShift[m]) == DIG_MODE_DP_SST) sst = 1;
    }
    if (!sst) return Plan->Reason = BC250_DPAUDIO_REASON_NOT_DP_SST;
    // The endpoint must be the pin Linux saw (M819: both pins report 0x185600F0, port connectivity 0 = jack).
    // dce_aud_endpoint_valid (dce_audio.c:1244-1256) only asks PORT_CONNECTIVITY != 1; this asks for the whole
    // value, because a different value means a different endpoint layout or a wrong index table, and the first
    // write must not land on either. Endpoint 0 is checked as well: hw_init writes it whatever e is.
    if (Obs->Regs[g_ConfigSlot[e]] != BC250_DPAUDIO_CONFIG_DEFAULT ||
        Obs->Regs[g_ConfigSlot[0]] != BC250_DPAUDIO_CONFIG_DEFAULT)
        return Plan->Reason = BC250_DPAUDIO_REASON_CONFIG_DEFAULT;
    if (VALID(Obs, HPD0_INT_STATUS) && stream == 0 && !(Obs->Regs[g_HpdSlot[0]] & g_HpdSense[0]))
        Plan->Notes |= BC250_DPAUDIO_NOTE_HPD_LOW;
    if (VALID(Obs, HPD1_INT_STATUS) && stream == 1 && !(Obs->Regs[g_HpdSlot[1]] & g_HpdSense[1]))
        Plan->Notes |= BC250_DPAUDIO_NOTE_HPD_LOW;
    if (Obs->Regs[g_HotPlugSlot[e]] & HPC_AUDIO_ENABLED) Plan->Notes |= BC250_DPAUDIO_NOTE_INHERITED;
    if (VALID(Obs, CODEC_REVISION) && Obs->Regs[BC250_DPAUDIO_OBS_CODEC_REVISION] != BC250_DPAUDIO_CODEC_REVISION)
        Plan->Notes |= BC250_DPAUDIO_NOTE_REVISION;
    if ((Obs->ValidMask & (1ull << g_UnsolSlot[e])) && (Obs->Regs[g_UnsolSlot[e]] & AZ0(UNSOLICITED_RESPONSE, ENABLE_MASK)))
        Plan->Notes |= BC250_DPAUDIO_NOTE_UNSOLICITED;
    return Plan->Reason;
}

// ---- step 1: the write groups ------------------------------------------------------------------------------------

#define TRY(x) do { status = (x); if (status < 0) return status; } while (0)
#define IR(ix, v) TRY(Bc250AzIndirectRead(Io, Endpoint, BC250_AZ_IX_##ix, (v)))
#define IW(ix, v) do { TRY(Bc250AzIndirectWrite(Io, Endpoint, BC250_AZ_IX_##ix, (v))); Result->Writes++; } while (0)

// dce_aud_hw_init (dce_audio.c:1260-1295), endpoint 0 only, as Linux (`if (audio->inst != 0) return`):
// rates 32/44.1/48 kHz into the function group, CLKSTOP and EPSS on, inside a CLOCK_GATING_DISABLE bracket.
long Bc250DpAudioHwInit(BC250_AZ_IO* Io, BC250_DPAUDIO_RESULT* Result)
{
    const unsigned long Endpoint = 0;
    unsigned long hpc, value;
    long status;

    Result->HotPlugBefore = Result->HotPlugAfter = Result->SizeRates = Result->PowerStates = Result->Writes = 0;
    IR(HOT_PLUG_CONTROL, &hpc);
    Result->HotPlugBefore = hpc;
    hpc |= HPC_CLOCK_GATING_DISABLE;
    IW(HOT_PLUG_CONTROL, hpc);
    // R5, R6, R7 (Linux's comment): 32, 44.1 and 48 kHz.
    TRY(Bc250AzRead(Io, BC250_REG_DMU_AZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES, &value));
    value = SetField(value, AZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES__AUDIO_RATE_CAPABILITIES_MASK,
                     AZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES__AUDIO_RATE_CAPABILITIES__SHIFT, 0x70);
    TRY(Bc250AzWrite(Io, BC250_REG_DMU_AZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES, value));
    Result->Writes++;
    TRY(Bc250AzRead(Io, BC250_REG_DMU_AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES, &value));
    value = SetField(value, AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES__CLKSTOP_MASK,
                     AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES__CLKSTOP__SHIFT, 1);
    value = SetField(value, AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES__EPSS_MASK,
                     AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES__EPSS__SHIFT, 1);
    TRY(Bc250AzWrite(Io, BC250_REG_DMU_AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES, value));
    Result->Writes++;
    hpc &= ~HPC_CLOCK_GATING_DISABLE;
    IW(HOT_PLUG_CONTROL, hpc);
    Result->HotPlugAfter = hpc;
    // Not in Linux: the two read-backs the log line and the escape report.
    TRY(Bc250AzRead(Io, BC250_REG_DMU_AZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES, &Result->SizeRates));
    TRY(Bc250AzRead(Io, BC250_REG_DMU_AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES, &Result->PowerStates));
    return BC250_AZ_STATUS_SUCCESS;
}

// The fixed "basic audio" set of step 1, until step 4 reads the monitor's EDID: what every DP sink with audio must
// accept (2-channel LPCM at 32, 44.1 and 48 kHz, 16 bit). Linux's values for unit A's monitor (M810: 32 to 192 kHz,
// 16/20/24 bit) are wider; this set is a subset of them.
#define LPCM_CHANNELS 2u
#define LPCM_RATES 0x07u                    // union audio_sample_rates (dc_types.h:437-449): RATE_32, RATE_44_1, RATE_48
#define LPCM_SIZES 0x01u                    // the CTA-861 SAD byte 3 of LPCM: bit 0 = 16 bit (amdgpu_dm copies it as is)
#define SPEAKERS_FL_FR 0x01u                // struct audio_speaker_flags (dc_types.h:451-): FL_FR is bit 0
// The two halves of the port ID. Linux DC writes these constants when no Windows DM supplies a container ID
// (dc_stream.c:98-109); step 4 replaces them with DXGK_CHILD_CONTAINER_ID.EldInfo.PortId.
#define SINK_PORT_ID0 0x5558859eul
#define SINK_PORT_ID1 0x0d989449ul
static const char g_SinkName[] = "BC-250 DP";
#define SINK_NAME_MAX 18u                   // MAX_HW_AUDIO_INFO_DISPLAY_NAME_SIZE_IN_CHARS (audio_types.h:34)

static const unsigned long g_Descriptor[14] = {
    BC250_AZ_IX_AUDIO_DESCRIPTOR0, BC250_AZ_IX_AUDIO_DESCRIPTOR1, BC250_AZ_IX_AUDIO_DESCRIPTOR2, BC250_AZ_IX_AUDIO_DESCRIPTOR3,
    BC250_AZ_IX_AUDIO_DESCRIPTOR4, BC250_AZ_IX_AUDIO_DESCRIPTOR5, BC250_AZ_IX_AUDIO_DESCRIPTOR6, BC250_AZ_IX_AUDIO_DESCRIPTOR7,
    BC250_AZ_IX_AUDIO_DESCRIPTOR8, BC250_AZ_IX_AUDIO_DESCRIPTOR9, BC250_AZ_IX_AUDIO_DESCRIPTOR10, BC250_AZ_IX_AUDIO_DESCRIPTOR11,
    BC250_AZ_IX_AUDIO_DESCRIPTOR12, BC250_AZ_IX_AUDIO_DESCRIPTOR13 };
// enum audio_format_code (dc_types.h:479-499): descriptor i is format code i + 1. 1BITAUDIO (9) and DST (13) are
// skipped, not zeroed, as dce_aud_az_configure skips them.
#define FORMAT_LPCM 1u
#define FORMAT_1BITAUDIO 9u
#define FORMAT_DST 13u

static unsigned long NameChar(unsigned long i)
{
    return i < sizeof(g_SinkName) - 1 ? (unsigned long)(unsigned char)g_SinkName[i] : 0;
}

// dce_aud_az_configure (dce_audio.c:663-1022) for SIGNAL_TYPE_DISPLAY_PORT with the fixed set above.
// Deviations from Linux, each for the reason given:
//   - ACP_DATA (SUPPORTS_AI) is not written: the DCN 2.0.1 header has no index for it (Linux reaches it through
//     the DCE 11 table, index 0x27). Its value would be 0 here anyway: no EDID, no Supports_AI flag.
//   - check_audio_bandwidth is not run: the set is 2 channels at 48 kHz at most, which fits the inherited mode's
//     160 pixels of horizontal blank (design 1.4); HBR_CAPABLE is written 0, the value Linux writes when the
//     192 kHz 8-channel check fails, so no high-bit-rate format is ever offered.
long Bc250DpAudioConfigure(BC250_AZ_IO* Io, unsigned long Endpoint, BC250_DPAUDIO_RESULT* Result)
{
    unsigned long hpc, value, i, length;
    long status;

    Result->HotPlugBefore = Result->HotPlugAfter = Result->SizeRates = Result->PowerStates = Result->Writes = 0;
    IR(HOT_PLUG_CONTROL, &hpc);
    Result->HotPlugBefore = hpc;
    IW(HOT_PLUG_CONTROL, hpc | HPC_CLOCK_GATING_DISABLE);
    // Speaker allocation and connection type, in Linux's order of field updates.
    IR(CHANNEL_SPEAKER, &value);
    value = SET(value, CHANNEL_SPEAKER, SPEAKER_ALLOCATION, SPEAKERS_FL_FR);
    value = SET(value, CHANNEL_SPEAKER, LFE_PLAYBACK_LEVEL, 0);
    value = SET(value, CHANNEL_SPEAKER, HDMI_CONNECTION, 0);
    value = SET(value, CHANNEL_SPEAKER, DP_CONNECTION, 0);
    value = SET(value, CHANNEL_SPEAKER, EXTRA_CONNECTION_INFO,
                ((value & AZ0(CHANNEL_SPEAKER, EXTRA_CONNECTION_INFO_MASK)) >> AZ0(CHANNEL_SPEAKER, EXTRA_CONNECTION_INFO__SHIFT)) & ~1ul);
    value = SET(value, CHANNEL_SPEAKER, DP_CONNECTION, 1);
    IW(CHANNEL_SPEAKER, value);
    // One descriptor per format code; only LPCM is supported.
    for (i = 0; i < 14; i++) {
        unsigned long code = i + 1;
        if (code == FORMAT_1BITAUDIO || code == FORMAT_DST) continue;
        value = 0;
        if (code == FORMAT_LPCM) {
            value = SET(value, AUDIO_DESCRIPTOR0, SUPPORTED_FREQUENCIES_STEREO, LPCM_RATES);
            value = SET(value, AUDIO_DESCRIPTOR0, MAX_CHANNELS, LPCM_CHANNELS - 1);
            value = SET(value, AUDIO_DESCRIPTOR0, SUPPORTED_FREQUENCIES, LPCM_RATES);
            value = SET(value, AUDIO_DESCRIPTOR0, DESCRIPTOR_BYTE_2, LPCM_SIZES);
        }
        TRY(Bc250AzIndirectWrite(Io, Endpoint, g_Descriptor[i], value));
        Result->Writes++;
    }
    // set_high_bit_rate_capable(false), set_video_latency(0), set_audio_latency(0): DP latency is 0 in amdgpu_dm
    // (amdgpu_dm.c:6766, "for DP, video and audio latency should be calculated from DPCD caps").
    IR(RESPONSE_HBR, &value);
    IW(RESPONSE_HBR, SET(value, RESPONSE_HBR, HBR_CAPABLE, 0));
    IR(RESPONSE_LIPSYNC, &value);
    IW(RESPONSE_LIPSYNC, SET(value, RESPONSE_LIPSYNC, VIDEO_LIPSYNC, 0));
    IR(RESPONSE_LIPSYNC, &value);
    IW(RESPONSE_LIPSYNC, SET(value, RESPONSE_LIPSYNC, AUDIO_LIPSYNC, 0));
    // Manufacturer and product unknown until step 4 reads the EDID.
    IW(SINK_INFO0, 0);
    // Linux's count (dce_audio.c:886-892) includes the terminator: `while (name[len++] != '\0')` stops one past it,
    // capped at 18. Kept as Linux has it, so that the two drivers report the same length for the same name.
    length = (unsigned long)sizeof(g_SinkName);         // the characters plus the terminator
    if (length > SINK_NAME_MAX) length = SINK_NAME_MAX;
    IW(SINK_INFO1, SET(0, SINK_INFO1, SINK_DESCRIPTION_LEN, length));
    IW(SINK_INFO2, SET(0, SINK_INFO2, PORT_ID0, SINK_PORT_ID0));
    IW(SINK_INFO3, SET(0, SINK_INFO3, PORT_ID1, SINK_PORT_ID1));
    IW(SINK_INFO4, SET(SET(SET(SET(0, SINK_INFO4, DESCRIPTION0, NameChar(0)), SINK_INFO4, DESCRIPTION1, NameChar(1)),
                           SINK_INFO4, DESCRIPTION2, NameChar(2)), SINK_INFO4, DESCRIPTION3, NameChar(3)));
    IW(SINK_INFO5, SET(SET(SET(SET(0, SINK_INFO5, DESCRIPTION4, NameChar(4)), SINK_INFO5, DESCRIPTION5, NameChar(5)),
                           SINK_INFO5, DESCRIPTION6, NameChar(6)), SINK_INFO5, DESCRIPTION7, NameChar(7)));
    IW(SINK_INFO6, SET(SET(SET(SET(0, SINK_INFO6, DESCRIPTION8, NameChar(8)), SINK_INFO6, DESCRIPTION9, NameChar(9)),
                           SINK_INFO6, DESCRIPTION10, NameChar(10)), SINK_INFO6, DESCRIPTION11, NameChar(11)));
    IW(SINK_INFO7, SET(SET(SET(SET(0, SINK_INFO7, DESCRIPTION12, NameChar(12)), SINK_INFO7, DESCRIPTION13, NameChar(13)),
                           SINK_INFO7, DESCRIPTION14, NameChar(14)), SINK_INFO7, DESCRIPTION15, NameChar(15)));
    IW(SINK_INFO8, SET(SET(0, SINK_INFO8, DESCRIPTION16, NameChar(16)), SINK_INFO8, DESCRIPTION17, NameChar(17)));
    IR(HOT_PLUG_CONTROL, &hpc);
    hpc &= ~HPC_CLOCK_GATING_DISABLE;
    IW(HOT_PLUG_CONTROL, hpc);
    Result->HotPlugAfter = hpc;
    return BC250_AZ_STATUS_SUCCESS;
}

// dce_aud_az_enable (dce_audio.c:611-631) and dce_aud_az_disable (:638-661). The read-back at the end is Linux's in
// the disable path and an addition in the enable path, so that both report what the register holds afterwards.
long Bc250DpAudioSetEnabled(BC250_AZ_IO* Io, unsigned long Endpoint, int Enable, BC250_DPAUDIO_RESULT* Result)
{
    unsigned long hpc;
    long status;

    Result->HotPlugBefore = Result->HotPlugAfter = Result->SizeRates = Result->PowerStates = Result->Writes = 0;
    IR(HOT_PLUG_CONTROL, &hpc);
    Result->HotPlugBefore = hpc;
    if (Enable) {
        hpc |= HPC_CLOCK_GATING_DISABLE | HPC_AUDIO_ENABLED;
        IW(HOT_PLUG_CONTROL, hpc);
        hpc &= ~HPC_CLOCK_GATING_DISABLE;
        IW(HOT_PLUG_CONTROL, hpc);
    } else {
        hpc |= HPC_CLOCK_GATING_DISABLE;
        IW(HOT_PLUG_CONTROL, hpc);
        hpc &= ~HPC_AUDIO_ENABLED;
        IW(HOT_PLUG_CONTROL, hpc);
        hpc &= ~HPC_CLOCK_GATING_DISABLE;
        IW(HOT_PLUG_CONTROL, hpc);
    }
    IR(HOT_PLUG_CONTROL, &Result->HotPlugAfter);
    return BC250_AZ_STATUS_SUCCESS;
}

unsigned long Bc250DpAudioRun(BC250_AZ_IO* Io, BC250_DPAUDIO_RUN* Run)
{
    unsigned long reason;
    long status;

    Run->Groups = Run->Wrote = 0;
    Run->Status = Run->ClearStatus = BC250_AZ_STATUS_SUCCESS;
    Run->Init.Writes = Run->Config.Writes = Run->Enable.Writes = Run->Clear.Writes = 0;
    Bc250DpAudioObserve(Io, &Run->Obs);
    reason = Bc250DpAudioDecide(&Run->Obs, &Run->Plan);
    if (reason != BC250_DPAUDIO_REASON_OK) return reason;
    Run->Wrote = 1;
    status = Bc250DpAudioHwInit(Io, &Run->Init);
    if (status >= 0) { Run->Groups = 1; status = Bc250DpAudioConfigure(Io, Run->Plan.Endpoint, &Run->Config); }
    if (status >= 0) { Run->Groups = 2; status = Bc250DpAudioSetEnabled(Io, Run->Plan.Endpoint, 1, &Run->Enable); }
    if (status >= 0) { Run->Groups = 3; return BC250_DPAUDIO_REASON_OK; }
    // Leave silence behind, whatever group failed: dce_aud_az_disable on the chosen endpoint.
    Run->Status = status;
    Run->ClearStatus = Bc250DpAudioSetEnabled(Io, Run->Plan.Endpoint, 0, &Run->Clear);
    return BC250_DPAUDIO_REASON_WRITE_FAILED;
}

#define BC250_DPAUDIO_REASON_TEXT(n, t) t,
static const char* const g_ReasonText[BC250_DPAUDIO_REASON_COUNT] = { BC250_DPAUDIO_REASON_LIST(BC250_DPAUDIO_REASON_TEXT) };

const char* Bc250DpAudioReasonText(unsigned long Reason)
{
    return Reason < BC250_DPAUDIO_REASON_COUNT ? g_ReasonText[Reason] : "unknown reason";
}
